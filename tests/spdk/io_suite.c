/* SPDX-License-Identifier: GPL-2.0-only */

/* Direct SPDK tests, independent of NVFUSE's legacy SPDK adapter. */
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"

#define MAX_JOBS 256
#define MAX_BYTES 65536
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static struct spdk_nvme_ctrlr *controller;
static struct spdk_nvme_ns *ns;
static struct spdk_nvme_transport_id trid;
static uint32_t random_state;
static bool removed;
static uint64_t submitted_ios, completed_ios;

struct job {
    struct spdk_nvme_qpair *qpair;
    unsigned char *expected, *actual;
    uint64_t lba;
    uint32_t blocks, bytes;
    int phase, done, error, expect_error;
};
static uint32_t random32(void) { random_state ^= random_state << 13; random_state ^= random_state >> 17; random_state ^= random_state << 5; return random_state; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void completion(void *arg, const struct spdk_nvme_cpl *cpl)
{
    struct job *job = arg;
    CHECK(!job->done);
    completed_ios++;
    job->error = spdk_nvme_cpl_is_error(cpl);
    if (job->expect_error) {
        CHECK(job->error);
        CHECK(cpl->status.sct == SPDK_NVME_SCT_GENERIC);
        CHECK(cpl->status.sc == SPDK_NVME_SC_LBA_OUT_OF_RANGE || cpl->status.sc == SPDK_NVME_SC_INVALID_FIELD);
        job->done = 1;
        return;
    }
    if (job->error) { job->done = 1; return; }
    if (job->phase == 0) {
        job->phase = 1;
        CHECK(spdk_nvme_ns_cmd_read(ns, job->qpair, job->actual, job->lba, job->blocks,
                                   completion, job, 0) == 0);
        submitted_ios++;
    } else {
        CHECK(!memcmp(job->actual, job->expected, job->bytes));
        job->done = 1;
    }
}
static void remove_cb(void *ctx, struct spdk_nvme_ctrlr *ctrlr) { (void)ctx; (void)ctrlr; removed = true; }
static bool probe(void *ctx, const struct spdk_nvme_transport_id *id, struct spdk_nvme_ctrlr_opts *opts)
{
    (void)ctx; (void)opts;
    return !strcmp(id->traddr, trid.traddr);
}
static void attach(void *ctx, const struct spdk_nvme_transport_id *id,
                   struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
    (void)ctx; (void)id; (void)opts;
    CHECK(!controller);
    const struct spdk_nvme_ctrlr_data *data = spdk_nvme_ctrlr_get_data(ctrlr);
    CHECK(!memcmp(data->sn, "NVFUSEDEV0001", 13));
    controller = ctrlr;
    ns = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
    CHECK(ns && spdk_nvme_ns_is_active(ns));
    CHECK(spdk_nvme_ns_get_size(ns) == UINT64_C(2147483648));
}
static struct spdk_nvme_qpair *new_queue(void)
{
    struct spdk_nvme_io_qpair_opts opts;
    spdk_nvme_ctrlr_get_default_io_qpair_opts(controller, &opts, sizeof(opts));
    opts.io_queue_size = 128;
    opts.io_queue_requests = 256;
    struct spdk_nvme_qpair *q = spdk_nvme_ctrlr_alloc_io_qpair(controller, &opts, sizeof(opts));
    CHECK(q);
    return q;
}
static void wait_jobs(struct job *jobs, unsigned count, struct spdk_nvme_qpair **queues, unsigned nqueues)
{
    double until = now() + 20;
    for (;;) {
        unsigned done = 0;
        for (unsigned i = 0; i < nqueues; i++) CHECK(spdk_nvme_qpair_process_completions(queues[i], 0) >= 0);
        for (unsigned i = 0; i < count; i++) done += jobs[i].done;
        if (done == count) break;
        CHECK(now() < until);
    }
}
static void prepare(struct job *job, struct spdk_nvme_qpair *q, uint64_t lba, uint32_t blocks, unsigned pattern)
{
    memset(job, 0, sizeof(*job));
    job->qpair = q; job->lba = lba; job->blocks = blocks;
    job->bytes = blocks * spdk_nvme_ns_get_sector_size(ns);
    CHECK(job->bytes <= MAX_BYTES);
    job->expected = spdk_dma_zmalloc(MAX_BYTES, 4096, NULL);
    job->actual = spdk_dma_zmalloc(MAX_BYTES, 4096, NULL);
    CHECK(job->expected && job->actual);
    for (unsigned i = 0; i < job->bytes; i++)
        job->expected[i] = pattern % 4 == 0 ? 0 : pattern % 4 == 1 ? 255 :
                           pattern % 4 == 2 ? (i & 1 ? 0xaa : 0x55) : random32();
}
static void release(struct job *job) { spdk_dma_free(job->expected); spdk_dma_free(job->actual); }

static void matrix(void)
{
    struct spdk_nvme_qpair *queues[4];
    const unsigned depths[] = {1, 8, 32, 64};
    const unsigned blocks[] = {1, 2, 7, 8, 9, 31, 127, 128};
    unsigned blocksize = spdk_nvme_ns_get_sector_size(ns);
    CHECK(blocksize == 512);
    uint64_t requests = 0;
    for (unsigned nq = 1; nq <= 4; nq *= 2) {
        for (unsigned i = 0; i < nq; i++) queues[i] = new_queue();
        for (unsigned d = 0; d < sizeof(depths) / sizeof(depths[0]); d++) {
            unsigned depth = depths[d], count = depth * nq;
            CHECK(count <= MAX_JOBS);
            for (unsigned round = 0; round < 3; round++) {
                struct job jobs[MAX_JOBS];
                unsigned rotation = random32() % 257;
                for (unsigned i = 0; i < count; i++) {
                    /* Distinct randomized slots prevent concurrent write/read races. */
                    uint64_t slot = (i * 73 + rotation) % 257;
                    uint64_t lba = 1024 + slot * (MAX_BYTES / blocksize + 1) + (round & 1);
                    prepare(&jobs[i], queues[i % nq], lba, blocks[random32() % 8], i + round);
                    CHECK(spdk_nvme_ns_cmd_write(ns, jobs[i].qpair, jobs[i].expected,
                          jobs[i].lba, jobs[i].blocks, completion, &jobs[i], 0) == 0);
                    submitted_ios++;
                }
                wait_jobs(jobs, count, queues, nq);
                for (unsigned i = 0; i < count; i++) { CHECK(!jobs[i].error && jobs[i].phase == 1); release(&jobs[i]); }
                requests += 2 * count;
                CHECK(submitted_ios == requests && completed_ios == requests);
            }
            printf("PASS queues=%u depth=%u submitted=completed=%u\n", nq, depth, depth * nq * 6);
            fflush(stdout);
        }
        for (unsigned i = 0; i < nq; i++) CHECK(spdk_nvme_ctrlr_free_io_qpair(queues[i]) == 0);
    }
    printf("PASS matrix requests=%" PRIu64 "\n", requests);
}
static void boundaries(void)
{
    struct spdk_nvme_qpair *q = new_queue();
    struct job job;
    uint64_t last = spdk_nvme_ns_get_num_sectors(ns) - 1;
    for (unsigned i = 0; i < 2; i++) {
        prepare(&job, q, i ? last : 0, 1, 3);
        CHECK(spdk_nvme_ns_cmd_write(ns, q, job.expected, job.lba, 1, completion, &job, 0) == 0);
        wait_jobs(&job, 1, &q, 1); CHECK(!job.error); release(&job);
    }
    for (unsigned write_op = 0; write_op < 2; write_op++) {
        for (unsigned cross = 0; cross < 2; cross++) {
            prepare(&job, q, cross ? last : last + 1, cross ? 2 : 1, 3);
            job.expect_error = 1; job.phase = 1;
            int rc = write_op ? spdk_nvme_ns_cmd_write(ns, q, job.expected, job.lba, job.blocks, completion, &job, 0)
                              : spdk_nvme_ns_cmd_read(ns, q, job.actual, job.lba, job.blocks, completion, &job, 0);
            CHECK(rc == 0 || rc == -EINVAL);
            if (!rc) wait_jobs(&job, 1, &q, 1);
            release(&job);
        }
    }
    CHECK(spdk_nvme_ctrlr_free_io_qpair(q) == 0);
    puts("PASS boundaries first/last LBA and invalid read/write ranges");
}
static void io_error(void *arg, const struct spdk_nvme_cpl *cpl)
{
    struct job *job = arg;
    CHECK(!job->done);
    job->error = spdk_nvme_cpl_is_error(cpl);
    job->done = 1;
}
static void injected(void)
{
    struct spdk_nvme_qpair *q = new_queue();
    struct job job;
    prepare(&job, q, 256, 1, 3);
    CHECK(spdk_nvme_ns_cmd_write(ns, q, job.expected, job.lba, 1, io_error, &job, 0) == 0);
    wait_jobs(&job, 1, &q, 1); CHECK(!job.error);
    job.done = 0;
    CHECK(spdk_nvme_ns_cmd_read(ns, q, job.actual, job.lba, 1, io_error, &job, 0) == 0);
    wait_jobs(&job, 1, &q, 1); CHECK(job.error);
    puts("PASS injected read returned NVMe error");
    job.done = 0; memset(job.actual, 0, job.bytes);
    CHECK(spdk_nvme_ns_cmd_read(ns, q, job.actual, job.lba, 1, io_error, &job, 0) == 0);
    wait_jobs(&job, 1, &q, 1); CHECK(!job.error && !memcmp(job.expected, job.actual, job.bytes));
    release(&job); CHECK(spdk_nvme_ctrlr_free_io_qpair(q) == 0);
    puts("PASS injected transient error recovery");
}
static void hotremove(void)
{
    struct spdk_nvme_qpair *q = new_queue();
    struct job job;
    prepare(&job, q, 256, 1, 3);
    puts("READY hotremove"); fflush(stdout);
    double until = now() + 25;
    uint64_t submitted = 0, completed = 0;
    while (now() < until) {
        job.done = 0;
        int rc = spdk_nvme_ns_cmd_read(ns, q, job.actual, job.lba, 1, io_error, &job, 0);
        if (rc < 0) { CHECK(removed || spdk_nvme_ctrlr_is_failed(controller)); break; }
        submitted++;
        while (!job.done && now() < until) {
            (void)spdk_nvme_qpair_process_completions(q, 0);
            (void)spdk_nvme_scan_attached(&trid);
        }
        CHECK(job.done); completed++;
        if (job.error || removed || spdk_nvme_ctrlr_is_failed(controller)) break;
    }
    CHECK(removed || spdk_nvme_ctrlr_is_failed(controller));
    CHECK(submitted == completed);
    release(&job);
    CHECK(spdk_nvme_ctrlr_free_io_qpair(q) == 0);
    printf("PASS hotremove submitted=completed=%" PRIu64 "\n", completed);
}
static bool timeout_seen;
static void timeout_cb(void *arg, struct spdk_nvme_ctrlr *ctrlr,
                       struct spdk_nvme_qpair *qpair, uint16_t cid)
{
    (void)arg; (void)ctrlr; (void)qpair; (void)cid;
    timeout_seen = true;
}
static void stalled(void)
{
    struct spdk_nvme_qpair *q = new_queue();
    struct job job;
    prepare(&job, q, 512, 1, 3);
    CHECK(spdk_nvme_ns_cmd_write(ns, q, job.expected, job.lba, 1, io_error, &job, 0) == 0);
    wait_jobs(&job, 1, &q, 1); CHECK(!job.error);
    puts("READY stalled"); fflush(stdout);
    double until = now() + 15;
    while (access("/tmp/nvfuse-stall-go", F_OK) != 0) { CHECK(now() < until); usleep(10000); }
    spdk_nvme_ctrlr_register_timeout_callback(controller, 200000, 0, timeout_cb, NULL);
    for (int attempt = 0; attempt < 4 && !timeout_seen; attempt++) {
        job.done = 0;
        CHECK(spdk_nvme_ns_cmd_read(ns, q, job.actual, job.lba, 1, io_error, &job, 0) == 0);
        while (!timeout_seen && !job.done) {
            CHECK(spdk_nvme_qpair_process_completions(q, 0) >= 0);
            CHECK(now() < until);
        }
    }
    CHECK(timeout_seen && !job.done);
    puts("TIMEOUT observed"); fflush(stdout);
    wait_jobs(&job, 1, &q, 1);
    CHECK(!job.error && !memcmp(job.expected, job.actual, job.bytes));
    release(&job); CHECK(spdk_nvme_ctrlr_free_io_qpair(q) == 0);
    puts("PASS stalled timeout and resume");
}
int main(int argc, char **argv)
{
    CHECK(argc == 4);
    random_state = strtoul(argv[3], NULL, 0); CHECK(random_state);
    struct spdk_env_opts opts = {.opts_size = sizeof(opts)};
    spdk_env_opts_init(&opts); opts.opts_size = sizeof(opts);
    opts.name = "nvfuse_test_suite"; opts.mem_size = 256; opts.core_mask = "0x1";
    CHECK(spdk_env_init(&opts) == 0);
    memset(&trid, 0, sizeof(trid));
    CHECK(spdk_nvme_transport_id_parse(&trid, argv[2]) == 0);
    CHECK(spdk_nvme_probe(&trid, NULL, probe, attach, remove_cb) == 0 && controller);
    if (!strcmp(argv[1], "matrix")) matrix();
    else if (!strcmp(argv[1], "boundaries")) boundaries();
    else if (!strcmp(argv[1], "injected")) injected();
    else if (!strcmp(argv[1], "hotremove")) hotremove();
    else if (!strcmp(argv[1], "stalled")) stalled();
    else CHECK(false);
    CHECK(spdk_nvme_detach(controller) == 0);
    spdk_env_fini();
    return 0;
}
