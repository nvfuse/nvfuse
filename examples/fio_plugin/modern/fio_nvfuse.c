/* FIO adapter. No NVFUSE/SPDK internal types cross the runtime interface. */
#include "config-host.h"
#include "fio.h"
#include "nvfuse_spdk_runtime.h"
#define NVFIO_BLOCK_SIZE 4096U
#define MAX_BIOS 2048U
#define MAX_DEPTH 128U
static int claimed;
struct job { struct nvfio_client *client; struct io_u *events[MAX_DEPTH]; };
static int setup(struct thread_data *td)
{
    if (!td->o.use_thread || td->o.numjobs > 4 || td->o.nr_files != 1 ||
        !td->o.odirect || !td->o.size || td->o.iodepth > MAX_DEPTH) {
        log_err("nvfuse_spdk: require thread=1,numjobs<=4,nrfiles=1,direct=1,size and iodepth<=128\n");
        return EINVAL;
    }
    unsigned long long max_bs = td->o.max_bs[DDIR_READ] > td->o.max_bs[DDIR_WRITE] ?
                                td->o.max_bs[DDIR_READ] : td->o.max_bs[DDIR_WRITE];
    if (!max_bs || max_bs % NVFIO_BLOCK_SIZE || max_bs / NVFIO_BLOCK_SIZE * td->o.iodepth > MAX_BIOS ||
        td->o.min_bs[DDIR_READ] % NVFIO_BLOCK_SIZE || td->o.min_bs[DDIR_WRITE] % NVFIO_BLOCK_SIZE) {
        log_err("nvfuse_spdk: aligned 4KiB I/O required; iodepth*(max_bs/4096) must be <=2048\n");
        return EINVAL;
    }
    if (++claimed > 4) { log_err("nvfuse_spdk: at most four jobs per process are supported\n"); return EINVAL; }
    return 0;
}
static int init(struct thread_data *td)
{
    const char *format = getenv("NVFUSE_FORMAT");
    int rc = nvfio_start(getenv("NVFUSE_SPDK_JSON"), getenv("NVFUSE_SPDK_BDF"), format && !strcmp(format, "1"));
    if (rc) log_err("nvfuse_spdk: startup failed (%d); require NVFUSE_SPDK_JSON/BDF/BDEV\n", rc);
    if (rc) return rc < 0 ? -rc : rc;
    struct job *job = calloc(1, sizeof(*job));
    if (!job) { nvfio_stop(); return ENOMEM; }
    unsigned long long max_bs = td->o.max_bs[DDIR_READ] > td->o.max_bs[DDIR_WRITE] ?
                                td->o.max_bs[DDIR_READ] : td->o.max_bs[DDIR_WRITE];
    unsigned queues = (max_bs / NVFIO_BLOCK_SIZE * td->o.iodepth + 511) / 512;
    job->client = nvfio_client_alloc(queues);
    if (!job->client) { free(job); nvfio_stop(); return ENOMEM; }
    td->io_ops_data = job;
    return 0;
}
static int open_file(struct thread_data *td, struct fio_file *f)
{
    int fd = nvfio_open(f->file_name, td_write(td));
    if (fd < 0) return -fd;
    f->fd = fd; return 0;
}
static int close_file(struct thread_data *td, struct fio_file *f)
{
    (void)td; int rc = nvfio_close(f->fd); f->fd = -1; return rc < 0 ? -rc : rc;
}
static int file_size(struct thread_data *td, struct fio_file *f)
{
    /* No host stat/create/extend: this pathname belongs inside NVFUSE. */
    f->real_file_size = td->o.size; fio_file_set_size_known(f); return 0;
}
static enum fio_q_status queue(struct thread_data *td, struct io_u *io)
{
    struct job *job = td->io_ops_data;
    fio_ro_check(td, io);
    int rc;
    if (io->ddir == DDIR_SYNC || io->ddir == DDIR_DATASYNC) {
        rc = nvfio_sync(job->client, io->file->fd);
        if (rc == -EBUSY) return FIO_Q_BUSY;
        io->error = rc < 0 ? -rc : 0; return FIO_Q_COMPLETED;
    }
    if (io->ddir != DDIR_READ && io->ddir != DDIR_WRITE) {
        io->error = EOPNOTSUPP; return FIO_Q_COMPLETED;
    }
    rc = nvfio_submit(job->client, io->engine_data, io->file->fd, io->xfer_buf, io->xfer_buflen,
                      io->offset, io->ddir == DDIR_WRITE);
    if (rc == -EAGAIN) return FIO_Q_BUSY;
    if (rc) { io->error = -rc; return FIO_Q_COMPLETED; }
    return FIO_Q_QUEUED;
}
static double now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9;
}
static int getevents(struct thread_data *td, unsigned min, unsigned max, const struct timespec *timeout)
{
    struct job *job = td->io_ops_data; unsigned count = 0;
    if (!max || max > MAX_DEPTH) return -EINVAL;
    double until = timeout ? now() + timeout->tv_sec + timeout->tv_nsec / 1e9 : 0;
    do {
        struct nvfio_request *completed[MAX_DEPTH];
        int n = nvfio_poll(job->client, max - count, completed);
        if (n < 0) return n;
        for (int i = 0; i < n; i++) {
            struct io_u *io = nvfio_request_user(completed[i]);
            io->error = nvfio_request_error(completed[i]); io->resid = 0;
            job->events[count++] = io;
        }
        if (count >= min || (timeout && now() >= until)) break;
    } while (count < max);
    return count;
}
static struct io_u *event(struct thread_data *td, int n) { struct job *job = td->io_ops_data; return job->events[n]; }
static int io_init(struct thread_data *td, struct io_u *io)
{
    (void)td; io->engine_data = nvfio_request_alloc(io); return io->engine_data ? 0 : ENOMEM;
}
static void io_free(struct thread_data *td, struct io_u *io)
{
    (void)td; nvfio_request_free(io->engine_data); io->engine_data = NULL;
}
static int mem_alloc(struct thread_data *td, size_t size)
{
    td->orig_buffer = nvfio_dma_alloc(size); return td->orig_buffer ? 0 : ENOMEM;
}
static void mem_free(struct thread_data *td) { nvfio_dma_free(td->orig_buffer); }
static void cleanup(struct thread_data *td)
{
    struct job *job = td->io_ops_data;
    if (!job) return;
    int rc = nvfio_client_free(job->client);
    if (rc) { log_err("nvfuse_spdk: client shutdown failed (%d)\n", rc); return; }
    free(job); td->io_ops_data = NULL;
    rc = nvfio_stop(); if (rc) log_err("nvfuse_spdk: shutdown failed (%d)\n", rc);
}
struct ioengine_ops ioengine = {
    .name = "nvfuse_spdk", .version = FIO_IOOPS_VERSION,
    .flags = FIO_DISKLESSIO | FIO_NOEXTEND | FIO_NODISKUTIL,
    .setup = setup, .init = init, .cleanup = cleanup,
    .open_file = open_file, .close_file = close_file, .get_file_size = file_size,
    .queue = queue, .getevents = getevents, .event = event,
    .io_u_init = io_init, .io_u_free = io_free,
    .iomem_alloc = mem_alloc, .iomem_free = mem_free,
};
