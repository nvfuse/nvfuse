/* SPDX-License-Identifier: GPL-2.0-only */

/* Actual primary-reactor filesystem crash/reopen test, private fixtures only.
 * The independent journal persistence oracle covers torn/unflushed I/O; these
 * process exits validate the real adapter, allocator and namespace integration. */
#include "spdk/stdinc.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_inode_cache.h"
#include "nvfuse_indirect.h"
#include "nvfuse_primary_journal.h"
#include "nvfuse_spdk_runtime.h"
#define nvfio_execute nvfio_execute_serialized
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
    (void)bytes;
    int payload = nvj_version(arg) == 3 ?
        offset >= (uint64_t)(NVJ_START + 3) * CLUSTER_SIZE &&
        offset < (uint64_t)(NVJ_START + NVJ_BLOCKS) * CLUSTER_SIZE :
        offset == (uint64_t)(NVJ_START + NVJ_PAYLOAD) * CLUSTER_SIZE;
    return write_op && payload ? -EIO : 0;
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
        int marker = nvfuse_openfile_path(h, "/primary-epoch-reuse-marker", O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(marker >= 0);
        CHECK(!put(h, "/primary-epoch-reuse-old", 0x7e));
        CHECK(!nvfuse_getattr(h, "/primary-epoch-reuse-old", &st)); ino = st.st_ino;
        CHECK(nvfuse_writefile(h, marker, (char *)&ino, sizeof(ino), 0) == sizeof(ino));
        CHECK(!nvfuse_closefile(h, marker));
        CHECK(!nvfuse_unlink(h, "/primary-epoch-reuse-old"));
        CHECK(!nvfuse_sync(h));
        return 0;
    }
    int marker = nvfuse_openfile_path(h, "/primary-epoch-reuse-marker", O_RDONLY, 0);
    CHECK(marker >= 0);
    CHECK(nvfuse_readfile(h, marker, (char *)&ino, sizeof(ino), 0) == sizeof(ino));
    CHECK(!nvfuse_closefile(h, marker));
    int create = !strcmp(mode, "reuse-create");
    if (create) h->nvh_sb.sb_last_allocated_ino = ino;
    int fd = nvfuse_openfile_path(h, "/primary-epoch-reuse-new", create ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY, 0640);
    CHECK(fd >= 0);
    CHECK(!nvfuse_getattr(h, "/primary-epoch-reuse-new", &st));
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
    CHECK(nvfuse_getattr(h, "/primary-epoch-reuse-old", &st) < 0);
    CHECK(!nvfuse_sync(h));
    return 0;
}
/* Small WAL-like appends cross direct/indirect and partial-block boundaries.
 * An overwrite straddling a block boundary must preserve surrounding bytes. */
static int partial_writes(struct nvfuse_handle *h, int create)
{
    const size_t bytes = 80 * CLUSTER_SIZE + 731;
    char *expected = malloc(bytes), *actual = malloc(bytes);
    CHECK(expected && actual);
    for (size_t i = 0; i < bytes; i++) expected[i] = (char)(i * 37 + i / 251);
    int fd = nvfuse_openfile_path(h, "/primary-epoch-partial-appends",
        create ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY, 0600);
    CHECK(fd >= 0);
    if (create) {
        for (size_t off = 0; off < bytes;) {
            size_t n = bytes - off < 1021 ? bytes - off : 1021;
            CHECK(nvfuse_writefile(h, fd, expected + off, n, off) == (s32)n);
            off += n;
            CHECK(!nvfuse_fsync(h, fd));
        }
        memset(expected + CLUSTER_SIZE - 17, 0xa9, 71);
        CHECK(nvfuse_writefile(h, fd, expected + CLUSTER_SIZE - 17,
            71, CLUSTER_SIZE - 17) == 71);
        CHECK(!nvfuse_fsync(h, fd));
    } else memset(expected + CLUSTER_SIZE - 17, 0xa9, 71);
    struct stat st;
    CHECK(!nvfuse_getattr(h, "/primary-epoch-partial-appends", &st) && st.st_size == bytes);
    CHECK(nvfuse_readfile(h, fd, actual, bytes, 0) == bytes);
    CHECK(!memcmp(actual, expected, bytes));
    if (create) {
        int other = nvfuse_openfile_path(h, "/primary-epoch-partial-appends", O_RDWR, 0);
        const size_t short_bytes = CLUSTER_SIZE + 113;
        CHECK(other >= 0 && !nvfuse_ftruncate(h, other, short_bytes));
        CHECK(nvfuse_readfile(h, fd, actual, bytes, 0) == (s32)short_bytes);
        CHECK(!memcmp(actual, expected, short_bytes));
        CHECK(nvfuse_writefile(h, fd, expected + short_bytes,
            bytes - short_bytes, short_bytes) == (s32)(bytes - short_bytes));
        CHECK(!nvfuse_fsync(h, fd));
        CHECK(nvfuse_readfile(h, fd, actual, bytes, 0) == (s32)bytes);
        CHECK(!memcmp(actual, expected, bytes));
        CHECK(!nvfuse_closefile(h, other));
    }
    CHECK(!nvfuse_closefile(h, fd));
    free(actual); free(expected);
    return 0;
}
static int partial_writes_long(struct nvfuse_handle *h, int create)
{
    const size_t bytes = 1100 * CLUSTER_SIZE + 731;
    char *expected = malloc(bytes), *actual = malloc(bytes);
    CHECK(expected && actual);
    for (size_t i = 0; i < bytes; i++) expected[i] = (char)(i * 37 + i / 251);
    int fd = nvfuse_openfile_path(h, "/primary-epoch-partial-appends",
        create ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY, 0600);
    CHECK(fd >= 0);
    if (create) {
        for (size_t off = 0; off < bytes;) {
            size_t n = bytes - off < 1021 ? bytes - off : 1021;
            CHECK(nvfuse_writefile(h, fd, expected + off, n, off) == (s32)n);
            off += n;
            CHECK(!nvfuse_fsync(h, fd));
        }
        memset(expected + CLUSTER_SIZE - 17, 0xa9, 71);
        CHECK(nvfuse_writefile(h, fd, expected + CLUSTER_SIZE - 17,
            71, CLUSTER_SIZE - 17) == 71);
        CHECK(!nvfuse_fsync(h, fd));
    } else memset(expected + CLUSTER_SIZE - 17, 0xa9, 71);
    struct stat st;
    CHECK(!nvfuse_getattr(h, "/primary-epoch-partial-appends", &st) && st.st_size == bytes);
    CHECK(nvfuse_readfile(h, fd, actual, bytes, 0) == bytes);
    CHECK(!memcmp(actual, expected, bytes));
    if (create) {
        int other = nvfuse_openfile_path(h, "/primary-epoch-partial-appends", O_RDWR, 0);
        const size_t short_bytes = CLUSTER_SIZE + 113;
        CHECK(other >= 0 && !nvfuse_ftruncate(h, other, short_bytes));
        CHECK(nvfuse_readfile(h, fd, actual, bytes, 0) == (s32)short_bytes);
        CHECK(!memcmp(actual, expected, short_bytes));
        CHECK(nvfuse_writefile(h, fd, expected + short_bytes,
            bytes - short_bytes, short_bytes) == (s32)(bytes - short_bytes));
        CHECK(!nvfuse_fsync(h, fd));
        CHECK(nvfuse_readfile(h, fd, actual, bytes, 0) == (s32)bytes);
        CHECK(!memcmp(actual, expected, bytes));
        CHECK(!nvfuse_closefile(h, other));
    }
    CHECK(!nvfuse_closefile(h, fd));
    free(actual); free(expected);
    return 0;
}
/* A freed indirect block must not retain a dirty physical-cache alias when
 * the same sector becomes another file's data. Check cache state and bytes. */
static int block_reuse(struct nvfuse_handle *h, int create)
{
    struct nvfuse_superblock *sb = &h->nvh_sb;
    char data[65536];
    if (create) {
        int old = nvfuse_openfile_path(h, "/primary-epoch-block-reuse-old", O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(old >= 0);
        memset(data, 0x7e, sizeof(data));
        for (size_t off = 0; off < 5 * 1024 * 1024; off += sizeof(data))
            CHECK(nvfuse_writefile(h, old, data, sizeof(data), off) == sizeof(data));
        CHECK(!nvfuse_fsync(h, old));
        struct stat st; CHECK(!nvfuse_getattr(h, "/primary-epoch-block-reuse-old", &st));
        struct nvfuse_inode_ctx *ctx = nvfuse_read_inode(sb, NULL, st.st_ino);
        u32 physical = ctx->ictx_inode->i_blocks[INDIRECT_BLOCKS];
        CHECK(physical && ctx->ictx_inode->i_blocks[DINDIRECT_BLOCKS]);
        nvfuse_release_inode(sb, ctx, NVF_CLEAN);
        CHECK(!nvfuse_ftruncate(h, old, 0));
        u64 key; nvfuse_make_pbno_key(BLOCK_IO_INO, physical, &key, NVFUSE_BP_TYPE_DATA);
        struct nvfuse_buffer_cache *bc = nvfuse_hash_lookup(sb->sb_bm, key);
        CHECK(!bc || (!bc->bc_load && !bc->bc_dirty && !bc->bc_pno));
        int fd = nvfuse_openfile_path(h, "/primary-epoch-block-reuse-data", O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(fd >= 0);
        u32 bg = physical / sb->sb_no_of_blocks_per_bg;
        CHECK(!nvfuse_getattr(h, "/primary-epoch-block-reuse-data", &st));
        sb->sb_last_allocated_bgid = bg;
        sb->sb_last_allocated_bgid_by_ino = st.st_ino;
        struct nvfuse_buffer_head *bh = nvfuse_get_bh(sb, NULL, BD_INO, bg, READ, NVFUSE_TYPE_META);
        ((struct nvfuse_bg_descriptor *)bh->bh_buf)->bd_next_block = physical % sb->sb_no_of_blocks_per_bg;
        nvfuse_release_bh(sb, bh, 0, DIRTY);
        memset(data, 0x5a, sizeof(data));
        CHECK(nvfuse_writefile(h, fd, data, sizeof(data), 0) == sizeof(data));
        CHECK(!nvfuse_getattr(h, "/primary-epoch-block-reuse-data", &st));
        ctx = nvfuse_read_inode(sb, NULL, st.st_ino);
        CHECK(ctx->ictx_inode->i_blocks[0] == physical);
        nvfuse_release_inode(sb, ctx, NVF_CLEAN);
        CHECK(!nvfuse_fsync(h, fd));
        CHECK(!nvfuse_closefile(h, old) && !nvfuse_closefile(h, fd));
        nvfuse_invalidate_inode_suffix(sb, st.st_ino, 0);
    }
    return get(h, "/primary-epoch-block-reuse-data", 0x5a);
}
static int run(struct nvfuse_handle *h, void *argument)
{
    const char *mode = argument;
    if (!strcmp(mode, "partial-long-create") || !strcmp(mode, "partial-long-check")) return partial_writes_long(h, !strcmp(mode, "partial-long-create"));
    CHECK(h->nvh_target->journal);
    if (!strcmp(mode, "partial-create") || !strcmp(mode, "partial-check") ||
        !strcmp(mode, "partial-create-crash")) {
        int rc = partial_writes(h, strcmp(mode, "partial-check") != 0);
        if (!rc && !strcmp(mode, "partial-create-crash")) {
            fprintf(stderr, "EXPECTED_CRASH phase=partial-after-fsync\n");
            fflush(NULL); _exit(86);
        }
        return rc;
    } else if (!strcmp(mode, "block-reuse-create") || !strcmp(mode, "block-reuse-check")) {
        return block_reuse(h, !strcmp(mode, "block-reuse-create"));
    } else if (!strncmp(mode, "reuse-", 6)) {
        CHECK(!strcmp(mode, "reuse-seed") || !strcmp(mode, "reuse-create") || !strcmp(mode, "reuse-check"));
        return reuse_inode(h, mode);
    } else if (!strcmp(mode, "format")) {
        CHECK(!put(h, "/primary-stable", 0x51)); CHECK(!nvfuse_sync(h));
    } else if (!strcmp(mode, "stamp-old")) {
        struct stat st; CHECK(!nvfuse_getattr(h, "/primary-stable", &st));
        struct nvfuse_inode_ctx *ctx = nvfuse_read_inode(&h->nvh_sb, NULL, st.st_ino);
        CHECK(ctx);
        ctx->ictx_inode->i_mtime = ctx->ictx_inode->i_ctime = 1;
        nvfuse_release_inode(&h->nvh_sb, ctx, DIRTY);
        CHECK(!nvfuse_sync(h));
    } else if (!strcmp(mode, "check-aio-times")) {
        struct stat st; CHECK(!nvfuse_getattr(h, "/primary-stable", &st));
        CHECK(st.st_size == 65536 && st.st_mtime > 1 && st.st_ctime > 1);
    } else if (!strcmp(mode, "mutate") || !strcmp(mode, "mutate-checkpoint")) {
        CHECK(!put(h, "/primary-new", 0x62));
        CHECK(!nvfuse_rename_path(h, "/primary-stable", "/primary-moved"));
        nvj_set_hook(h->nvh_target->journal, crash, NULL);
        CHECK(!nvfuse_sync(h));
        if (!strcmp(mode, "mutate-checkpoint")) {
            nvj_discard(h->nvh_target->journal); /* Empty TX opened by sync. */
            CHECK(!nvj_checkpoint(h->nvh_target->journal));
            CHECK(!nvj_begin(h->nvh_target->journal, 0, 0));
        }
        CHECK(!phase || !*phase); /* An unknown phase is a failed test. */
    } else if (!strcmp(mode, "io-fault")) {
        CHECK(!put(h, "/primary-new", 0x62));
        CHECK(!nvfuse_rename_path(h, "/primary-stable", "/primary-moved"));
        nvj_set_io_fault_hook(h->nvh_target->journal, fail_payload, h->nvh_target->journal);
        CHECK(nvfuse_sync(h) < 0);
        CHECK(nvj_recovery_required(h->nvh_target->journal));
        fprintf(stderr, "EXPECTED_IO_FAILURE recovery_required=1\n"); fflush(NULL); _exit(87);
    } else {
        int committed = !strcmp(mode, "check-new");
        if (!strcmp(mode, "check-either")) {
            struct stat probe;
            committed = !nvfuse_getattr(h, "/primary-moved", &probe);
            printf("RECOVERED_NAMESPACE state=%s\n", committed ? "new" : "old");
        }
        struct stat st;
        CHECK(!get(h, committed ? "/primary-moved" : "/primary-stable", 0x51));
        CHECK(nvfuse_getattr(h, committed ? "/primary-stable" : "/primary-moved", &st) < 0);
        if (committed) CHECK(!get(h, "/primary-new", 0x62));
        else CHECK(nvfuse_getattr(h, "/primary-new", &st) < 0);
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
