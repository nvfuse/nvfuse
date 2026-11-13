/* Reuse the namespace/payload recovery oracle; only this test executable links
 * the SPDK error-injection API. The failure occurs in actual bdev completions. */
#define main primary_journal_test_main
#include "primary_journal_test.c"
#undef main
#include "module/bdev/error/vbdev_error.h"
#include <semaphore.h>
struct home_fault { struct nvfuse_handle *h; sem_t done; unsigned certificates; int rc; };
static void inject_home_error(void *argument)
{
    struct home_fault *f = argument;
    struct vbdev_error_inject_opts opts = {
        .io_type = SPDK_BDEV_IO_TYPE_WRITE,
        .error_type = SPDK_BDEV_ERROR_TYPE_FAILURE, .error_num = 1
    };
    f->rc = vbdev_error_inject_error(getenv("NVFUSE_SPDK_BDEV"), &opts);
    sem_post(&f->done);
}
static void after_certificate(void *argument, const char *name, uint64_t block)
{
    struct home_fault *f = argument; (void)block;
    if (strcmp(name, "commit-flush") || ++f->certificates != 2) return;
    /* Both commit certificates are durable. Remove the diagnostic hook so
     * the real scatter path runs, then arm exactly one failed home write. */
    nvj_set_hook(f->h->nvh_target->journal, NULL, NULL);
    f->rc = spdk_thread_send_msg(f->h->nvh_target->io_thread, inject_home_error, f);
    if (!f->rc) while (sem_wait(&f->done) && errno == EINTR) {}
}
static int home_fault_run(struct nvfuse_handle *h, void *unused)
{
    (void)unused;
    struct io_target *t = h->nvh_target;
    CHECK(t->journal && t->journal_home_task);
    CHECK(!put(h, "/new", 0x62));
    CHECK(!nvfuse_rename_path(h, "/stable", "/moved"));
    uint64_t before = nvj_stats(t->journal)->home_batches;
    struct home_fault f = {.h = h}; CHECK(!sem_init(&f.done, 0, 0));
    nvj_set_hook(t->journal, after_certificate, &f);
    CHECK(nvfuse_sync(h) == -EIO);
    CHECK(!f.rc && f.certificates == 2 && nvj_recovery_required(t->journal));
    CHECK(nvj_stats(t->journal)->home_batches > before);
    CHECK(!t->current_queue_depth); /* Every submitted job was completed. */
    CHECK(!sem_destroy(&f.done));
    fprintf(stderr, "EXPECTED_HOME_IO_FAILURE scatter=1 drained=1 recovery_required=1\n");
    fflush(NULL); _exit(87);
}
int main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[3], "home-io-fault")) return primary_journal_test_main(argc, argv);
    int rc = nvfio_start(argv[1], argv[2], 0);
    if (!rc) rc = nvfio_execute(home_fault_run, NULL);
    if (rc) { fflush(NULL); _exit(1); }
    return 1; /* The injected failure must exit inside the owner callback. */
}
