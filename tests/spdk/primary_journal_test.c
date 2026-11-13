/* Actual primary-reactor filesystem crash/reopen test, private fixtures only.
 * The independent journal persistence oracle covers torn/unflushed I/O; these
 * process exits validate the real adapter, allocator and namespace integration. */
#include "spdk/stdinc.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_inode_cache.h"
#include "nvfuse_primary_journal.h"
#include "runtime.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return -EIO; } } while (0)
static const char *phase;
static void crash(void *arg, const char *name, uint64_t block)
{
    (void)arg; (void)block;
    if (!strcmp(phase, name)) {
        fprintf(stderr, "EXPECTED_CRASH phase=%s\n", name); fflush(NULL); _exit(86);
    }
}
static int fail_payload(void *arg, uint64_t offset, size_t bytes, int write_op)
{
    (void)arg; (void)bytes;
    return write_op && offset == (uint64_t)(NVJ_START + NVJ_PAYLOAD) * CLUSTER_SIZE ? -EIO : 0;
}
static int put(struct nvfuse_handle *h, const char *path, int value)
{
    int fd = nvfuse_openfile_path(h, path, O_CREAT | O_RDWR, 0644);
    CHECK(fd >= 0);
    char data[65536]; memset(data, value, sizeof(data));
    CHECK(nvfuse_writefile(h, fd, data, sizeof(data), 0) == sizeof(data));
    CHECK(nvfuse_closefile(h, fd) == 0);
    return 0;
}
static int get(struct nvfuse_handle *h, const char *path, int value)
{
    struct stat st; CHECK(!nvfuse_getattr(h, path, &st) && st.st_size == 65536);
    int fd = nvfuse_openfile_path(h, path, O_RDONLY, 0); CHECK(fd >= 0);
    char data[65536]; CHECK(nvfuse_readfile(h, fd, data, sizeof(data), 0) == sizeof(data));
    for (unsigned i = 0; i < sizeof(data); i++) CHECK(data[i] == value);
    CHECK(!nvfuse_closefile(h, fd)); return 0;
}
/* Persist a freed inode containing old data pointers, then reuse it in a fresh
 * process. Partial writes must never expose bytes from its previous lifetime. */
static int reuse_inode(struct nvfuse_handle *h, const char *mode)
{
    struct stat st;
    inode_t ino;
    if (!strcmp(mode, "reuse-seed")) {
        int marker = nvfuse_openfile_path(h, "/reuse-marker", O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(marker >= 0);
        CHECK(!put(h, "/reuse-old", 0x7e));
        CHECK(!nvfuse_getattr(h, "/reuse-old", &st)); ino = st.st_ino;
        CHECK(nvfuse_writefile(h, marker, (char *)&ino, sizeof(ino), 0) == sizeof(ino));
        CHECK(!nvfuse_closefile(h, marker));
        CHECK(!nvfuse_unlink(h, "/reuse-old"));
        CHECK(!nvfuse_sync(h));
        return 0;
    }
    int marker = nvfuse_openfile_path(h, "/reuse-marker", O_RDONLY, 0);
    CHECK(marker >= 0);
    CHECK(nvfuse_readfile(h, marker, (char *)&ino, sizeof(ino), 0) == sizeof(ino));
    CHECK(!nvfuse_closefile(h, marker));
    int create = !strcmp(mode, "reuse-create");
    if (create) h->nvh_sb.sb_last_allocated_ino = ino;
    int fd = nvfuse_openfile_path(h, "/reuse-new", create ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY, 0640);
    CHECK(fd >= 0);
    CHECK(!nvfuse_getattr(h, "/reuse-new", &st));
    CHECK(st.st_ino == ino && (st.st_mode & 0777) == 0640 && st.st_nlink == 1);
    char data[124];
    if (create) {
        CHECK(st.st_size == 0);
        CHECK(nvfuse_readfile(h, fd, data, sizeof(data), 0) == 0);
        char value = (char)0xa9;
        CHECK(nvfuse_writefile(h, fd, &value, 1, 123) == 1);
    } else CHECK(st.st_size == sizeof(data));
    memset(data, 0x55, sizeof(data));
    CHECK(nvfuse_readfile(h, fd, data, sizeof(data), 0) == sizeof(data));
    for (unsigned i = 0; i < sizeof(data) - 1; i++) CHECK(data[i] == 0);
    CHECK((unsigned char)data[123] == 0xa9);
    CHECK(!nvfuse_closefile(h, fd));
    CHECK(nvfuse_getattr(h, "/reuse-old", &st) < 0);
    CHECK(!nvfuse_sync(h));
    return 0;
}
static int run(struct nvfuse_handle *h, void *argument)
{
    const char *mode = argument;
    CHECK(h->nvh_target->journal);
    if (!strncmp(mode, "reuse-", 6)) {
        CHECK(!strcmp(mode, "reuse-seed") || !strcmp(mode, "reuse-create") || !strcmp(mode, "reuse-check"));
        return reuse_inode(h, mode);
    } else if (!strcmp(mode, "format")) {
        CHECK(!put(h, "/stable", 0x51)); CHECK(!nvfuse_sync(h));
    } else if (!strcmp(mode, "stamp-old")) {
        struct stat st; CHECK(!nvfuse_getattr(h, "/stable", &st));
        struct nvfuse_inode_ctx *ctx = nvfuse_read_inode(&h->nvh_sb, NULL, st.st_ino);
        CHECK(ctx);
        ctx->ictx_inode->i_mtime = ctx->ictx_inode->i_ctime = 1;
        nvfuse_release_inode(&h->nvh_sb, ctx, DIRTY);
        CHECK(!nvfuse_sync(h));
    } else if (!strcmp(mode, "check-aio-times")) {
        struct stat st; CHECK(!nvfuse_getattr(h, "/stable", &st));
        CHECK(st.st_size == 65536 && st.st_mtime > 1 && st.st_ctime > 1);
    } else if (!strcmp(mode, "mutate") || !strcmp(mode, "mutate-checkpoint")) {
        CHECK(!put(h, "/new", 0x62));
        CHECK(!nvfuse_rename_path(h, "/stable", "/moved"));
        nvj_set_hook(h->nvh_target->journal, crash, NULL);
        CHECK(!nvfuse_sync(h));
        if (!strcmp(mode, "mutate-checkpoint")) {
            nvj_discard(h->nvh_target->journal); /* Empty TX opened by sync. */
            CHECK(!nvj_checkpoint(h->nvh_target->journal));
            CHECK(!nvj_begin(h->nvh_target->journal, 0, 0));
        }
        CHECK(!phase || !*phase); /* An unknown phase is a failed test. */
    } else if (!strcmp(mode, "io-fault")) {
        CHECK(!put(h, "/new", 0x62));
        CHECK(!nvfuse_rename_path(h, "/stable", "/moved"));
        nvj_set_io_fault_hook(h->nvh_target->journal, fail_payload, NULL);
        CHECK(nvfuse_sync(h) < 0);
        CHECK(nvj_recovery_required(h->nvh_target->journal));
        fprintf(stderr, "EXPECTED_IO_FAILURE recovery_required=1\n"); fflush(NULL); _exit(87);
    } else {
        int committed = !strcmp(mode, "check-new");
        if (!strcmp(mode, "check-either")) {
            struct stat probe;
            committed = !nvfuse_getattr(h, "/moved", &probe);
            printf("RECOVERED_NAMESPACE state=%s\n", committed ? "new" : "old");
        }
        struct stat st;
        CHECK(!get(h, committed ? "/moved" : "/stable", 0x51));
        CHECK(nvfuse_getattr(h, committed ? "/stable" : "/moved", &st) < 0);
        if (committed) CHECK(!get(h, "/new", 0x62));
        else CHECK(nvfuse_getattr(h, "/new", &st) < 0);
    }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc < 4 || argc > 5) return 2;
    phase = argc == 5 ? argv[4] : "";
    int rc = nvfio_start(argv[1], argv[2], !strcmp(argv[3], "format"));
    if (rc) return 3;
    rc = nvfio_execute(run, argv[3]);
    /* Failed assertions must not enter SPDK's process-exit handlers while the
     * application thread is still running. Preserve the committed journal for
     * the next process, as the intentional crash cases do. */
    if (rc) { fflush(NULL); _exit(1); }
    if (!rc) rc = nvfio_stop();
    if (!rc) printf("PASS primary journal %s\n", argv[3]);
    return rc ? 1 : 0;
}
