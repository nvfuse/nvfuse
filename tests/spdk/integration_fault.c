/* Reuse the seeded payload oracle; fault barriers exist only in test builds. */
#define main framework_main
#include "../framework_test.c"
#undef main
#include <time.h>

static const char *fault;
static uint64_t target_lba;
static int armed, pending_submit, want_write;
static unsigned submissions;

static void observe(int submitted, int write_op, uint64_t lba)
{
    if (submitted) {
        if (pending_submit) {
            printf("SUBMITTED fault %s ordinal=%u lba=%" PRIu64 "\n", fault, ++submissions, lba);
            fflush(stdout);
        }
        return;
    }
    if (!armed || write_op != want_write || (target_lba && lba != target_lba)) return;
    armed = 0;
    if (!strcmp(fault, "locate")) {
        printf("LOCATED payload lba=%" PRIu64 "\n", lba);
        fflush(stdout);
        return;
    }
    pending_submit = 1;
    printf("READY fault %s lba=%" PRIu64 "\n", fault, lba);
    fflush(stdout);
    time_t until = time(NULL) + 30;
    while (access("/tmp/nvfuse-fault-go", F_OK)) {
        if (time(NULL) > until) { fprintf(stderr, "fault barrier timed out\n"); exit(2); }
        usleep(10000);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 5);
    fault = argv[1];
    CHECK(!strncmp(argv[2], "pcie:", 5));
    uint32_t seed = strtoul(argv[3], NULL, 0);
    target_lba = strtoull(argv[4], NULL, 0);
    if (!strcmp(fault, "unclean")) {
        void *device = NULL;
        uint64_t size;
        struct nvfuse_superblock_common disk;
        CHECK(nvfuse_nvme_open(argv[2] + 5, &device, &size) == 0);
        CHECK(nvfuse_nvme_transfer(device, 0, sizeof(disk), &disk, 0) == 0);
        CHECK(disk.sb_signature == NVFUSE_SB_SIGNATURE && disk.sb_state == FS_STATE_MOUNTED);
        CHECK(nvfuse_nvme_close(device) == 0);
        struct nvfuse_handle *h = nvfuse_file_open(argv[2], 0, 0);
        CHECK(!h && errno == EINVAL);
        puts("PASS unclean mounted image rejected");
        return 0;
    }
    CHECK(!strcmp(fault, "locate") || !strcmp(fault, "read-eio") ||
          !strcmp(fault, "write-eio") || !strcmp(fault, "stall-resume") ||
          !strcmp(fault, "stall-timeout") || !strcmp(fault, "hotremove"));
    struct nvfuse_handle *h = nvfuse_file_open(argv[2], 0, 0);
    CHECK(h);
    int fd = nvfuse_openfile_path(h, "/payload", O_RDWR, 0);
    CHECK(fd >= 0);
    char data[8192];
    want_write = !strcmp(fault, "write-eio");
    size_t bytes = want_write ? 4096 : sizeof(data);
    /* A full-block overwrite avoids a read-modify-write. blkdebug arms an
     * error at its event, then applies it to the next matching-sector I/O. */
    if (want_write) memset(data, 0x5a, sizeof(data));
    reactor_sync_flush(h->nvh_target);
    armed = 1;
    nvfuse_nvme_test_observer(observe);
    if (want_write) {
        CHECK(nvfuse_writefile(h, fd, data, bytes, 0) == (int)bytes);
        CHECK(nvfuse_sync(h) == 0);
        CHECK(nvfuse_fsync(h, fd) == 0);
    } else CHECK(nvfuse_readfile(h, fd, data, bytes, 0) == (int)bytes);
    CHECK(!armed);
    nvfuse_nvme_test_observer(NULL);
    if (strcmp(fault, "locate") && strcmp(fault, "stall-resume")) {
        fprintf(stderr, "fault unexpectedly returned success: %s\n", fault);
        /* Do not run SPDK's atexit teardown with a deliberately live handle. */
        _Exit(1);
    }
    CHECK(nvfuse_closefile(h, fd) == 0);
    CHECK(workload(h, seed, 0) == 0);
    CHECK(nvfuse_file_close(h) == 0);
    printf("PASS fault %s full payload compared\n", fault);
    return 0;
}
