/* SPDX-License-Identifier: GPL-2.0-only */
/* Real primary adapter: target-only durability, deferred allocations, repeated
 * subset commits and conservative fallbacks. Run on disposable AIO images. */
#include "spdk/stdinc.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_primary_journal.h"
#include "nvfuse_spdk_runtime.h"
#define nvfio_execute nvfio_execute_serialized
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return -EIO; } } while (0)

static size_t base_a;
static const size_t base_b = 3 * CLUSTER_SIZE + 37;
static const size_t extra_a = 2 * CLUSTER_SIZE + 91;
static const size_t extra_b = 5 * CLUSTER_SIZE + 71;
static const char *crash_phase;

static void crash_hook(void *unused, const char *phase, uint64_t block)
{
    (void)unused; (void)block;
    if (crash_phase && !strcmp(phase, crash_phase)) {
        printf("EXPECTED_CRASH phase=%s\n", phase); fflush(NULL); _exit(86);
    }
}

static int write_bytes(struct nvfuse_handle *h, int fd, size_t offset, size_t bytes, int value)
{
    char *data = malloc(bytes);
    CHECK(data); memset(data, value, bytes);
    int rc = nvfuse_writefile(h, fd, data, bytes, offset);
    free(data); CHECK(rc == (int)bytes); return 0;
}

static int verify(struct nvfuse_handle *h, const char *path, size_t base, size_t extra,
                   int initial, int overwritten, int appended, int interrupted)
{
    struct stat st; CHECK(!nvfuse_getattr(h, path, &st));
    CHECK(st.st_size == (off_t)(base + extra));
    int fd = nvfuse_openfile_path(h, path, O_RDONLY, 0); CHECK(fd >= 0);
    char *data = malloc(base + extra); CHECK(data);
    CHECK(nvfuse_readfile(h, fd, data, base + extra, 0) == (int)(base + extra));
    for (size_t i = 0; i < base + extra; i++) {
        int expected = i >= base ? appended : i < 19 ? overwritten : initial;
        if (interrupted && i < 19) CHECK((unsigned char)data[i] == initial || (unsigned char)data[i] == overwritten);
        else CHECK((unsigned char)data[i] == expected);
    }
    free(data); CHECK(!nvfuse_closefile(h, fd)); return 0;
}

static unsigned dirty_inode(struct nvfuse_superblock *sb, inode_t ino)
{
    struct list_head *ptr, *next; unsigned count = 0;
    SPINLOCK_LOCK(&sb->sb_bm->bm_lock);
    list_for_each_safe(ptr, next, &sb->sb_bm->bm_list[BUFFER_TYPE_DIRTY]) {
        struct nvfuse_buffer_cache *bc = list_entry(ptr, struct nvfuse_buffer_cache, bc_list);
        if (bc->bc_ino == ino) count++;
    }
    SPINLOCK_UNLOCK(&sb->sb_bm->bm_lock); return count;
}

static int run(struct nvfuse_handle *h, void *argument)
{
    const char *mode = argument;
    struct io_target *t = h->nvh_target;
    if (!strcmp(mode, "format")) return 0;
    if (!strcmp(mode, "seed")) {
        int a = nvfuse_openfile_path(h, "/fscope-a", O_CREAT | O_EXCL | O_RDWR, 0600);
        int b = nvfuse_openfile_path(h, "/fscope-b", O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(a >= 0 && b >= 0);
        CHECK(!write_bytes(h, a, 0, base_a, 0x31));
        CHECK(!write_bytes(h, b, 0, base_b, 0x41));
        CHECK(!nvfuse_sync(h));
        printf("FSCOPE_SEED free=%ld used=%ld\n", (long)h->nvh_sb.sb_free_blocks, (long)h->nvh_sb.sb_no_of_used_blocks);
        CHECK(!nvfuse_closefile(h, a) && !nvfuse_closefile(h, b));
        return 0;
    }
    if (!strncmp(mode, "check-", 6)) {
        int ack = strcmp(mode, "check-interrupted") != 0;
        int both = !strcmp(mode, "check-both");
        CHECK(!verify(h, "/fscope-a", base_a, ack ? extra_a : 0, 0x31, 0x32, 0x33, !ack));
        CHECK(!verify(h, "/fscope-b", base_b, both ? extra_b : 0, 0x41, both ? 0x42 : 0x41, 0x43, 0));
        printf("FSCOPE_RECOVERED free=%ld used=%ld\n", (long)h->nvh_sb.sb_free_blocks, (long)h->nvh_sb.sb_no_of_used_blocks);
        return 0;
    }
    int a = nvfuse_openfile_path(h, "/fscope-a", O_RDWR, 0);
    int b = nvfuse_openfile_path(h, "/fscope-b", O_RDWR, 0);
    CHECK(a >= 0 && b >= 0);
    int fallback = !strncmp(mode, "fallback-", 9);
    if (!strcmp(mode, "fallback-create")) {
        int other = nvfuse_openfile_path(h, "/fscope-new", O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(other >= 0 && !nvfuse_closefile(h, other));
    } else if (!strcmp(mode, "fallback-truncate")) {
        CHECK(!nvfuse_ftruncate(h, b, base_b - 1));
        CHECK(!write_bytes(h, b, base_b - 1, 1, 0x41));
    } else if (!strcmp(mode, "fallback-staged")) {
        /* Existing adapter collection/eviction may stage any file's metadata.
         * Such an overlay must never leak into a supposedly selective commit. */
        CHECK(!nvfuse_sync_superblock(&h->nvh_sb));
        CHECK(nvj_pending_blocks(t->journal) > 0);
    }
    CHECK(!write_bytes(h, b, 0, 19, 0x42));
    CHECK(!write_bytes(h, b, base_b, extra_b, 0x43));
    CHECK(!write_bytes(h, a, 0, 19, 0x32));
    CHECK(!write_bytes(h, a, base_a, extra_a, 0x33));
    uint64_t before = t->journal_file_sync_hits;
    uint64_t allocations = t->journal_file_sync_allocations;
    if (crash_phase) nvj_set_hook(t->journal, crash_hook, NULL);
    CHECK(!nvfuse_fsync(h, a));
    if (crash_phase) CHECK(!"requested hook did not fire");
    int selective = t->journal_file_sync && nvj_version(t->journal) == 3 && !fallback;
    CHECK(t->journal_file_sync_hits == before + selective);
    unsigned pending = dirty_inode(&h->nvh_sb, nvfuse_get_file_table(&h->nvh_sb, b)->ino);
    CHECK(selective ? pending > 0 : pending == 0);
    printf("FSCOPE_ACK selected=%d pending_b=%u allocations=%llu\n", selective, pending,
           (unsigned long long)(t->journal_file_sync_allocations - allocations));
    CHECK(!verify(h, "/fscope-b", base_b, extra_b, 0x41, 0x42, 0x43, 0));
    if (!strcmp(mode, "both") || fallback || !selective) {
        CHECK(!nvfuse_fdatasync(h, b));
        if (selective) CHECK(t->journal_file_sync_hits == before + 2);
    }
    printf("EXPECTED_CRASH phase=fscope-after-ack\n"); fflush(NULL); _exit(86);
}

int main(int argc, char **argv)
{
    if (argc < 5 || argc > 6) return 2;
    base_a = (size_t)strtoul(argv[4], NULL, 10) * CLUSTER_SIZE + 113;
    if (argc == 6) crash_phase = argv[5];
    int rc = nvfio_start(argv[1], argv[2], !strcmp(argv[3], "format"));
    if (rc) return 3;
    rc = nvfio_execute(run, argv[3]);
    if (rc) { fflush(NULL); _exit(1); }
    rc = nvfio_stop();
    if (!rc) printf("PASS file fsync %s\n", argv[3]);
    return rc ? 1 : 0;
}
