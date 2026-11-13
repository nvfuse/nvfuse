#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_backend.h"
#include "nvfuse_mkfs.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_tx_internal.h"
#ifdef NVFUSE_SPDK_TRANSPORT
#include "nvfuse_nvme_transport.h"
#endif
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#define IMAGE_GROUP_BYTES (128ULL * 1024 * 1024)
#define META_IMAGE_SIGNATURE UINT32_C(0x4d46564e)
static int active_handle;

/* Core callers assume successful block I/O. Fail loudly rather than allowing
 * an I/O error to be mistaken for a successfully persisted metadata update. */
static int raw_transfer(struct io_target *target, uint64_t offset, size_t bytes,
                        void *buffer, int write_op)
{
    if (offset > target->size_bytes || bytes > target->size_bytes - offset) {
        fprintf(stderr, "file backend: I/O beyond image boundary\n");
        abort();
    }
#ifdef NVFUSE_SPDK_TRANSPORT
    if (target->spdk_transport) {
        int rc = nvfuse_nvme_transfer(target->spdk_transport, offset, bytes, buffer, write_op);
        if (rc) return rc;
        return 0;
    }
#endif
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = write_op ? pwrite(target->fd, (char *)buffer + done, bytes - done, offset + done)
                             : pread(target->fd, (char *)buffer + done, bytes - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (!n) errno = EIO;
            return -errno;
        }
        done += (size_t)n;
    }
    return 0;
}
static int file_transfer(struct io_target *target, uint64_t offset, size_t bytes,
                         void *buffer, int write_op)
{
    if (offset > target->size_bytes || bytes > target->size_bytes - offset) {
        fprintf(stderr, "file backend: I/O beyond image boundary\n");
        abort();
    }
    if (write_op && target->tx_rollback) return 0;
    void *transfer_buffer = buffer;
    void *copy = NULL;
    unsigned char prefix[CLUSTER_SIZE];
    if (write_op && offset == 0 && bytes >= sizeof(uint32_t) &&
        (target->metadata_only || target->journal)) {
        copy = bytes<=sizeof(prefix)?prefix:malloc(bytes);
        if (!copy) abort();
        memcpy(copy, buffer, bytes);
        uint32_t signature = target->journal ?
            (target->metadata_only ? NVJ_META_SIGNATURE : NVJ_SIGNATURE) : META_IMAGE_SIGNATURE;
        memcpy(copy, &signature, sizeof(signature));
        transfer_buffer = copy;
    }
    int rc = target->journal ? nvj_transfer(target->journal, offset, bytes, transfer_buffer, write_op)
                             : raw_transfer(target, offset, bytes, transfer_buffer, write_op);
    if(copy && copy!=prefix) free(copy);
    /* Legacy callers cannot unwind a failed cache operation. A soft TX budget
     * error is surfaced by the operation wrapper after cache staging completes. */
    if (rc) { fprintf(stderr, "file backend I/O transfer error: %d\n", rc); abort(); }
    if (!write_op && offset == 0 && bytes >= sizeof(uint32_t) &&
        (target->metadata_only || target->journal)) {
        uint32_t signature = NVFUSE_SB_SIGNATURE;
        memcpy(buffer, &signature, sizeof(signature));
        if (target->journal && bytes >= sizeof(struct nvfuse_superblock_common)) {
            struct nvfuse_superblock_common *disk = buffer;
            if (disk->sb_state == FS_STATE_MOUNTED || disk->sb_state == FS_STATE_CRASHED)
                disk->sb_state = FS_STATE_UMOUNTED;
        }
    }
    return 0;
}
int reactor_sync_read_blk(struct io_target *t, long block, int count, void *buf)
{
    return file_transfer(t, (uint64_t)block * CLUSTER_SIZE, (size_t)count * CLUSTER_SIZE, buf, 0);
}
int reactor_sync_write_blk(struct io_target *t, long block, int count, void *buf)
{
    return file_transfer(t, (uint64_t)block * CLUSTER_SIZE, (size_t)count * CLUSTER_SIZE, buf, 1);
}
static int raw_flush(struct io_target *t)
{
#ifdef NVFUSE_SPDK_TRANSPORT
    if (t->spdk_transport) {
        int rc = nvfuse_nvme_flush(t->spdk_transport);
        if (rc) return rc;
        return 0;
    }
#endif
    int ret;
    do { ret = fsync(t->fd); } while (ret < 0 && errno == EINTR);
    if (ret) return -errno;
    return 0;
}

int reactor_sync_flush(struct io_target *t)
{
    /* The outer commit supplies the ordered-data barrier. Never commit a user
     * transaction as a side effect of cache eviction or an internal helper. */
    if (t->tx_rollback || (t->journal && nvj_active(t->journal))) return 0;
    return raw_flush(t);
}
static int journal_transfer(void *t, uint64_t off, size_t n, void *buf, int write_op)
{
    return raw_transfer(t, off, n, buf, write_op);
}
static int journal_flush(void *t) { return raw_flush(t); }
static struct nvj_ops journal_ops(struct io_target *t)
{
    return (struct nvj_ops){journal_transfer, journal_flush, t};
}
static int format_journal(struct io_target *t)
{
    unsigned char block[CLUSTER_SIZE];
    int rc;
    unsigned journal_blocks=t->journal_blocks?t->journal_blocks:NVJ_BLOCKS;
    if(journal_blocks!=NVJ_BLOCKS && journal_blocks!=8192u && journal_blocks!=16384u) return -EINVAL;
    unsigned journal_start=NVJ_GROUP_BLOCKS-journal_blocks;
    if ((rc = raw_transfer(t, CLUSTER_SIZE, sizeof(block), block, 0))) return rc;
    struct nvfuse_bg_descriptor *bd = (void *)block;
    uint32_t bitmap_block = bd->bd_dbitmap_start;
    if (journal_start < bd->bd_dtable_start) return -EINVAL;
    bd->bd_free_blocks -= journal_blocks;
    if ((rc = raw_transfer(t, CLUSTER_SIZE, sizeof(block), block, 1))) return rc;
    if ((rc = raw_transfer(t, (uint64_t)bitmap_block * CLUSTER_SIZE, sizeof(block), block, 0))) return rc;
    for (unsigned i=journal_start; i<journal_start+journal_blocks; i++) block[i/8] |= 1u << (i%8);
    if ((rc = raw_transfer(t, (uint64_t)bitmap_block * CLUSTER_SIZE, sizeof(block), block, 1))) return rc;
    if ((rc = raw_transfer(t, 0, sizeof(block), block, 0))) return rc;
    struct nvfuse_superblock_common *disk = (void *)block;
    disk->sb_free_blocks -= journal_blocks;
    disk->sb_signature = t->metadata_only ? NVJ_META_SIGNATURE : NVJ_SIGNATURE;
    if ((rc = raw_transfer(t, 0, sizeof(block), block, 1))) return rc;
    struct nvj_ops ops = journal_ops(t);
    return nvj_format_sized(&ops,t->size_bytes,t->metadata_only,journal_blocks);
}

/* Synchronous execution with explicit completions preserves the cache's batch
 * writeback contract, without emulating SPDK's event loops or worker cores. */
struct reactor_task *reactor_alloc_task(struct io_target *t, int32_t depth)
{
    if (depth < 1 || depth >= REACTOR_MAX_REQUEST) return NULL;
    struct reactor_task *task = calloc(1, sizeof(*task));
    if (task) { task->target = t; task->qdepth = depth + 1; }
    return task;
}
void reactor_free_task(struct io_target *t, struct reactor_task *task) { (void)t; free(task); }
int reactor_cq_size(struct reactor_task *task)
{
    return (task->cq_head - task->cq_tail + task->qdepth) % task->qdepth;
}
int32_t reactor_submit_reqs(struct io_target *t, struct reactor_task *task, struct io_job **reqs, int count)
{
    if (!t || !task || task->target != t || (count && !reqs)) return -EINVAL;
    if (count < 0 || count > task->qdepth - 1 - reactor_cq_size(task)) return -EINVAL;
    for (int i = 0; i < count; i++) {
        if (!reqs[i]) return -EINVAL;
        if (reqs[i]->req_type != SPDK_BDEV_IO_TYPE_FLUSH &&
            reqs[i]->req_type != SPDK_BDEV_IO_TYPE_READ &&
            reqs[i]->req_type != SPDK_BDEV_IO_TYPE_WRITE) return -EINVAL;
        if (reqs[i]->req_type != SPDK_BDEV_IO_TYPE_FLUSH &&
            (!reqs[i]->buf || reqs[i]->offset < 0 || reqs[i]->bytes <= 0 ||
             (uint64_t)reqs[i]->offset > t->size_bytes ||
             (uint64_t)reqs[i]->bytes > t->size_bytes - reqs[i]->offset)) return -EINVAL;
    }
    for (int i = 0; i < count; i++) {
        struct io_job *req = reqs[i];
        req->task = task;
        if (req->req_type == SPDK_BDEV_IO_TYPE_FLUSH) reactor_sync_flush(t);
        else if (req->req_type == SPDK_BDEV_IO_TYPE_READ || req->req_type == SPDK_BDEV_IO_TYPE_WRITE)
            file_transfer(t, req->offset, req->bytes, req->buf, req->req_type == SPDK_BDEV_IO_TYPE_WRITE);
        else return -EINVAL;
        req->ret = 0; req->complete = 1;
        task->cq[task->cq_head] = req;
        task->cq_head = (task->cq_head + 1) % task->qdepth;
    }
    return 0;
}
int reactor_cq_get_reqs(struct reactor_task *task, struct io_job **reqs, int min, int max)
{
    int count = reactor_cq_size(task);
    if (min < 0 || max < min || count < min) return -EINVAL;
    if (count > max) count = max;
    for (int i = 0; i < count; i++) {
        reqs[i] = task->cq[task->cq_tail];
        task->cq[task->cq_tail] = NULL;
        task->cq_tail = (task->cq_tail + 1) % task->qdepth;
    }
    return count;
}
void reactor_bio_cb(struct spdk_bdev_io *io, bool ok, void *arg)
{
    (void)io; (void)ok; (void)arg;
    /* File completions are posted by reactor_submit_reqs, never by SPDK. */
    abort();
}

static int read_disk_header(struct io_target *target, struct nvfuse_superblock_common *disk)
{
#ifdef NVFUSE_SPDK_TRANSPORT
    if (target->spdk_transport)
        return nvfuse_nvme_transfer(target->spdk_transport, 0, sizeof(*disk), disk, 0);
#endif
    return pread(target->fd, disk, sizeof(*disk), 0) == sizeof(*disk) ? 0 : -1;
}
static int mount_target(struct nvfuse_handle *nvh, int create)
{
    struct io_target *target = nvh->nvh_target;
    uint64_t size = target->size_bytes;
    int metadata_only = target->metadata_only;
    target->tx_thread = pthread_self();
    nvh->blk_size = SECTOR_SIZE;
    nvh->total_blkcount = size / SECTOR_SIZE;
    nvh->nvh_params.buffer_size = 16;
    nvh->nvh_params.qdepth = 32;
    nvh->nvh_params.need_mount = 1;
    if (create && nvfuse_format(nvh)) return -1;
    if (create && format_journal(target)) return -1;
    /* Refuse malformed/unclean images before allocating mount resources. */
    struct nvfuse_superblock_common disk;
    if (read_disk_header(target, &disk)) { errno = EINVAL; return -1; }
    if (!target->journal) {
        struct nvj_ops ops = journal_ops(target);
        int rc = nvj_open(&target->journal, &ops, size, metadata_only);
        if (rc && disk.sb_signature != NVFUSE_SB_SIGNATURE && disk.sb_signature != META_IMAGE_SIGNATURE) {
            errno = -rc; return -1;
        }
        /* New-format images are never downgraded to clean-only legacy mode. */
        if (!rc) read_disk_header(target, &disk);
    }
    uint32_t expected = target->journal ? (metadata_only ? NVJ_META_SIGNATURE : NVJ_SIGNATURE) :
                                        (metadata_only ? META_IMAGE_SIGNATURE : NVFUSE_SB_SIGNATURE);
    if (
        disk.sb_signature != expected ||
        disk.sb_no_of_sectors != (int64_t)nvh->total_blkcount ||
        disk.sb_no_of_blocks != (int64_t)(size / CLUSTER_SIZE) ||
        disk.sb_no_of_blocks_per_bg != IMAGE_GROUP_BYTES / CLUSTER_SIZE ||
        disk.sb_bg_num != (int64_t)(size / IMAGE_GROUP_BYTES) ||
        disk.sb_root_ino != ROOT_INO ||
        (disk.sb_state != FS_STATE_FORMATTED && disk.sb_state != FS_STATE_UMOUNTED &&
         !(target->journal && disk.sb_state == FS_STATE_MOUNTED))) {
        errno = EINVAL; return -1;
    }
    if (target->journal) {
        nvj_mark_filesystem_metadata(target->journal);
        if (nvj_begin(target->journal, 0, 0)) return -1;
    }
    /* Existing core statistics use process-local rings and lazy pools. */
    if (nvfuse_ipc_init(&nvh->nvh_ipc_ctx)) return -1;
    nvh->nvh_sb.sb_nvh = nvh;
    if (nvfuse_mount(nvh)) {
        fprintf(stderr, "file backend: mount initialization failed\n");
        /* The legacy core has no partial-mount unwind contract. */
        abort();
    }
    target->tx_mounted = 1;
    if (target->journal) {
        nvfuse_check_flush_dirty(&nvh->nvh_sb, DIRTY_FLUSH_FORCE);
        int sync_error=nvfuse_sync_superblock(&nvh->nvh_sb);
        if(sync_error) nvj_fail(target->journal,sync_error<0?sync_error:-EIO);
        int rc = nvj_commit(target->journal);
        if (rc) {
            target->tx_rollback = 1;
            nvfuse_umount(nvh);
            nvfuse_ipc_exit(&nvh->nvh_ipc_ctx);
            rte_ring_free(nvh->nvh_ipc_ctx.id_gen);
            target->tx_rollback = target->tx_mounted = 0;
            errno = -rc; return -1;
        }
    }
    if (target->journal && !target->tx_restoring && nvfuse_orphan_reap(nvh)) {
        nvj_require_recovery(target->journal); target->tx_rollback=1;
        nvfuse_umount(nvh); nvfuse_ipc_exit(&nvh->nvh_ipc_ctx);
        rte_ring_free(nvh->nvh_ipc_ctx.id_gen);
        target->tx_rollback=target->tx_mounted=0; errno=EIO; return -1;
    }
    active_handle = 1;
    return 0;
}

struct nvfuse_handle *nvfuse_file_open(const char *path, int create, uint64_t size)
{
    return nvfuse_file_open_mode(path, create, size, 0);
}

struct nvfuse_handle *nvfuse_file_open_mode(const char *path,int create,uint64_t size,int metadata_only) {
    return nvfuse_file_open_config(path,create,size,metadata_only,NVJ_BLOCKS);
}
struct nvfuse_handle *nvfuse_file_open_config(const char *path,int create,uint64_t size,
                                            int metadata_only,unsigned journal_blocks)
{
    if (create && journal_blocks!=NVJ_BLOCKS && journal_blocks!=8192u && journal_blocks!=16384u) { errno=EINVAL; return NULL; }
    if (metadata_only != 0 && metadata_only != 1) { errno = EINVAL; return NULL; }
    if (active_handle) { errno = EBUSY; return NULL; }
    if (create && (size < IMAGE_GROUP_BYTES || size % IMAGE_GROUP_BYTES ||
                   size / CLUSTER_SIZE > UINT32_MAX)) {
        errno = EINVAL; return NULL;
    }
    int flags = O_RDWR | O_NOFOLLOW | O_NONBLOCK;
    if (create) flags |= O_CREAT | O_EXCL;
    int fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st)) goto fail_fd;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto fail_fd; }
    if (flock(fd, LOCK_EX | LOCK_NB)) goto fail_fd;
    if (create && ftruncate(fd, (off_t)size)) goto fail_fd;
    if (!create) size = st.st_size;
    if (size < IMAGE_GROUP_BYTES || size % IMAGE_GROUP_BYTES || size / CLUSTER_SIZE > UINT32_MAX) {
        errno = EINVAL; goto fail_fd;
    }
    struct nvfuse_handle *nvh = calloc(1, sizeof(*nvh));
    struct io_target *target = calloc(1, sizeof(*target));
    if (!nvh || !target) { free(nvh); free(target); goto fail_fd; }
    target->fd = fd; target->size_bytes = size;
    target->metadata_only = metadata_only;
    target->journal_blocks = journal_blocks;
    nvh->nvh_target = target;
    if (mount_target(nvh, create)) goto fail_handle;
    return nvh;
fail_handle:
    nvj_close(target->journal);
    free(target); free(nvh);
fail_fd:;
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return NULL;
}

int nvfuse_file_close(struct nvfuse_handle *nvh)
{
    if (nvh->nvh_target->journal && !pthread_equal(nvh->nvh_target->tx_thread, pthread_self())) return -EPERM;
    if(nvh->nvh_target->tx_queues) return -EBUSY;
    int failed = nvj_recovery_required(nvh->nvh_target->journal);
    if (!failed && nvh->nvh_target->tx_user) return -EBUSY;
    if (!failed && nvh->nvh_target->journal) {
        for(unsigned i=START_OPEN_FILE;i<MAX_OPEN_FILE;i++)
            nvh->nvh_sb.sb_file_table[i].used=0;
        if(nvfuse_orphan_reap(nvh)) { nvj_require_recovery(nvh->nvh_target->journal); failed=1; }
    }
    if (failed) nvh->nvh_target->tx_rollback = 1;
    if (!failed && nvh->nvh_target->journal && nvj_begin(nvh->nvh_target->journal, 0, 0)) return -EIO;
    int mounted = nvh->nvh_target->tx_mounted;
    int ret = mounted ? nvfuse_umount(nvh) : 0;
    if (ret) return ret;
    nvh->nvh_target->tx_mounted = 0;
    if (!failed && nvh->nvh_target->journal) {
        ret = nvj_commit(nvh->nvh_target->journal);
        if (ret) failed = 1;
    }
    if (!failed) reactor_sync_flush(nvh->nvh_target);
    int fd = nvh->nvh_target->fd;
    /* Keep the handle alive until IPC teardown is complete. */
    if (mounted) {
        nvfuse_ipc_exit(&nvh->nvh_ipc_ctx);
        rte_ring_free(nvh->nvh_ipc_ctx.id_gen);
    }
    int result;
#ifdef NVFUSE_SPDK_TRANSPORT
    if (nvh->nvh_target->spdk_transport)
        result = nvfuse_nvme_close(nvh->nvh_target->spdk_transport);
    else
#endif
        result = close(fd);
    nvj_close(nvh->nvh_target->journal);
    free(nvh->nvh_target->tx_snapshot);
    free(nvh->nvh_target); free(nvh);
    active_handle = 0;
    return failed ? -EIO : result;
}

int nvfuse_file_reload(struct nvfuse_handle *h)
{
    struct io_target *t = h->nvh_target;
    t->tx_rollback = 1;
    int rc = nvfuse_umount(h);
    nvfuse_ipc_exit(&h->nvh_ipc_ctx);
    rte_ring_free(h->nvh_ipc_ctx.id_gen);
    t->tx_mounted = 0;
    nvj_discard(t->journal);
    t->tx_rollback = t->tx_user = t->tx_depth = t->tx_keyed = 0;
    if (rc) { nvj_require_recovery(t->journal); return rc; }
    memset(h, 0, sizeof(*h));
    h->nvh_target = t;
    t->tx_restoring=1;
    int mount_error=mount_target(h,0); t->tx_restoring=0;
    if (mount_error) { nvj_require_recovery(t->journal); return -EIO; }
    return 0;
}

/* Device geometry discovery is shared by kernel and SPDK transports. It stays
 * outside the I/O hot path and retains mount_target's journal/recovery logic. */
#if defined(__linux__) || defined(NVFUSE_SPDK_TRANSPORT)
static int mount_device(struct nvfuse_handle *h, uint64_t capacity, int format,
                        uint64_t logical_size, int metadata_only)
{
    struct io_target *t = h->nvh_target;
    t->size_bytes = capacity;
    if (format && !logical_size)
        logical_size = capacity - capacity % IMAGE_GROUP_BYTES;
    if (!format) {
        struct nvj_ops ops = journal_ops(t);
        int probe = nvj_probe(&ops, capacity, &logical_size, metadata_only);
        if (probe && probe != -ENOENT) { errno = -probe; return -1; }
        if (probe == -ENOENT) {
            struct nvfuse_superblock_common disk;
            if (read_disk_header(t, &disk) || disk.sb_no_of_blocks <= 0 ||
                disk.sb_no_of_blocks > (int64_t)(capacity / CLUSTER_SIZE)) {
                errno = EINVAL; return -1;
            }
            logical_size = disk.sb_no_of_blocks * CLUSTER_SIZE;
        }
    }
    if (logical_size > capacity || logical_size < IMAGE_GROUP_BYTES ||
        logical_size % IMAGE_GROUP_BYTES || logical_size / CLUSTER_SIZE > INT32_MAX) {
        errno = EINVAL; return -1;
    }
    t->size_bytes = logical_size;
    t->metadata_only = metadata_only;
    return mount_target(h, format);
}
#endif

#ifdef NVFUSE_SPDK_TRANSPORT
static struct nvfuse_handle *nvme_fs_open(const char *bdf, uint32_t nsid,
                                         int format, uint64_t size,
                                         int metadata_only, int fixture_only)
{
    if (active_handle) { errno = EBUSY; return NULL; }
    if (!bdf || (metadata_only != 0 && metadata_only != 1) ||
        (format && (size < IMAGE_GROUP_BYTES || size % IMAGE_GROUP_BYTES))) {
        errno = EINVAL; return NULL;
    }
    struct nvfuse_handle *h = calloc(1, sizeof(*h));
    struct io_target *t = calloc(1, sizeof(*t));
    if (!h || !t) { free(h); free(t); errno = ENOMEM; return NULL; }
    t->fd = -1;
    h->nvh_target = t;
    uint64_t capacity;
    int rc = fixture_only ? nvfuse_nvme_open(bdf, &t->spdk_transport, &capacity) :
                           nvfuse_nvme_open_namespace(bdf, nsid, &t->spdk_transport, &capacity);
    if (rc) { errno = -rc; goto fail; }
    if (mount_device(h, capacity, format, size, metadata_only)) goto fail;
    return h;
fail:;
    int saved = errno;
    nvfuse_nvme_close(t->spdk_transport);
    nvj_close(t->journal);
    free(t); free(h); errno = saved;
    return NULL;
}

/* Preserve the old test API's exact disposable-device restriction. */
struct nvfuse_handle *nvfuse_nvme_fs_open(const char *bdf, int format,
                                        uint64_t logical_size, int metadata_only)
{
    return nvme_fs_open(bdf, 1, format, logical_size, metadata_only, 1);
}
#endif

#ifdef __linux__
static struct nvfuse_handle *kernel_fs_open(const struct nvfuse_backend_options *options)
{
    if (active_handle) { errno = EBUSY; return NULL; }
    /* O_EXCL claims a block device in the kernel, including partition/holder
     * conflicts. An advisory file lock alone cannot exclude a mounted FS. */
    int fd = open(options->target, O_RDWR | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    uint64_t capacity;
    if (fstat(fd, &st)) goto fail_fd;
    if (!S_ISBLK(st.st_mode)) { errno = ENOTBLK; goto fail_fd; }
    if (ioctl(fd, BLKGETSIZE64, &capacity)) goto fail_fd;
    struct nvfuse_handle *h = calloc(1, sizeof(*h));
    struct io_target *t = calloc(1, sizeof(*t));
    if (!h || !t) { free(h); free(t); errno = ENOMEM; goto fail_fd; }
    t->fd = fd;
    h->nvh_target = t;
    if (!mount_device(h, capacity, options->format, options->size_bytes, 0)) return h;
    nvj_close(t->journal);
    free(t); free(h);
fail_fd:;
    int saved = errno;
    close(fd);
    errno = saved;
    return NULL;
}
#endif

int nvfuse_backend_supported(enum nvfuse_backend_kind kind)
{
    switch (kind) {
    case NVFUSE_BACKEND_IMAGEFILE: return 1;
#ifdef __linux__
    case NVFUSE_BACKEND_KERNEL: return 1;
#endif
#ifdef NVFUSE_SPDK_TRANSPORT
    case NVFUSE_BACKEND_SPDK: return 1;
#endif
    default: return 0;
    }
}

struct nvfuse_handle *nvfuse_backend_open(const struct nvfuse_backend_options *o)
{
    if (!o || !o->target || !o->target[0] || (o->format != 0 && o->format != 1) ||
        (o->kind != NVFUSE_BACKEND_SPDK && o->namespace_id) ||
        (!o->format && o->size_bytes) ||
        (o->format && (o->size_bytes || o->kind == NVFUSE_BACKEND_IMAGEFILE) &&
                      (o->size_bytes < IMAGE_GROUP_BYTES ||
                       o->size_bytes % IMAGE_GROUP_BYTES ||
                       o->size_bytes / CLUSTER_SIZE > UINT32_MAX))) {
        errno = EINVAL; return NULL;
    }
    if (o->kind < NVFUSE_BACKEND_IMAGEFILE || o->kind > NVFUSE_BACKEND_SPDK) {
        errno = EINVAL; return NULL;
    }
    if (!nvfuse_backend_supported(o->kind)) { errno = ENOTSUP; return NULL; }
    switch (o->kind) {
    case NVFUSE_BACKEND_IMAGEFILE:
        return nvfuse_file_open(o->target, o->format, o->size_bytes);
#ifdef __linux__
    case NVFUSE_BACKEND_KERNEL:
        return kernel_fs_open(o);
#endif
#ifdef NVFUSE_SPDK_TRANSPORT
    case NVFUSE_BACKEND_SPDK:
        return nvme_fs_open(o->target, o->namespace_id ? o->namespace_id : 1,
                            o->format, o->size_bytes, 0, 0);
#endif
    default: errno = ENOTSUP; return NULL;
    }
}

int nvfuse_backend_close(struct nvfuse_handle *h)
{
    return h ? nvfuse_file_close(h) : -EINVAL;
}
