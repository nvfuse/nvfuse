#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_mkfs.h"
#include "nvfuse_ipc_ring.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMAGE_GROUP_BYTES (128ULL * 1024 * 1024)
static int active_handle;

/* Core callers assume successful block I/O. Fail loudly rather than allowing
 * an I/O error to be mistaken for a successfully persisted metadata update. */
static int file_transfer(struct io_target *target, uint64_t offset, size_t bytes,
                         void *buffer, int write_op)
{
    if (offset > target->size_bytes || bytes > target->size_bytes - offset) {
        fprintf(stderr, "file backend: I/O beyond image boundary\n");
        abort();
    }
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
    if (count < 0 || count > task->qdepth - 1 - reactor_cq_size(task)) return -EINVAL;
    for (int i = 0; i < count; i++) {
        if (reqs[i]->req_type != SPDK_BDEV_IO_TYPE_FLUSH &&
            reqs[i]->req_type != SPDK_BDEV_IO_TYPE_READ &&
            reqs[i]->req_type != SPDK_BDEV_IO_TYPE_WRITE) return -EINVAL;
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

struct nvfuse_handle *nvfuse_file_open(const char *path, int create, uint64_t size)
{
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
    nvh->nvh_target = target;
    nvh->blk_size = SECTOR_SIZE;
    nvh->total_blkcount = size / SECTOR_SIZE;
    nvh->nvh_params.buffer_size = 16;
    nvh->nvh_params.qdepth = 32;
    nvh->nvh_params.need_mount = 1;
    if (create && nvfuse_format(nvh)) goto fail_handle;
    /* Refuse malformed/unclean images before allocating mount resources. */
    struct nvfuse_superblock_common disk;
    if (pread(fd, &disk, sizeof(disk), 0) != sizeof(disk) ||
        disk.sb_signature != NVFUSE_SB_SIGNATURE ||
        disk.sb_no_of_sectors != (int64_t)nvh->total_blkcount ||
        disk.sb_no_of_blocks != (int64_t)(size / CLUSTER_SIZE) ||
        disk.sb_no_of_blocks_per_bg != IMAGE_GROUP_BYTES / CLUSTER_SIZE ||
        disk.sb_bg_num != (int64_t)(size / IMAGE_GROUP_BYTES) ||
        disk.sb_root_ino != ROOT_INO ||
        (disk.sb_state != FS_STATE_FORMATTED && disk.sb_state != FS_STATE_UMOUNTED)) {
        errno = EINVAL; goto fail_handle;
    }
    /* Existing core statistics use process-local rings and lazy pools. */
    if (nvfuse_ipc_init(&nvh->nvh_ipc_ctx)) goto fail_handle;
    nvh->nvh_sb.sb_nvh = nvh;
    if (nvfuse_mount(nvh)) {
        fprintf(stderr, "file backend: mount initialization failed\n");
        /* The legacy core has no partial-mount unwind contract. */
        abort();
    }
    active_handle = 1;
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
    free(nvh->nvh_target); free(nvh);
    active_handle = 0;
    return close(fd);
}
