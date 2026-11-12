#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_mkfs.h"
#include "nvfuse_ipc_ring.h"
#ifdef NVFUSE_SPDK_TRANSPORT
#include "nvfuse_nvme_transport.h"
#endif
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

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
        if (rc) { fprintf(stderr, "NVFUSE SPDK: block I/O failed: %d\n", rc); abort(); }
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
            perror("file backend I/O"); abort();
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
    if (write_op && target->metadata_only && offset < sizeof(uint32_t)) {
        uint32_t signature = META_IMAGE_SIGNATURE;
        size_t prefix = sizeof(signature) - offset;
        if (prefix > bytes) prefix = bytes;
        raw_transfer(target, offset, prefix, (char *)&signature + offset, 1);
        return raw_transfer(target, offset + prefix, bytes - prefix, (char *)buffer + prefix, 1);
    }
    raw_transfer(target, offset, bytes, buffer, write_op);
    if (!write_op && target->metadata_only && offset == 0 && bytes >= sizeof(uint32_t)) {
        uint32_t signature = NVFUSE_SB_SIGNATURE;
        memcpy(buffer, &signature, sizeof(signature));
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
int reactor_sync_flush(struct io_target *t)
{
#ifdef NVFUSE_SPDK_TRANSPORT
    if (t->spdk_transport) {
        int rc = nvfuse_nvme_flush(t->spdk_transport);
        if (rc) { fprintf(stderr, "NVFUSE SPDK: flush failed: %d\n", rc); abort(); }
        return 0;
    }
#endif
    int ret;
    do { ret = fsync(t->fd); } while (ret < 0 && errno == EINTR);
    if (ret) { perror("file backend fsync"); abort(); }
    return 0;
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
    nvh->blk_size = SECTOR_SIZE;
    nvh->total_blkcount = size / SECTOR_SIZE;
    nvh->nvh_params.buffer_size = 16;
    nvh->nvh_params.qdepth = 32;
    nvh->nvh_params.need_mount = 1;
    if (create && nvfuse_format(nvh)) return -1;
    /* Refuse malformed/unclean images before allocating mount resources. */
    struct nvfuse_superblock_common disk;
    if (read_disk_header(target, &disk) ||
        disk.sb_signature != (metadata_only ? META_IMAGE_SIGNATURE : NVFUSE_SB_SIGNATURE) ||
        disk.sb_no_of_sectors != (int64_t)nvh->total_blkcount ||
        disk.sb_no_of_blocks != (int64_t)(size / CLUSTER_SIZE) ||
        disk.sb_no_of_blocks_per_bg != IMAGE_GROUP_BYTES / CLUSTER_SIZE ||
        disk.sb_bg_num != (int64_t)(size / IMAGE_GROUP_BYTES) ||
        disk.sb_root_ino != ROOT_INO ||
        (disk.sb_state != FS_STATE_FORMATTED && disk.sb_state != FS_STATE_UMOUNTED)) {
        errno = EINVAL; return -1;
    }
    /* Existing core statistics use process-local rings and lazy pools. */
    if (nvfuse_ipc_init(&nvh->nvh_ipc_ctx)) return -1;
    nvh->nvh_sb.sb_nvh = nvh;
    if (nvfuse_mount(nvh)) {
        fprintf(stderr, "file backend: mount initialization failed\n");
        /* The legacy core has no partial-mount unwind contract. */
        abort();
    }
    active_handle = 1;
    return 0;
}

struct nvfuse_handle *nvfuse_file_open(const char *path, int create, uint64_t size)
{
    return nvfuse_file_open_mode(path, create, size, 0);
}

struct nvfuse_handle *nvfuse_file_open_mode(const char *path, int create, uint64_t size,
                                         int metadata_only)
{
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
    nvh->nvh_target = target;
    if (mount_target(nvh, create)) goto fail_handle;
    return nvh;
fail_handle:
    free(target); free(nvh);
fail_fd:;
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return NULL;
}

int nvfuse_file_close(struct nvfuse_handle *nvh)
{
    int ret = nvfuse_umount(nvh);
    if (ret) return ret;
    reactor_sync_flush(nvh->nvh_target);
    int fd = nvh->nvh_target->fd;
    /* Keep the handle alive until IPC teardown is complete. */
    nvfuse_ipc_exit(&nvh->nvh_ipc_ctx);
    rte_ring_free(nvh->nvh_ipc_ctx.id_gen);
    int result;
#ifdef NVFUSE_SPDK_TRANSPORT
    if (nvh->nvh_target->spdk_transport)
        result = nvfuse_nvme_close(nvh->nvh_target->spdk_transport);
    else
#endif
        result = close(fd);
    free(nvh->nvh_target); free(nvh);
    active_handle = 0;
    return result;
}

#ifdef NVFUSE_SPDK_TRANSPORT
struct nvfuse_handle *nvfuse_nvme_fs_open(const char *bdf, int format,
                                        uint64_t logical_size, int metadata_only)
{
    if (active_handle) { errno = EBUSY; return NULL; }
    if ((metadata_only != 0 && metadata_only != 1) ||
        (format && (logical_size < IMAGE_GROUP_BYTES || logical_size % IMAGE_GROUP_BYTES))) {
        errno = EINVAL; return NULL;
    }
    struct nvfuse_handle *h = calloc(1, sizeof(*h));
    struct io_target *t = calloc(1, sizeof(*t));
    if (!h || !t) { free(h); free(t); return NULL; }
    t->fd = -1;
    uint64_t device_size;
    int rc = nvfuse_nvme_open(bdf, &t->spdk_transport, &device_size);
    if (rc) { errno = -rc; goto fail; }
    if (!format) {
        struct nvfuse_superblock_common disk;
        if (read_disk_header(t, &disk) || disk.sb_no_of_blocks <= 0 ||
            disk.sb_no_of_blocks > (int64_t)(device_size / CLUSTER_SIZE)) {
            errno = EINVAL; goto fail;
        }
        logical_size = disk.sb_no_of_blocks * CLUSTER_SIZE;
    }
    if (logical_size > device_size || logical_size < IMAGE_GROUP_BYTES || logical_size % IMAGE_GROUP_BYTES) {
        errno = EINVAL; goto fail;
    }
    t->size_bytes = logical_size;
    t->metadata_only = metadata_only;
    h->nvh_target = t;
    if (mount_target(h, format)) goto fail;
    return h;
fail:;
    int saved = errno;
    nvfuse_nvme_close(t->spdk_transport);
    free(t); free(h); errno = saved;
    return NULL;
}
#endif
