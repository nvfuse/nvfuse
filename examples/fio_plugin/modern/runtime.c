/* Keep NVFUSE/SPDK and FIO headers in separate translation units: both embed
 * Linux-derived tree/list declarations with incompatible public names. */
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include "spdk/module/bdev/nvme.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_aio.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_primary_journal.h"
#include "runtime.h"
#include "cache_policy.h"
#include "spsc.h"
#include "stats.h"
#include <sys/mman.h>
#define CLIENT_QUEUES 4
struct nvfio_lane { struct nvfuse_aio_queue queue; unsigned reserved; };
struct nvfio_client {
    struct nvfio_lane lanes[CLIENT_QUEUES];
    unsigned count, inflight;
    unsigned outstanding; /* fio worker only; includes pending and completed I/O. */
    struct nvfio_ring pending, completed;
    struct nvfio_client *next; /* filesystem thread only */
};
struct nvfio_request { struct nvfuse_aio_req aio; void *user; unsigned blocks; int error; struct nvfio_lane *lane; };
static pthread_mutex_t lifecycle = PTHREAD_MUTEX_INITIALIZER;
static unsigned references;
static uint64_t journal_deadline, journal_interval;
static int drain_clients(void);
static int poll_queues = 1;
static unsigned buffer_mb; /* Zero selects capacity-based automatic sizing. */
static struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready, exited, result, started, format;
    struct nvfuse_handle *handle;
    struct nvfuse_ipc_context ipc;
    struct nvfuse_params params;
    struct spdk_pci_addr address;
    struct spdk_app_opts opts;
    struct spdk_thread *fs_thread;
    struct spdk_poller *data_poller, *stats_poller;
    struct nvfio_stats_record *stats_map, stats;
    struct nvfio_client *clients;
    atomic_int data_failed;
    uint64_t submitted, completed, submit_passes;
    unsigned max_batch;
} rt = {.mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};
struct call {
    void (*fn)(struct call *);
    const char *path;
    int fd, create;
    struct nvfio_client *client;
    struct nvfio_request *request;
    struct nvfio_request **completed;
    unsigned max;
    int result, done;
    int (*work)(struct nvfuse_handle *, void *);
    void *argument;
};
static int service_clients(void *unused);
static int publish_stats(void *unused)
{
    (void)unused;
    if (!rt.stats_map) return SPDK_POLLER_IDLE;
    uint64_t now = spdk_get_ticks(), hz = spdk_get_ticks_hz();
    rt.stats.word[NS_TIMESTAMP_NS] = (now / hz) * UINT64_C(1000000000) +
        (now % hz) * UINT64_C(1000000000) / hz;
    rt.stats.word[NS_INFLIGHT] = rt.stats.word[NS_PENDING] = rt.stats.word[NS_READY] = 0;
    for (struct nvfio_client *c = rt.clients; c; c = c->next) {
        rt.stats.word[NS_INFLIGHT] += c->inflight;
        rt.stats.word[NS_PENDING] += nvfio_ring_count(&c->pending);
        rt.stats.word[NS_READY] += nvfio_ring_count(&c->completed);
    }
    rt.stats.word[NS_SUBMITTED] = rt.submitted;
    rt.stats.word[NS_COMPLETED] = rt.completed;
    uint64_t sequence = rt.stats_map->word[NS_SEQ];
    __atomic_store_n(&rt.stats_map->word[NS_SEQ], sequence + 1, __ATOMIC_SEQ_CST);
    for (unsigned i = 1; i < NS_WORDS; i++)
        __atomic_store_n(&rt.stats_map->word[i], rt.stats.word[i], __ATOMIC_RELAXED);
    memcpy(rt.stats_map->bdf, rt.stats.bdf, sizeof(rt.stats.bdf));
    memcpy(rt.stats_map->bdev, rt.stats.bdev, sizeof(rt.stats.bdev));
    __atomic_store_n(&rt.stats_map->word[NS_SEQ], sequence + 2, __ATOMIC_RELEASE);
    return SPDK_POLLER_IDLE;
}
static int start_stats(void)
{
    const char *setting = getenv("NVFUSE_SPDK_STATS");
    if (!setting || !strcmp(setting, "0")) return 0;
    char path[PATH_MAX];
    if (!strcmp(setting, "1"))
        snprintf(path, sizeof(path), "/tmp/nvfuse-spdk-%ld.stats", (long)getpid());
    else {
        if (*setting != '/' || strlen(setting) >= sizeof(path)) return -EINVAL;
        strcpy(path, setting);
    }
    /* Never truncate a pre-existing path or follow a symlink. */
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -errno;
    if (ftruncate(fd, sizeof(*rt.stats_map))) {
        int error = errno; close(fd); unlink(path); return -error;
    }
    rt.stats_map = mmap(NULL, sizeof(*rt.stats_map), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int error = errno; close(fd);
    if (rt.stats_map == MAP_FAILED) { rt.stats_map = NULL; unlink(path); return -error; }
    memset(&rt.stats, 0, sizeof(rt.stats));
    rt.stats.word[NS_MAGIC] = NVFIO_STATS_MAGIC; rt.stats.word[NS_VERSION] = 2;
    snprintf(rt.stats.bdf, sizeof(rt.stats.bdf), "%s", getenv("NVFUSE_SPDK_BDF"));
    const char *bdev = getenv("NVFUSE_SPDK_BDEV");
    if (strlen(bdev) >= sizeof(rt.stats.bdev)) {
        munmap(rt.stats_map, sizeof(*rt.stats_map)); rt.stats_map = NULL; unlink(path); return -ENAMETOOLONG;
    }
    strcpy(rt.stats.bdev, bdev);
    rt.stats.word[NS_PID] = getpid(); rt.stats.word[NS_STATE] = 1;
    rt.stats.word[NS_TICKS_HZ] = spdk_get_ticks_hz();
    rt.stats.word[NS_IO_CORE] = spdk_env_get_first_core();
    rt.stats.word[NS_FS_CORE] = spdk_env_get_next_core(spdk_env_get_first_core());
    publish_stats(NULL);
    rt.stats_poller = spdk_poller_register(publish_stats, NULL, 100000);
    if (!rt.stats_poller) { munmap(rt.stats_map, sizeof(*rt.stats_map)); rt.stats_map = NULL; unlink(path); return -ENOMEM; }
    printf("NVFUSE_STATS path=%s interval_ms=100 version=2\n", path);
    return 0;
}
static void stop_stats(void)
{
    if (!rt.stats_map) return;
    spdk_poller_unregister(&rt.stats_poller);
    rt.stats.word[NS_STATE] = 2; publish_stats(NULL);
    munmap(rt.stats_map, sizeof(*rt.stats_map)); rt.stats_map = NULL;
}

static int api_error(int rc) { return rc < -1 ? rc : -(errno ? errno : EIO); }
static void signal_ready(int result)
{
    pthread_mutex_lock(&rt.mutex); rt.result = result; rt.ready = 1;
    pthread_cond_broadcast(&rt.cond); pthread_mutex_unlock(&rt.mutex);
}
static void mount_fs(void *unused)
{
    (void)unused;
    struct spdk_bdev_nvme_opts nvme_opts;
    spdk_bdev_nvme_get_opts(&nvme_opts, sizeof(nvme_opts));
    const char *journal = getenv("NVFUSE_JOURNAL");
    if ((!journal || strcmp(journal, "0")) && !nvme_opts.enable_flush) {
        fprintf(stderr, "nvfuse_spdk: journaling requires bdev_nvme enable_flush=true; refusing before mount/format\n");
        spdk_thread_exit(rt.fs_thread);
        signal_ready(-EINVAL); spdk_app_stop(1); return;
    }
    strcpy(rt.params.appname, "fio");
    rt.params.need_mount = 1; rt.params.need_format = rt.format;
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(getenv("NVFUSE_SPDK_BDEV"));
    uint64_t blocks = bdev ? spdk_bdev_get_num_blocks(bdev) : 0;
    uint32_t block_size = bdev ? spdk_bdev_get_block_size(bdev) : 0;
    if (!blocks || !block_size || blocks > UINT64_MAX / block_size) {
        signal_ready(-ENODEV); spdk_app_stop(1); return;
    }
    uint64_t device_bytes = blocks * block_size;
    unsigned selected_mb = buffer_mb ? buffer_mb : nvfio_auto_buffer_mb(device_bytes);
    rt.params.buffer_size = selected_mb;
    printf("NVFUSE_CONFIG buffer_mb=%u mode=%s device_bytes=%" PRIu64
           " target_mb=%" PRIu64 " cap_mb=%u spdk_memory_mb=%u\n",
           selected_mb, buffer_mb ? "explicit" : "auto", device_bytes,
           device_bytes / 1000U / (1024U * 1024U),
           NVFIO_CACHE_MAX_MB, NVFIO_SPDK_MEMORY_MB);
    rt.handle = nvfuse_create_handle(&rt.ipc, &rt.params);
    if (!rt.handle) { signal_ready(-EIO); spdk_app_stop(1); return; }
    struct statvfs vfs;
    if (!nvfuse_statvfs(rt.handle, "/", &vfs))
        printf("NVFUSE_CAPACITY device_bytes=%" PRIu64 " filesystem_bytes=%" PRIu64
               " free_bytes=%" PRIu64 " sectors=%" PRIu64 "\n",
               rt.handle->total_blkcount * (uint64_t)SECTOR_SIZE,
               (uint64_t)vfs.f_blocks * vfs.f_frsize,
               (uint64_t)vfs.f_bavail * vfs.f_frsize,
               (uint64_t)rt.handle->nvh_sb.sb_no_of_sectors);
    unsigned interval_ms = 100;
    const char *interval = getenv("NVFUSE_JOURNAL_MS");
    if (interval) {
        char *end; unsigned long value = strtoul(interval, &end, 10);
        if (!*interval || *end || value < 1 || value > 1000) {
            fprintf(stderr, "NVFUSE_JOURNAL_MS must be 1..1000\n"); abort();
        }
        interval_ms = (unsigned)value;
    }
    journal_interval = spdk_get_ticks_hz() / 1000 * interval_ms;
    journal_deadline = spdk_get_ticks() + journal_interval;
    if (rt.handle->nvh_target->journal)
        printf("NVFUSE_JOURNAL_POLICY interval_ms=%u checkpoint=%s owner=filesystem\n", interval_ms,
               nvj_version(rt.handle->nvh_target->journal) == 3 ? "pressure" : "synchronous");
    rt.data_poller = spdk_poller_register(service_clients, NULL, 0);
    if (!rt.data_poller) {
        nvfuse_destroy_handle(rt.handle, 1, 1); rt.handle = NULL;
        signal_ready(-ENOMEM); spdk_app_stop(1); return;
    }
    int stats_result = start_stats();
    if (stats_result) {
        fprintf(stderr, "NVFUSE_STATS startup failed: %s\n", strerror(-stats_result));
        spdk_poller_unregister(&rt.data_poller);
        nvfuse_destroy_handle(rt.handle, 1, 1); rt.handle = NULL;
        signal_ready(stats_result); spdk_app_stop(1); return;
    }
    signal_ready(0);
}
static void start_fs(void *unused)
{
    (void)unused;
    struct spdk_cpuset cpuset;
    unsigned first = spdk_env_get_first_core();
    unsigned fs_core = spdk_env_get_next_core(first);
    if (fs_core == UINT32_MAX) {
        signal_ready(-EINVAL); spdk_app_stop(1); return;
    }
    printf("NVFUSE_CORES io=%u fs=%u\n", first, fs_core);
    spdk_cpuset_zero(&cpuset); spdk_cpuset_set_cpu(&cpuset, fs_core, true);
    rt.fs_thread = spdk_thread_create("nvfuse_fs", &cpuset);
    if (!rt.fs_thread || spdk_thread_send_msg(rt.fs_thread, mount_fs, NULL)) {
        signal_ready(-ENOMEM); spdk_app_stop(1);
    }
}
static void *app_thread(void *unused)
{
    (void)unused;
    int rc = spdk_app_start(&rt.opts, start_fs, NULL);
    spdk_app_fini();
    pthread_mutex_lock(&rt.mutex);
    if (!rt.ready) { rt.result = rc ? -EIO : -ECANCELED; rt.ready = 1; }
    atomic_store_explicit(&rt.data_failed, 1, memory_order_release);
    rt.exited = 1; pthread_cond_broadcast(&rt.cond); pthread_mutex_unlock(&rt.mutex);
    return NULL;
}
static void run_call(void *arg1)
{
    struct call *c = arg1; errno = 0; c->fn(c);
    pthread_mutex_lock(&rt.mutex); c->done = 1;
    pthread_cond_broadcast(&rt.cond); pthread_mutex_unlock(&rt.mutex);
}
static int dispatch(struct call *c)
{
    pthread_mutex_lock(&rt.mutex);
    if (!rt.ready || rt.exited || rt.result) { pthread_mutex_unlock(&rt.mutex); return -EIO; }
    if (spdk_thread_send_msg(rt.fs_thread, run_call, c)) {
        pthread_mutex_unlock(&rt.mutex); return -EIO;
    }
    while (!c->done && !rt.exited) pthread_cond_wait(&rt.cond, &rt.mutex);
    int result = c->done ? c->result : -EIO;
    pthread_mutex_unlock(&rt.mutex); return result;
}
static int start(const char *json, const char *bdf, int format)
{
    if (rt.started || !json || !bdf || !getenv("NVFUSE_SPDK_BDEV") ||
        spdk_pci_addr_parse(&rt.address, bdf)) return -EINVAL;
    const char *cache = getenv("NVFUSE_BUFFER_MB");
    if (cache && strcmp(cache, "auto")) {
        char *end;
        errno = 0;
        unsigned long value = strtoul(cache, &end, 10);
        if (!*cache || *cache < '0' || *cache > '9' || *end || errno ||
            value < NVFIO_CACHE_MIN_MB || value > NVFIO_CACHE_MAX_MB) {
            fprintf(stderr, "nvfuse_spdk: NVFUSE_BUFFER_MB must be auto or an integer from %u to %u MiB\n",
                    NVFIO_CACHE_MIN_MB, NVFIO_CACHE_MAX_MB);
            return -EINVAL;
        }
        buffer_mb = (unsigned)value;
    }
    const char *poll = getenv("NVFUSE_SPDK_POLL_QUEUES");
    if (poll) {
        if (strcmp(poll, "0") && strcmp(poll, "1")) {
            fprintf(stderr, "nvfuse_spdk: NVFUSE_SPDK_POLL_QUEUES must be 0 or 1\n");
            return -EINVAL;
        }
        poll_queues = !strcmp(poll, "1");
    }
    const char *mask = getenv("NVFUSE_SPDK_CORE_MASK");
    if (mask) {
        struct spdk_cpuset cpus;
        if (spdk_cpuset_parse(&cpus, mask) || spdk_cpuset_count(&cpus) < 2) {
            fprintf(stderr, "nvfuse_spdk: NVFUSE_SPDK_CORE_MASK requires at least two cores\n");
            return -EINVAL;
        }
    }
    /* SPDK v26.09 otherwise suppresses NVMe FLUSH by default. Set this before
     * JSON controller attachment, and reject a later JSON override at mount. */
    struct spdk_bdev_nvme_opts nvme_opts;
    spdk_bdev_nvme_get_opts(&nvme_opts, sizeof(nvme_opts));
    nvme_opts.enable_flush = true;
    int options_rc = spdk_bdev_nvme_set_opts(&nvme_opts);
    if (options_rc) return options_rc;
    rt.started = 1; rt.format = format;
    spdk_app_opts_init(&rt.opts, sizeof(rt.opts));
    rt.opts.name = "nvfuse_fio"; rt.opts.json_config_file = json;
    rt.opts.reactor_mask = mask ? mask : "0x3";
    rt.opts.mem_size = NVFIO_SPDK_MEMORY_MB;
    rt.opts.num_pci_addr = 1; rt.opts.pci_allowed = malloc(sizeof(rt.address));
    if (!rt.opts.pci_allowed) return -ENOMEM;
    *rt.opts.pci_allowed = rt.address; rt.opts.rpc_addr = NULL;
    int rc = pthread_create(&rt.thread, NULL, app_thread, NULL);
    if (rc) return -rc;
    pthread_mutex_lock(&rt.mutex);
    while (!rt.ready) pthread_cond_wait(&rt.cond, &rt.mutex);
    rc = rt.result; pthread_mutex_unlock(&rt.mutex);
    if (rc) pthread_join(rt.thread, NULL);
    return rc;
}
int nvfio_start(const char *json, const char *bdf, int format)
{
    pthread_mutex_lock(&lifecycle);
    int rc = references ? 0 : start(json, bdf, format);
    if (!rc) references++;
    pthread_mutex_unlock(&lifecycle);
    return rc;
}
static void client_init(struct call *c)
{
    unsigned initialized = 0;
    for (unsigned i = 0; i < c->client->count; i++) {
        struct nvfuse_aio_queue *q = &c->client->lanes[i].queue;
        if (nvfuse_aio_queue_init(rt.handle->nvh_target, q, AIO_MAX_QDEPTH / 2)) goto fail;
        initialized++;
        if (poll_queues && reactor_task_enable_polling(rt.handle->nvh_target, q->task)) goto fail;
    }
    printf("NVFUSE_DATA_QUEUE mode=%s\n", poll_queues ? "poll" : "message");
    c->client->next = rt.clients; rt.clients = c->client;
    return;
fail:
    for (unsigned i = 0; i < initialized; i++)
        reactor_free_task(rt.handle->nvh_target, c->client->lanes[i].queue.task);
    c->result = -ENOMEM;
}
struct nvfio_client *nvfio_client_alloc(unsigned queues)
{
    if (!queues || queues > CLIENT_QUEUES) return NULL;
    struct nvfio_client *client;
    if (posix_memalign((void **)&client, _Alignof(struct nvfio_client), sizeof(*client))) return NULL;
    memset(client, 0, sizeof(*client));
    nvfio_ring_init(&client->pending); nvfio_ring_init(&client->completed);
    client->count = queues;
    struct call c = {.fn = client_init, .client = client};
    if (dispatch(&c)) { free(client); return NULL; }
    return client;
}
static void client_fini(struct call *c)
{
    if (c->client->inflight || nvfio_ring_count(&c->client->pending) ||
        nvfio_ring_count(&c->client->completed)) { c->result = -EBUSY; return; }
    struct nvfio_client **link = &rt.clients;
    while (*link && *link != c->client) link = &(*link)->next;
    if (!*link) { c->result = -EINVAL; return; }
    *link = c->client->next;
    for (unsigned i = 0; i < c->client->count; i++) {
        struct nvfuse_aio_queue *q = &c->client->lanes[i].queue;
        if (q->aio_stat->aio_lat_total_count) nvfuse_aio_queue_deinit(rt.handle, q);
        reactor_free_task(rt.handle->nvh_target, q->task);
    }
}
int nvfio_client_free(struct nvfio_client *client)
{
    if (client->outstanding) return -EBUSY;
    struct call c = {.fn = client_fini, .client = client};
    int rc = dispatch(&c); if (!rc) free(client); return rc;
}
static void open_fs(struct call *c)
{
    int fd = nvfuse_openfile_path(rt.handle, c->path, O_RDWR | O_DIRECT | (c->create ? O_CREAT : 0), 0644);
    c->result = fd < 0 ? api_error(fd) : fd;
}
int nvfio_open(const char *path, int create)
{
    struct call c = {.fn = open_fs, .path = path, .create = create}; return dispatch(&c);
}
static void close_fs(struct call *c)
{
    int rc = nvfuse_closefile(rt.handle, c->fd); c->result = rc < 0 ? api_error(rc) : 0;
}
int nvfio_close(int fd) { struct call c = {.fn = close_fs, .fd = fd}; return dispatch(&c); }
static void sync_fs(struct call *c)
{
    if (rt.handle->nvh_target->journal) {
        int rc = drain_clients();
        if (rc) { c->result = rc; return; }
    } else if (c->client->inflight) { c->result = -EBUSY; return; }
    int rc = nvfuse_fsync(rt.handle, c->fd); c->result = rc < 0 ? api_error(rc) : 0;
}
int nvfio_sync(struct nvfio_client *client, int fd) { if (client->outstanding) return -EBUSY; struct call c = {.fn = sync_fs, .client = client, .fd = fd}; return dispatch(&c); }
struct nvfio_request *nvfio_request_alloc(void *user)
{
    struct nvfio_request *r = calloc(1, sizeof(*r)); if (r) r->user = user; return r;
}
void nvfio_request_free(struct nvfio_request *r) { free(r); }
void *nvfio_request_user(struct nvfio_request *r) { return r->user; }
int nvfio_request_error(struct nvfio_request *r) { return r->error ? r->error : (r->aio.error ? EIO : 0); }
static void submit_fs(struct call *c)
{
    struct nvfio_request *r = c->request;
    struct nvfio_lane *lane = NULL;
    for (unsigned i = 0; i < c->client->count; i++)
        if (c->client->lanes[i].reserved + r->blocks <= AIO_MAX_QDEPTH) {
            lane = &c->client->lanes[i]; break;
        }
    if (!lane) { c->result = -EAGAIN; return; }
    r->lane = lane;
    int rc = nvfuse_aio_queue_submission(rt.handle, &lane->queue, &r->aio);
    if (rc) { c->result = api_error(rc); return; }
    lane->reserved += r->blocks; c->client->inflight++;
}
int nvfio_submit(struct nvfio_client *client, struct nvfio_request *r, int fd, void *buf, size_t size, uint64_t offset, int write)
{
    if (!size || size > AIO_MAX_QDEPTH * CLUSTER_SIZE || offset % CLUSTER_SIZE ||
        size % CLUSTER_SIZE || (uintptr_t)buf % CLUSTER_SIZE) return -EINVAL;
    if (client->outstanding >= 128) return -EAGAIN;
    if (atomic_load_explicit(&rt.data_failed, memory_order_acquire)) return -EIO;
    r->error = 0;
    r->blocks = size / CLUSTER_SIZE;
    memset(&r->aio, 0, sizeof(r->aio));
    r->aio.fid = fd; r->aio.opcode = write ? WRITE : READ;
    r->aio.buf = buf; r->aio.bytes = size; r->aio.offset = offset;
    r->aio.sb = &rt.handle->nvh_sb; INIT_LIST_HEAD(&r->aio.list);
    if (nvfio_ring_push(&client->pending, r)) return -EAGAIN;
    client->outstanding++;
    return 0;
}
static void poll_fs(struct call *c)
{
    struct nvfuse_aio_req *list[AIO_MAX_QDEPTH];
    unsigned count = 0;
    for (unsigned lane = 0; lane < c->client->count && count < c->max; lane++) {
        struct nvfuse_aio_queue *q = &c->client->lanes[lane].queue;
        int n = nvfuse_io_getevents(&rt.handle->nvh_sb, q, 0, c->max - count, list);
        if (n < 0) { c->result = n; return; }
        for (int i = 0; i < n; i++) {
            struct nvfio_request *r = (struct nvfio_request *)((char *)list[i] - offsetof(struct nvfio_request, aio));
            if (rt.stats_map) {
                if (r->aio.error) rt.stats.word[NS_ERRORS]++;
                else {
                    int write = r->aio.opcode == WRITE;
                    rt.stats.word[write ? NS_WRITE_IOS : NS_READ_IOS]++;
                    rt.stats.word[write ? NS_WRITE_BYTES : NS_READ_BYTES] += r->aio.bytes;
                    rt.stats.word[write ? NS_WRITE_LAT_TICKS : NS_READ_LAT_TICKS] +=
                        r->aio.complete_tsc - r->aio.submit_tsc;
                }
            }
            c->completed[count++] = r; c->client->inflight--;
            r->lane->reserved -= r->blocks;
            q->aio_stat->aio_lat_total_count++;
            q->aio_stat->aio_total_size += r->aio.bytes;
        }
    }
    c->result = count;
}
/* Poll every client's submitted I/O before a durability boundary; pending
 * frontend requests remain queued and cannot cross the commit barrier. */
static int drain_clients(void)
{
    unsigned inflight;
    do {
        inflight = 0;
        for (struct nvfio_client *client = rt.clients; client; client = client->next) {
            struct nvfio_request *completed[32];
            struct call poll = {.client = client, .max = 32, .completed = completed};
            poll_fs(&poll);
            if (poll.result < 0) return poll.result;
            for (int i = 0; i < poll.result; i++) {
                int rc = nvfio_ring_push(&client->completed, completed[i]);
                assert(!rc); (void)rc;
            }
            rt.completed += poll.result;
            inflight += client->inflight;
        }
    } while (inflight);
    return 0;
}
static void execute_fs(struct call *c)
{
    int rc = drain_clients();
    c->result = rc ? rc : c->work(rt.handle, c->argument);
}
int nvfio_execute(int (*work)(struct nvfuse_handle *, void *), void *argument)
{
    if (!work) return -EINVAL;
    struct call c = {.fn = execute_fs, .work = work, .argument = argument};
    return dispatch(&c);
}
/* Runs only on the filesystem thread. Core metadata/API ownership is unchanged. */
static int service_clients(void *unused)
{
    (void)unused;
    unsigned work = 0;
    struct io_target *target = rt.handle->nvh_target;
    int draining = target->journal && spdk_get_ticks() >= journal_deadline;
    for (struct nvfio_client *client = rt.clients; client; client = client->next) {
        struct nvfio_request *completed[32];
        struct call poll = {.client = client, .max = 32, .completed = completed};
        if (client->inflight) {
            poll_fs(&poll);
            if (poll.result < 0) {
                atomic_store_explicit(&rt.data_failed, 1, memory_order_release);
                return SPDK_POLLER_BUSY;
            }
            for (int i = 0; i < poll.result; i++) {
                int rc = nvfio_ring_push(&client->completed, completed[i]);
                assert(!rc); (void)rc;
            }
            work += poll.result; rt.completed += poll.result;
        }
        unsigned batch = 0;
        while (!draining && batch < 32) {
            struct nvfio_request *r = nvfio_ring_peek(&client->pending);
            if (!r) break;
            struct call submit = {.client = client, .request = r};
            errno = 0; submit_fs(&submit);
            if (submit.result == -EAGAIN) break;
            /* Save the pointer before consuming; producer may reuse the slot. */
            nvfio_ring_consume(&client->pending);
            if (submit.result) {
                r->error = -submit.result;
                if (rt.stats_map) rt.stats.word[NS_ERRORS]++;
                int rc = nvfio_ring_push(&client->completed, r);
                assert(!rc); (void)rc;
            }
            batch++;
        }
        if (batch) {
            rt.submitted += batch; rt.submit_passes++;
            if (batch > rt.max_batch) rt.max_batch = batch;
            work += batch;
        }
    }
    if (draining && !target->journal_aio) {
        if (target->journal_dirty || nvfuse_get_dirty_count(&rt.handle->nvh_sb)) {
            int rc = nvfuse_primary_journal_commit(rt.handle, 1);
            if (rc) {
                fprintf(stderr, "NVFUSE_JOURNAL periodic commit failed: %d\n", rc);
                atomic_store_explicit(&rt.data_failed, 1, memory_order_release);
                abort();
            }
            work++;
        }
        journal_deadline = spdk_get_ticks() + journal_interval;
    }
    return work ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
int nvfio_poll(struct nvfio_client *client, unsigned max, struct nvfio_request **completed)
{
    if (!max || max > AIO_MAX_QDEPTH) return -EINVAL;
    unsigned n = 0;
    while (n < max) {
        struct nvfio_request *r = nvfio_ring_peek(&client->completed);
        if (!r) break;
        completed[n++] = r;
        nvfio_ring_consume(&client->completed);
    }
    assert(n <= client->outstanding); client->outstanding -= n;
    if (!n && atomic_load_explicit(&rt.data_failed, memory_order_acquire)) return -EIO;
    return n;
}
void *nvfio_dma_alloc(size_t size) { return spdk_dma_zmalloc(size, CLUSTER_SIZE, NULL); }
void nvfio_dma_free(void *buffer) { spdk_dma_free(buffer); }
static void unmount_fs(struct call *c)
{
    if (rt.clients) { c->result = -EBUSY; return; }
    spdk_poller_unregister(&rt.data_poller);
    stop_stats();
    printf("NVFUSE_ASYNC submitted=%" PRIu64 " completed=%" PRIu64
           " submit_passes=%" PRIu64 " max_batch=%u\n",
           rt.submitted, rt.completed, rt.submit_passes, rt.max_batch);
    nvfuse_destroy_handle(rt.handle, 1, 1); rt.handle = NULL;
    spdk_thread_exit(rt.fs_thread);
}
int nvfio_stop(void)
{
    pthread_mutex_lock(&lifecycle);
    if (!references) { pthread_mutex_unlock(&lifecycle); return 0; }
    if (--references) { pthread_mutex_unlock(&lifecycle); return 0; }
    struct call c = {.fn = unmount_fs}; int rc = dispatch(&c);
    if (!rc) { spdk_app_stop(0); pthread_join(rt.thread, NULL); }
    pthread_mutex_unlock(&lifecycle); return rc;
}
