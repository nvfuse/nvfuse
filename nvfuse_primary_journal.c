/* Single filesystem owner, existing SPDK I/O reactor. Async regular data stays
 * on the normal queues; protected blocks are staged by the owner and their
 * completions are published by the I/O thread, preserving SPSC ownership. */
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_primary_journal.h"

#define JOURNAL_DMA_BYTES (32u * CLUSTER_SIZE)

#ifndef NVFUSE_USE_CEPH_SPDK
static void journal_complete(struct spdk_bdev_io *, bool, void *);
#else
static void journal_complete(struct spdk_bdev_io *, enum spdk_bdev_io_status, void *);
#endif
/* One request at a time, owned by the serialized filesystem thread. Reuse
 * only the task; request/DMA lifetime and every disk dependency stay intact. */
static int raw_request(struct io_target *t, uint64_t offset, size_t bytes, void *buf, int type)
{
    if (!t->journal_raw_task) {
        if (type == SPDK_BDEV_IO_TYPE_FLUSH) return reactor_sync_flush(t);
        return type == SPDK_BDEV_IO_TYPE_WRITE ?
            reactor_sync_write_blk(t, offset / CLUSTER_SIZE, bytes / CLUSTER_SIZE, buf) :
            reactor_sync_read_blk(t, offset / CLUSTER_SIZE, bytes / CLUSTER_SIZE, buf);
    }
    struct io_job *job = reactor_make_single_req(t, offset, bytes, buf, type), *done;
    job->cb = journal_complete;
    int rc = reactor_submit_reqs(t, t->journal_raw_task, &job, 1);
    if (!rc) {
        /* Completion is consumed even on bdev failure, before freeing job. */
        reactor_cq_get_reqs(t->journal_raw_task, &done, 1, 1);
        rc = done->ret ? -EIO : 0;
    }
    reactor_free_reqs(t, &job, 1);
    return rc;
}

static uint64_t profile_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static int raw_transfer(void *context, uint64_t offset, size_t bytes, void *buf, int write_op)
{
    struct io_target *t = context;
    if (offset % CLUSTER_SIZE || bytes % CLUSTER_SIZE) return -EINVAL;
    unsigned char *p = buf;
    int rc = 0;
    t->journal_raw++;
    while (bytes && !rc) {
        size_t n = bytes < JOURNAL_DMA_BYTES ? bytes : JOURNAL_DMA_BYTES;
        if (write_op) memcpy(t->journal_dma, p, n);
        rc = raw_request(t, offset, n, t->journal_dma,
                         write_op ? SPDK_BDEV_IO_TYPE_WRITE : SPDK_BDEV_IO_TYPE_READ);
        if (!rc && !write_op) memcpy(p, t->journal_dma, n);
        offset += n; p += n; bytes -= n;
    }
    t->journal_raw--;
    return rc ? -EIO : 0;
}
static int raw_flush(void *context)
{
    struct io_target *t = context;
#ifndef NVFUSE_USE_CEPH_SPDK
    /* All preceding writes have completed before this barrier. SPDK promises
     * persistence at completion when the bdev has no volatile write cache;
     * its NVMe module already turns these flushes into inline no-ops. Avoid
     * sending that no-op across reactors, retaining journal barrier hooks.
     * File-backed and volatile-cache bdevs still issue their real FLUSH. */
    if (t->journal_flush_fast && !spdk_bdev_has_write_cache(t->bdev)) {
        t->journal_flush_elided++;
        return 0;
    }
#endif
    t->journal_raw++;
    int rc = raw_request(t, 0, CLUSTER_SIZE, NULL, SPDK_BDEV_IO_TYPE_FLUSH);
    t->journal_raw--;
    return rc ? -EIO : 0;
}
/* The task, DMA image and submitted jobs stay alive until every completion,
 * including failures. The owner alone stages journal state; this callback on
 * the I/O thread only publishes completion, like ordinary reactor requests. */
#ifndef NVFUSE_USE_CEPH_SPDK
static void journal_complete(struct spdk_bdev_io *io, bool success, void *argument)
#else
static void journal_complete(struct spdk_bdev_io *io, enum spdk_bdev_io_status success, void *argument)
#endif
{
    struct io_job *job = argument;
    struct reactor_task *task = job->task;
#ifndef NVFUSE_USE_CEPH_SPDK
    job->ret = success ? 0 : (size_t)-EIO;
#else
    job->ret = success == SPDK_BDEV_IO_STATUS_SUCCESS ? 0 : (size_t)-EIO;
#endif
    task->target->current_queue_depth--;
    task->target->io_completed++;
    reactor_cq_put_req(task, job);
    spdk_bdev_free_io(io);
}
static int raw_home_batch(void *context, const struct nvj_home_write *writes, unsigned count)
{
    struct io_target *t = context;
    struct io_job *jobs[NVJ_HOME_BATCH_BLOCKS], *done[NVJ_HOME_BATCH_BLOCKS];
    size_t bytes = 0;
    if (!count || count > NVJ_HOME_BATCH_BLOCKS || !t->journal_home_task) return -EINVAL;
    for (unsigned i = 0; i < count; i++) {
        if (writes[i].offset % CLUSTER_SIZE || !writes[i].bytes ||
            writes[i].bytes % CLUSTER_SIZE || writes[i].bytes > JOURNAL_DMA_BYTES - bytes)
            return -EINVAL;
        bytes += writes[i].bytes;
    }
    bytes = 0;
    for (unsigned i = 0; i < count; i++) {
        void *image = (unsigned char *)t->journal_dma + bytes;
        memcpy(image, writes[i].buffer, writes[i].bytes);
        jobs[i] = reactor_make_single_req(t, writes[i].offset, writes[i].bytes,
                                          image, SPDK_BDEV_IO_TYPE_WRITE);
        jobs[i]->cb = journal_complete;
        bytes += writes[i].bytes;
    }
    t->journal_raw++;
    int rc = reactor_submit_reqs(t, t->journal_home_task, jobs, count);
    if (!rc) {
        unsigned completed = 0;
        while (completed < count)
            completed += reactor_cq_get_reqs(t->journal_home_task, done + completed,
                                             count - completed, count - completed);
        for (unsigned i = 0; i < count; i++) if (done[i]->ret) rc = -EIO;
    }
    t->journal_raw--;
    reactor_free_reqs(t, jobs, count);
    return rc ? -EIO : 0;
}
static struct nvj_ops ops(struct io_target *t)
{
    return (struct nvj_ops){raw_transfer, raw_flush, t};
}
int nvfuse_primary_journal_before_format(struct io_target *t)
{
    const char *mode = getenv("NVFUSE_JOURNAL");
    const char *crc_mode = getenv("NVFUSE_JOURNAL_CRC");
    const char *version = getenv("NVFUSE_JOURNAL_FORMAT_VERSION");
    if (crc_mode && strcmp(crc_mode, "auto") && strcmp(crc_mode, "software")) return -EINVAL;
    if (version && strcmp(version, "2") && strcmp(version, "3")) return -EINVAL;
    if (mode && strcmp(mode, "0") && strcmp(mode, "1")) return -EINVAL;
    if (spdk_bdev_get_num_blocks(t->bdev) * (uint64_t)spdk_bdev_get_block_size(t->bdev) <
        (uint64_t)NVJ_GROUP_BLOCKS * CLUSTER_SIZE) return -EINVAL;
    /* Destroy the previous generation's discovery anchor BEFORE overwriting
     * any home metadata, including when formatting a legacy/OFF volume. */
    void *zero = spdk_dma_zmalloc(CLUSTER_SIZE, CLUSTER_SIZE, NULL);
    if (!zero) return -ENOMEM;
    int rc = reactor_sync_write_blk(t, NVJ_START, 1, zero);
    if (!rc) rc = reactor_sync_flush(t);
    spdk_dma_free(zero);
    return rc ? -EIO : 0;
}
static int format_log(struct nvfuse_handle *h, uint64_t bytes)
{
    struct io_target *t = h->nvh_target;
    unsigned char block[CLUSTER_SIZE];
    int rc = raw_transfer(t, CLUSTER_SIZE, sizeof(block), block, 0);
    if (rc) return rc;
    struct nvfuse_bg_descriptor *bd = (void *)block;
    uint32_t bitmap = bd->bd_dbitmap_start;
    if (NVJ_START < bd->bd_dtable_start || bd->bd_free_blocks < NVJ_BLOCKS) return -EINVAL;
    bd->bd_free_blocks -= NVJ_BLOCKS;
    if ((rc = raw_transfer(t, CLUSTER_SIZE, sizeof(block), block, 1))) return rc;
    if ((rc = raw_transfer(t, (uint64_t)bitmap * CLUSTER_SIZE, sizeof(block), block, 0))) return rc;
    for (unsigned i = NVJ_START; i < NVJ_GROUP_BLOCKS; i++) {
        /* New format only: never overwrite already allocated data. */
        if (block[i / 8] & (1u << (i % 8))) return -EINVAL;
        block[i / 8] |= 1u << (i % 8);
    }
    if ((rc = raw_transfer(t, (uint64_t)bitmap * CLUSTER_SIZE, sizeof(block), block, 1))) return rc;
    if ((rc = raw_transfer(t, 0, sizeof(block), block, 0))) return rc;
    struct nvfuse_superblock_common *sb = (void *)block;
    sb->sb_free_blocks -= NVJ_BLOCKS;
    sb->sb_signature = NVJ_SIGNATURE;
    if ((rc = raw_transfer(t, 0, sizeof(block), block, 1))) return rc;
    struct nvj_ops io = ops(t);
    const char *version = getenv("NVFUSE_JOURNAL_FORMAT_VERSION");
    return nvj_format_version(&io, bytes, 0, NVJ_BLOCKS,
                               version && !strcmp(version, "3") ? 3 : 2);
}
int nvfuse_primary_journal_prepare(struct nvfuse_handle *h, int formatted)
{
    struct io_target *t = h->nvh_target;
    const char *mode = getenv("NVFUSE_JOURNAL");
    const char *crc_mode = getenv("NVFUSE_JOURNAL_CRC");
    const char *reuse = getenv("NVFUSE_JOURNAL_RAW_REUSE");
    const char *poll = getenv("NVFUSE_JOURNAL_RAW_POLL");
    const char *version = getenv("NVFUSE_JOURNAL_FORMAT_VERSION");
    const char *collect = getenv("NVFUSE_JOURNAL_OWNER_COLLECT");
    const char *dirty_reuse = getenv("NVFUSE_JOURNAL_DIRTY_REUSE");
    const char *flush_fast = getenv("NVFUSE_JOURNAL_FLUSH_FAST");
    const char *alloc_batch = getenv("NVFUSE_BUFFERED_ALLOC_BATCH");
    const char *fast_append = getenv("NVFUSE_BUFFERED_FAST_APPEND");
    const char *write_hit = getenv("NVFUSE_BUFFERED_WRITE_HIT");
    const char *alloc_cached = getenv("NVFUSE_BUFFERED_ALLOC_CACHED");
    const char *alloc_fused = getenv("NVFUSE_BUFFERED_ALLOC_FUSED");
    const char *dirty_iov = getenv("NVFUSE_JOURNAL_DIRTY_IOV");
    const char *alloc_account = getenv("NVFUSE_BUFFERED_ALLOC_ACCOUNT");
    const char *bc_direct = getenv("NVFUSE_BUFFERED_BC_DIRECT");
    const char *map_cache = getenv("NVFUSE_BUFFERED_MAP_CACHE");
    if (crc_mode && strcmp(crc_mode, "auto") && strcmp(crc_mode, "software")) return -EINVAL;
    if (mode && strcmp(mode, "0") && strcmp(mode, "1")) return -EINVAL;
    if (reuse && strcmp(reuse, "0") && strcmp(reuse, "1")) return -EINVAL;
    if (poll && strcmp(poll, "0") && strcmp(poll, "1")) return -EINVAL;
    if (version && strcmp(version, "2") && strcmp(version, "3")) return -EINVAL;
    if (collect && strcmp(collect, "0") && strcmp(collect, "1")) return -EINVAL;
    if (dirty_reuse && strcmp(dirty_reuse, "0") && strcmp(dirty_reuse, "1")) return -EINVAL;
    if (flush_fast && strcmp(flush_fast, "0") && strcmp(flush_fast, "1")) return -EINVAL;
    if (alloc_batch && strcmp(alloc_batch, "0") && strcmp(alloc_batch, "1")) return -EINVAL;
    if (map_cache && strcmp(map_cache, "0") && strcmp(map_cache, "1")) return -EINVAL;
    if (bc_direct && strcmp(bc_direct, "0") && strcmp(bc_direct, "1")) return -EINVAL;
    if (alloc_account && strcmp(alloc_account, "0") && strcmp(alloc_account, "1")) return -EINVAL;
    if (alloc_fused && strcmp(alloc_fused, "0") && strcmp(alloc_fused, "1")) return -EINVAL;
    if (alloc_cached && strcmp(alloc_cached, "0") && strcmp(alloc_cached, "1")) return -EINVAL;
    if (fast_append && strcmp(fast_append, "0") && strcmp(fast_append, "1")) return -EINVAL;
    if (write_hit && strcmp(write_hit, "0") && strcmp(write_hit, "1")) return -EINVAL;
    if (dirty_iov && strcmp(dirty_iov, "0") && strcmp(dirty_iov, "1")) return -EINVAL;
    int enabled = !mode || strcmp(mode, "0");
    int standalone = nvfuse_process_model_is_standalone();
    t->buffered_fast_append = enabled && (fast_append ? !strcmp(fast_append, "1") : standalone);
    t->buffered_write_hit = enabled && (write_hit ? !strcmp(write_hit, "1") : standalone);
    t->buffered_alloc_cached = enabled && standalone && (alloc_cached ? !strcmp(alloc_cached, "1") : standalone);
    t->buffered_alloc_fused = enabled && standalone && (alloc_fused ? !strcmp(alloc_fused, "1") : standalone);
    t->journal_dirty_iov = enabled && (dirty_iov ? !strcmp(dirty_iov, "1") : standalone);
    t->buffered_alloc_account = enabled && (alloc_account ? !strcmp(alloc_account, "1") : standalone);
    t->buffered_bc_direct = enabled && (bc_direct ? !strcmp(bc_direct, "1") : standalone);
    t->buffered_map_cache = enabled && (!map_cache || strcmp(map_cache, "0"));
    t->buffered_alloc_batch = enabled && (!alloc_batch || strcmp(alloc_batch, "0"));
    t->journal_flush_fast = enabled && (!flush_fast || strcmp(flush_fast, "0"));
    t->journal_owner_collect = enabled && (!collect || strcmp(collect, "0"));
    int reuse_enabled = enabled && (!reuse || strcmp(reuse, "0"));
    int poll_enabled = reuse_enabled && (!poll || strcmp(poll, "0"));
    if (poll && !strcmp(poll, "1") && !reuse_enabled) return -EINVAL;
    t->journal_dma = spdk_dma_zmalloc(JOURNAL_DMA_BYTES, CLUSTER_SIZE, NULL);
    if (!t->journal_dma) return -ENOMEM;
    if (reuse_enabled) {
        t->journal_raw_task = reactor_alloc_task(t, 1);
        if (!t->journal_raw_task) return -ENOMEM;
        if (poll_enabled) {
            int error = reactor_task_enable_polling(t, t->journal_raw_task);
            if (error) return error;
        }
    }
    if (enabled && (!dirty_reuse || strcmp(dirty_reuse, "0"))) {
        t->journal_dirty_task = reactor_alloc_task(t, AIO_MAX_QDEPTH);
        if (!t->journal_dirty_task) return -ENOMEM;
        if (poll_enabled) {
            int error = reactor_task_enable_polling(t, t->journal_dirty_task);
            if (error) return error;
        }
    }
    uint64_t capacity = h->total_blkcount * SECTOR_SIZE;
    uint64_t bytes = capacity / (NVJ_GROUP_BLOCKS * (uint64_t)CLUSTER_SIZE) *
                     (NVJ_GROUP_BLOCKS * (uint64_t)CLUSTER_SIZE);
    struct nvj_ops io = ops(t);
    int rc;
    if (formatted && enabled && (rc = format_log(h, bytes))) return rc;
    if (!enabled) {
        unsigned char block[CLUSTER_SIZE];
        if ((rc = raw_transfer(t, 0, sizeof(block), block, 0))) return rc;
        if (((struct nvfuse_superblock_common *)(void *)block)->sb_signature != NVFUSE_SB_SIGNATURE) {
            fprintf(stderr, "NVFUSE_JOURNAL refusing OFF mount of a journal-format volume\n");
            return -EINVAL;
        }
        printf("NVFUSE_JOURNAL enabled=0 durability=legacy\n");
        return 0;
    }
    uint64_t volume = 0;
    if ((rc = nvj_probe(&io, capacity, &volume, 0)) || volume != bytes) {
        fprintf(stderr, "NVFUSE_JOURNAL requires a valid journal-format volume; legacy volumes require explicit NVFUSE_JOURNAL=0\n");
        return rc ? rc : -EINVAL;
    }
    if ((rc = nvj_open(&t->journal, &io, volume, 0))) return rc;
    const char *batch = getenv("NVFUSE_JOURNAL_BATCH_IO");
    const char *profile = getenv("NVFUSE_JOURNAL_PROFILE");
    const char *compare = getenv("NVFUSE_JOURNAL_COMPARE_READS");
    const char *home = getenv("NVFUSE_JOURNAL_HOME_BATCH");
    const char *join = getenv("NVFUSE_JOURNAL_LOG_JOIN");
    if ((batch && strcmp(batch, "0") && strcmp(batch, "1")) ||
        (profile && strcmp(profile, "0") && strcmp(profile, "1")) ||
        (compare && strcmp(compare, "0") && strcmp(compare, "1")) ||
        (home && strcmp(home, "0") && strcmp(home, "1")) ||
        (join && strcmp(join, "0") && strcmp(join, "1"))) return -EINVAL;
    if ((rc = nvj_set_batch_io(t->journal, !batch || strcmp(batch, "0")))) return rc;
    int join_enabled = !join || strcmp(join, "0");
    if ((rc = nvj_set_log_join(t->journal, join_enabled))) return rc;
    /* Complete cache images need no old home bytes for redo. Keep the
     * read/compare path as an explicit diagnostic/reference policy. */
    int compare_enabled = compare && !strcmp(compare, "1");
    if ((rc = nvj_set_compare_reads(t->journal, compare_enabled))) return rc;
    int home_enabled = !home || strcmp(home, "0");
    if (home_enabled) {
        t->journal_home_task = reactor_alloc_task(t, NVJ_HOME_BATCH_BLOCKS);
        if (!t->journal_home_task) return -ENOMEM;
        if ((rc = nvj_set_home_writer(t->journal, raw_home_batch, t))) return rc;
    }
    t->journal_profile = profile && !strcmp(profile, "1");
    nvj_set_profile(t->journal, t->journal_profile);
    printf("NVFUSE_JOURNAL_IO batch=%d profile=%d compare_reads=%d max_bytes=%u crc=%s home_batch=%d log_join=%d raw_reuse=%d raw_poll=%d owner_collect=%u dirty_reuse=%d\n",
           !batch || strcmp(batch, "0"), t->journal_profile, compare_enabled, JOURNAL_DMA_BYTES,
           nvj_crc_backend(), home_enabled, join_enabled, t->journal_raw_task != NULL, poll_enabled, t->journal_owner_collect, t->journal_dirty_task != NULL);
    printf("NVFUSE_BUFFERED_POLICY bc_direct=%u alloc_account=%u alloc_fused=%u alloc_cached=%u write_hit=%u fast_append=%u dirty_iov=%u\n",
           t->buffered_bc_direct, t->buffered_alloc_account, t->buffered_alloc_fused,
           t->buffered_alloc_cached, t->buffered_write_hit, t->buffered_fast_append, t->journal_dirty_iov);
    unsigned char block[CLUSTER_SIZE];
    if ((rc = raw_transfer(t, 0, sizeof(block), block, 0))) return rc;
    struct nvfuse_superblock_common *sb = (void *)block;
    if (sb->sb_signature != NVJ_SIGNATURE || sb->sb_no_of_blocks != (int64_t)(bytes / CLUSTER_SIZE) ||
        sb->sb_no_of_sectors != (int64_t)(bytes / SECTOR_SIZE) ||
        sb->sb_no_of_blocks_per_bg != NVJ_GROUP_BLOCKS || sb->sb_root_ino != ROOT_INO ||
        (sb->sb_state != FS_STATE_FORMATTED && sb->sb_state != FS_STATE_UMOUNTED &&
         sb->sb_state != FS_STATE_MOUNTED)) return -EINVAL;
    nvj_mark_filesystem_metadata(t->journal);
    t->journal_mounting = 1;
    printf("NVFUSE_JOURNAL enabled=1 version=%u start=%u blocks=%u replays=%" PRIu64 "\n",
           nvj_version(t->journal), nvj_journal_start(t->journal), NVJ_BLOCKS, nvj_stats(t->journal)->replays);
    return nvj_begin(t->journal, 0, 0);
}
int nvfuse_primary_journal_stage(struct io_target *t, struct io_job *r)
{
    r->journal_staged = 0;
    if (!t->journal || t->journal_raw) return 0;
    if (nvj_error(t->journal) || nvj_recovery_required(t->journal)) return -EIO;
    if (r->req_type == SPDK_BDEV_IO_TYPE_FLUSH) {
        /* Internal eviction flushes cannot publish a transaction. */
        r->journal_staged = 1; r->ret = 0; return 0;
    }
    int write_op = r->req_type == SPDK_BDEV_IO_TYPE_WRITE;
    if (r->offset < 0 || r->bytes <= 0 ||
        (r->req_type != SPDK_BDEV_IO_TYPE_READ && !write_op)) return -EINVAL;
    if (write_op) t->journal_dirty = 1;
    if (!nvj_needs_overlay(t->journal, r->offset, r->bytes, write_op)) {
        if (write_op) nvj_note_data_write(t->journal);
        return 0;
    }
    void *buf = r->buf, *copy = NULL;
    if (!r->offset && write_op) {
        copy = malloc(r->bytes);
        if (!copy) return -ENOMEM;
        memcpy(copy, buf, r->bytes);
        ((struct nvfuse_superblock_common *)copy)->sb_signature = NVJ_SIGNATURE;
        buf = copy;
    }
    int rc = nvj_transfer(t->journal, r->offset, r->bytes, buf, write_op);
    free(copy);
    if (rc) return rc;
    r->ret = 0; r->journal_staged = 1;
    return 0;
}
int nvfuse_primary_journal_sync(struct io_target *t, int64_t offset, size_t bytes,
                                void *buffer, unsigned type)
{
    if (!t->journal || t->journal_raw || !t->journal_owner_collect) return 0;
    struct io_job request = {.offset = offset, .bytes = bytes, .buf = buffer, .req_type = type};
    int rc = nvfuse_primary_journal_stage(t, &request);
    if (rc) {
        /* Preserve the legacy submission path's failure contract: cache
         * writeback cannot unwind partially staged protected images. */
        fprintf(stderr, "NVFUSE_JOURNAL owner staging failed: %d; recovery required\n", rc);
        abort();
    }
    return request.journal_staged ? 1 : 0;
}
int nvfuse_primary_journal_collect(struct io_target *t, const struct nvj_home_write *w, unsigned count)
{
    if (!t->journal || t->journal_raw || !t->journal_owner_collect) return 0;
    for (unsigned i = 0; i < count; i++)
        if (!nvj_needs_overlay(t->journal, w[i].offset, w[i].bytes, 1)) return 0;
    for (unsigned i = 0; i < count; i++) {
        int handled = nvfuse_primary_journal_sync(t, (int64_t)w[i].offset, w[i].bytes,
                                                 (void *)w[i].buffer, SPDK_BDEV_IO_TYPE_WRITE);
        if (handled != 1) abort(); /* Preflight and mutation share one owner. */
    }
    return 1;
}
int nvfuse_primary_journal_commit(struct nvfuse_handle *h, int restart)
{
    struct io_target *t = h->nvh_target;
    if (!t->journal) return 0;
    if (t->journal_aio) return -EBUSY;
    uint64_t start = t->journal_profile ? profile_ns() : 0;
    nvfuse_check_flush_dirty(&h->nvh_sb, DIRTY_FLUSH_FORCE);
    uint64_t dirty_end = t->journal_profile ? profile_ns() : 0;
    int rc = nvfuse_sync_superblock(&h->nvh_sb);
    if (t->journal_profile) {
        uint64_t end = profile_ns();
        t->journal_dirty_ns += dirty_end - start;
        t->journal_super_ns += end - dirty_end;
        t->journal_collect_ns += end - start;
    }
    if (!rc) rc = nvj_commit(t->journal);
    if (rc) { nvj_require_recovery(t->journal); return rc; }
    t->journal_dirty = 0;
    return restart ? nvj_begin(t->journal, 0, 0) : 0;
}
int nvfuse_primary_journal_read_header(struct io_target *t, void *buf, size_t bytes)
{
    if (!t->journal || t->journal_raw || bytes < sizeof(struct nvfuse_superblock_common)) return 0;
    struct nvfuse_superblock_common *disk = buf;
    if (disk->sb_signature != NVJ_SIGNATURE) return -EIO;
    disk->sb_signature = NVFUSE_SB_SIGNATURE;
    if (t->journal_mounting && disk->sb_state == FS_STATE_MOUNTED)
        disk->sb_state = FS_STATE_UMOUNTED;
    return 0;
}
void nvfuse_primary_journal_release(struct io_target *t)
{
    if (t->journal) {
        printf("NVFUSE_FLUSH no_volatile_cache_barriers=%" PRIu64 " enabled=%u\n",
               t->journal_flush_elided, t->journal_flush_fast);
        printf("NVFUSE_FAST_APPEND calls=%" PRIu64 " hits=%" PRIu64 "\n",
               t->buffered_fast_append_calls, t->buffered_fast_append_hits);
        printf("NVFUSE_DIRTY_IO blocks=%" PRIu64 " requests=%" PRIu64 " iov=%u\n", t->journal_dirty_blocks,t->journal_dirty_requests,t->journal_dirty_iov);
        const struct nvj_stats *s = nvj_stats(t->journal);
        printf("NVFUSE_JOURNAL_STATS commits=%" PRIu64 " replays=%" PRIu64
               " journal_bytes=%" PRIu64 " home_bytes=%" PRIu64 " flushes=%" PRIu64
               " peak_blocks=%" PRIu64 " home_batches=%" PRIu64 " home_requests=%" PRIu64
               " log_join_commits=%" PRIu64 " log_join_blocks=%" PRIu64
               "\n", s->commits, s->replays, s->journal_bytes,
               s->home_bytes, s->flushes, s->peak_blocks, s->home_batches, s->home_requests,
               s->log_join_commits, s->log_join_blocks);
        nvj_close(t->journal); t->journal = NULL;
    }
    /* CQ publication can precede the submit/drain callback returning (notably
     * an inline no-op flush). Fence that callback before destroying task locks. */
    if (t->journal_raw_task || t->journal_home_task || t->journal_dirty_task) reactor_wait_idle(t);
    if (t->journal_raw_task) {
        reactor_free_task(t, t->journal_raw_task); t->journal_raw_task = NULL;
    }
    if (t->journal_home_task) {
        reactor_free_task(t, t->journal_home_task); t->journal_home_task = NULL;
    }
    if (t->journal_dirty_task) {
        reactor_free_task(t, t->journal_dirty_task); t->journal_dirty_task = NULL;
    }
    spdk_dma_free(t->journal_dma); t->journal_dma = NULL;
}
