/* SPDX-License-Identifier: GPL-2.0-only */

/* Exercise persistent raw-task completion failures on disposable bdev images. */
int primary_journal_test_main(int, char **);
#define main primary_journal_test_main
#include "primary_journal_test.c"
#undef main
#include "module/bdev/error/vbdev_error.h"
#include <semaphore.h>

struct raw_fault {
    struct nvfuse_handle *h;
    const char *boundary;
    unsigned type, seen, occurrence;
    sem_t done;
    int rc;
};
static void arm_raw_fault(void *arg)
{
    struct raw_fault *f = arg;
    struct vbdev_error_inject_opts opts = {
        .io_type = f->type, .error_type = SPDK_BDEV_ERROR_TYPE_FAILURE, .error_num = 1
    };
    f->rc = vbdev_error_inject_error(getenv("NVFUSE_SPDK_BDEV"), &opts);
    sem_post(&f->done);
}
static void raw_boundary(void *arg, const char *name, uint64_t block)
{
    struct raw_fault *f = arg; (void)block;
    if (strcmp(name, f->boundary) || ++f->seen != f->occurrence) return;
    nvj_set_hook(f->h->nvh_target->journal, NULL, NULL);
    f->rc = spdk_thread_send_msg(f->h->nvh_target->io_thread, arm_raw_fault, f);
    if (!f->rc) while (sem_wait(&f->done) && errno == EINTR) {}
}
static int raw_fault_run(struct nvfuse_handle *h, void *arg)
{
    const char *mode = arg;
    struct io_target *t = h->nvh_target;
    CHECK(t->journal && t->journal_raw_task);
    CHECK(!put(h, "/new", 0x62));
    CHECK(!nvfuse_rename_path(h, "/stable", "/moved"));
    struct raw_fault f = {.h = h, .occurrence = 2,
        .boundary = "prepare-flush", .type = SPDK_BDEV_IO_TYPE_WRITE};
    int checkpoint = !strcmp(mode, "raw-v3-home-fault") || !strcmp(mode, "raw-v3-epoch-fault");
    if (!strcmp(mode, "raw-v3-log-fault")) {
        f.boundary = "ordered-data-flush"; f.occurrence = 1;
    } else if (!strcmp(mode, "raw-v3-flush-fault")) {
        f.boundary = "v3-log-write"; f.occurrence = 1; f.type = SPDK_BDEV_IO_TYPE_FLUSH;
    } else if (checkpoint) {
        f.boundary = !strcmp(mode, "raw-v3-home-fault") ? "v3-durable" : "v3-home-flush";
        f.occurrence = 1;
    } else if (!strcmp(mode, "raw-flush-fault")) {
        f.boundary = "descriptor-write"; f.occurrence = 1;
        f.type = SPDK_BDEV_IO_TYPE_FLUSH;
    } else if (!strcmp(mode, "raw-retire-fault")) {
        f.boundary = "checkpoint-flush"; f.occurrence = 1;
    }
    CHECK(!sem_init(&f.done, 0, 0));
    nvj_set_hook(t->journal, raw_boundary, &f);
    if (checkpoint) {
        CHECK(!nvfuse_sync(h));
        nvj_discard(t->journal); /* The empty restart transaction. */
        CHECK(nvj_checkpoint(t->journal) == -EIO);
    } else CHECK(nvfuse_sync(h) == -EIO);
    CHECK(!f.rc && f.seen == f.occurrence && nvj_recovery_required(t->journal));
    CHECK(!t->current_queue_depth && !reactor_cq_size(t->journal_raw_task));
    reactor_wait_idle(t);
    CHECK(t->journal_raw_task->sq_head == t->journal_raw_task->sq_tail);
    CHECK(!sem_destroy(&f.done));
    fprintf(stderr, "EXPECTED_RAW_IO_FAILURE mode=%s drained=1 recovery_required=1\n", mode);
    fflush(NULL); _exit(88);
}
static int reuse_run(struct nvfuse_handle *h, void *unused)
{
    (void)unused;
    struct io_target *t = h->nvh_target;
    struct reactor_task *task = t->journal_raw_task;
    const char *poll = getenv("NVFUSE_JOURNAL_RAW_POLL");
    CHECK(task && task->polling == (!poll || strcmp(poll, "0")));
    for (unsigned i = 0; i < 128; i++) {
        int fd = nvfuse_openfile_path(h, "/raw-reuse", O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(fd >= 0 && !nvfuse_closefile(h, fd) && !nvfuse_sync(h));
        CHECK(!nvfuse_rename_path(h, "/raw-reuse", "/raw-renamed") && !nvfuse_sync(h));
        CHECK(!nvfuse_unlink(h, "/raw-renamed") && !nvfuse_sync(h));
        CHECK(t->journal_raw_task == task && !reactor_cq_size(task));
    }
    reactor_wait_idle(t);
    CHECK(task->sq_head == task->sq_tail);
    puts("PASS raw task reused through 384 durable namespace operations");
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 4 || (strncmp(argv[3], "raw-", 4)))
        return primary_journal_test_main(argc, argv);
    int rc = nvfio_start(argv[1], argv[2], 0);
    if (!rc) rc = nvfio_execute(!strcmp(argv[3], "raw-reuse") ? reuse_run : raw_fault_run, argv[3]);
    if (rc) { fflush(NULL); _exit(1); }
    return nvfio_stop() ? 1 : 0;
}
