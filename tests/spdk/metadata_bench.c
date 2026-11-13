/* A common workload for direct API/SPDK and POSIX references. This is a
 * purpose-built mdtest-style workload, not the mdtest or fs_mark executable. */
#ifdef NVMETA_POSIX
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#else
#include "spdk/stdinc.h"
#include <dirent.h>
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_journal.h"
#include "runtime.h"
#endif
#include <sys/resource.h>

struct phase_cpu { struct rusage owner, process; };
static struct phase_cpu cpu_sample(void)
{
    struct phase_cpu r;
#ifdef __linux__
    if (getrusage(RUSAGE_THREAD, &r.owner)) abort();
#else
    if (getrusage(RUSAGE_SELF, &r.owner)) abort();
#endif
    if (getrusage(RUSAGE_SELF, &r.process)) abort();
    return r;
}
static uint64_t cpu_us(struct rusage r)
{
    return (uint64_t)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000 +
           r.ru_utime.tv_usec + r.ru_stime.tv_usec;
}
static void cpu_result(const char *name, struct phase_cpu before, struct phase_cpu after)
{
    printf("NVMETA_CPU {\"phase\":\"%s\",\"owner_us\":%" PRIu64
           ",\"process_us\":%" PRIu64 ",\"owner_voluntary\":%ld,\"owner_involuntary\":%ld}\n",
           name, cpu_us(after.owner) - cpu_us(before.owner),
           cpu_us(after.process) - cpu_us(before.process),
           after.owner.ru_nvcsw - before.owner.ru_nvcsw,
           after.owner.ru_nivcsw - before.owner.ru_nivcsw);
}
struct workload {
    void *handle;
    const char *base;
    unsigned count, batch, rounds;
    int directory;
    const char *sync_scope;
    int latency;
};
static uint64_t ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int compare_latency(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void latency_result(const char *name, uint64_t *samples, unsigned count)
{
    qsort(samples, count, sizeof(*samples), compare_latency);
    printf("NVMETA_LATENCY {\"phase\":\"%s\",\"samples\":%u,\"unit\":\"ns\","
           "\"p50\":%" PRIu64 ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64
           ",\"p999\":%" PRIu64 ",\"max\":%" PRIu64 "}\n", name, count,
           samples[(count * 50u + 99u) / 100u - 1], samples[(count * 95u + 99u) / 100u - 1],
           samples[(count * 99u + 99u) / 100u - 1], samples[(count * 999u + 999u) / 1000u - 1], samples[count - 1]);
}
static int checkpoint(struct workload *w)
{
#ifdef NVMETA_POSIX
#ifdef __linux__
    if (strcmp(w->sync_scope, "directory")) return syncfs(w->directory);
#endif
    return fsync(w->directory);
#else
    return nvfuse_sync(w->handle);
#endif
}
static int directory(struct workload *w, const char *path, int remove)
{
#ifdef NVMETA_POSIX
    (void)w;
    return remove ? rmdir(path) : mkdir(path, 0755);
#else
    return remove ? nvfuse_rmdir_path(w->handle, path) : nvfuse_mkdir_path(w->handle, path, 0755);
#endif
}
enum operation { CREATE, STAT, OPEN_CLOSE, RENAME, UNLINK, MKDIR, RMDIR };
static int operation(struct workload *w, enum operation op, const char *a, const char *b)
{
    int fd;
    struct stat st;
#ifdef NVMETA_POSIX
    (void)w;
    switch (op) {
    case CREATE: case OPEN_CLOSE:
        fd = open(a, O_RDWR | (op == CREATE ? O_CREAT | O_EXCL : 0), 0644);
        return fd < 0 ? -1 : close(fd);
    case STAT: return stat(a, &st) || st.st_size != 0 ? -1 : 0;
    case RENAME: return rename(a, b);
    case UNLINK: return unlink(a);
    default: return directory(w, a, op == RMDIR);
    }
#else
    switch (op) {
    case CREATE: case OPEN_CLOSE:
        fd = nvfuse_openfile_path(w->handle, a, O_RDWR | (op == CREATE ? O_CREAT | O_EXCL : 0), 0644);
        return fd < 0 ? -1 : nvfuse_closefile(w->handle, fd);
    case STAT:
        if (nvfuse_getattr(w->handle, a, &st)) return -1;
        return st.st_size != 0 ? -1 : 0;
    case RENAME: return nvfuse_rename_path(w->handle, a, b);
    case UNLINK: return nvfuse_unlink(w->handle, a);
    default: return directory(w, a, op == RMDIR);
    }
#endif
}
static struct nvmeta_counters { uint64_t commits, bytes, flushes, collect_ns, dirty_ns, super_ns;
    uint64_t compare_reads, compare_equal, compare_ns, stage_read_ns;
    uint64_t descriptor_alloc_ns, payload_crc_ns, descriptor_encode_ns, certificate_crc_ns;
    uint64_t profile_ns[9]; } counters(struct workload *w)
{
    struct nvmeta_counters r = {0};
#ifndef NVMETA_POSIX
    struct nvfuse_handle *h = w->handle;
    const struct nvj_stats *s = h->nvh_target->journal ? nvj_stats(h->nvh_target->journal) : NULL;
    if (s) {
        r.commits = s->commits; r.bytes = s->journal_bytes; r.flushes = s->flushes;
        r.collect_ns = h->nvh_target->journal_collect_ns;
        r.dirty_ns = h->nvh_target->journal_dirty_ns;
        r.super_ns = h->nvh_target->journal_super_ns;
        r.compare_reads = s->compare_reads; r.compare_equal = s->compare_equal;
        r.compare_ns = s->compare_ns; r.stage_read_ns = s->stage_read_ns;
        r.descriptor_alloc_ns = s->descriptor_alloc_ns; r.payload_crc_ns = s->payload_crc_ns;
        r.descriptor_encode_ns = s->descriptor_encode_ns; r.certificate_crc_ns = s->certificate_crc_ns;
        _Static_assert(NVJ_P_COUNT == 9, "metadata profile schema");
        memcpy(r.profile_ns, s->profile_ns, sizeof(r.profile_ns));
    }
#else
    (void)w;
#endif
    return r;
}
static void result(struct workload *w, const char *name, unsigned count, uint64_t elapsed,
                   uint64_t sync_time, struct nvmeta_counters before)
{
    struct nvmeta_counters after = counters(w);
    printf("NVMETA {\"phase\":\"%s\",\"operations\":%u,\"nanoseconds\":%" PRIu64
           ",\"ops_per_second\":%.3f,\"checkpoint_ns\":%" PRIu64
           ",\"outside_checkpoint_ns\":%" PRIu64 ",\"sync_scope\":\"%s\""
           ",\"batch\":%u,\"commits\":%" PRIu64 ",\"journal_bytes\":%" PRIu64
           ",\"flushes\":%" PRIu64 ",\"collect_ns\":%" PRIu64 ",\"profile_ns\":[", name, count, elapsed,
           elapsed ? (double)count * 1e9 / elapsed : 0, sync_time, elapsed - sync_time,
           w->sync_scope, w->batch,
           after.commits - before.commits, after.bytes - before.bytes, after.flushes - before.flushes,
           after.collect_ns - before.collect_ns);
    for (unsigned i = 0; i < 9; i++)
        printf("%s%" PRIu64, i ? "," : "", after.profile_ns[i] - before.profile_ns[i]);
    printf("],\"dirty_ns\":%" PRIu64 ",\"super_ns\":%" PRIu64
           ",\"compare_reads\":%" PRIu64 ",\"compare_equal\":%" PRIu64
           ",\"compare_ns\":%" PRIu64 ",\"stage_read_ns\":%" PRIu64
           ",\"descriptor_alloc_ns\":%" PRIu64 ",\"payload_crc_ns\":%" PRIu64
           ",\"descriptor_encode_ns\":%" PRIu64 ",\"certificate_crc_ns\":%" PRIu64 "}\n",
           after.dirty_ns - before.dirty_ns, after.super_ns - before.super_ns,
           after.compare_reads - before.compare_reads, after.compare_equal - before.compare_equal,
           after.compare_ns - before.compare_ns, after.stage_read_ns - before.stage_read_ns,
           after.descriptor_alloc_ns - before.descriptor_alloc_ns, after.payload_crc_ns - before.payload_crc_ns,
           after.descriptor_encode_ns - before.descriptor_encode_ns, after.certificate_crc_ns - before.certificate_crc_ns);
    fflush(stdout);
}
static int phase(struct workload *w, const char *name, enum operation op, unsigned count)
{
    char a[1024], b[1024];
    int mutate = op != STAT && op != OPEN_CLOSE;
    unsigned rounds = mutate ? 1 : w->rounds;
    uint64_t *latencies = w->latency && mutate ? calloc(count, sizeof(*latencies)) : NULL;
    if (w->latency && mutate && !latencies) return -1;
    struct phase_cpu cpu_before = cpu_sample();
    uint64_t begin = ns(), sync_time = 0;
    struct nvmeta_counters before = counters(w);
    for (unsigned r = 0; r < rounds; r++) for (unsigned i = 0; i < count; i++) {
        uint64_t operation_start = latencies ? ns() : 0;
        snprintf(a, sizeof(a), "%s/%c%08u", w->base, op == UNLINK ? 'r' : (op >= MKDIR ? 'd' : 'f'), i);
        snprintf(b, sizeof(b), "%s/r%08u", w->base, i);
        if (operation(w, op, a, b)) {
            fprintf(stderr, "metadata phase=%s item=%u failed errno=%d\n", name, i, errno); free(latencies); return -1;
        }
        if (mutate && strcmp(w->sync_scope, "none") &&
            ((i + 1) % w->batch == 0 || i + 1 == count)) {
            uint64_t start = ns();
            if (checkpoint(w)) { free(latencies); return -1; }
            sync_time += ns() - start;
        }
        if (latencies) latencies[i] = ns() - operation_start;
    }
    uint64_t elapsed = ns() - begin;
    struct phase_cpu cpu_after = cpu_sample();
    result(w, name, count * rounds, elapsed, sync_time, before);
    cpu_result(name, cpu_before, cpu_after);
    if (latencies) { latency_result(name, latencies, count); free(latencies); }
    /* Deferred mode measures visibility/operation cost, not durability. Fence
     * outside the timed phase so the next phase starts with bounded credits. */
    if (mutate && !strcmp(w->sync_scope, "none")) {
        uint64_t start = ns();
        if (checkpoint(w)) return -1;
        printf("NVMETA_DRAIN {\"phase\":\"%s\",\"nanoseconds\":%" PRIu64 "}\n",
               name, ns() - start);
        fflush(stdout);
    }
    return 0;
}
static int enumerate(struct workload *w)
{
    struct phase_cpu cpu_before = cpu_sample();
    uint64_t begin = ns(); unsigned count = 0;
    struct nvmeta_counters before = counters(w);
    for (unsigned r = 0; r < w->rounds; r++) {
#ifdef NVMETA_POSIX
        DIR *dir = opendir(w->base); if (!dir) return -1;
        struct dirent *entry;
        while ((entry = readdir(dir))) if (entry->d_name[0] != '.') count++;
        if (closedir(dir)) return -1;
#else
        struct nvfuse_handle *h = w->handle;
        int ino = nvfuse_opendir(h, w->base); if (ino < 0) return -1;
        struct stat st; if (nvfuse_getattr(h, w->base, &st)) return -1;
        struct dirent entry;
        /* The API uses physical directory-slot offsets; holes are not EOF. */
        for (off_t off = 0; off < st.st_size / DIR_ENTRY_SIZE || off < 2; off++)
            if (nvfuse_readdir(h, ino, &entry, off) && entry.d_name[0] != '.') count++;
#endif
    }
    if (count != w->count * w->rounds) return -1;
    uint64_t elapsed = ns() - begin;
    struct phase_cpu cpu_after = cpu_sample();
    result(w, "readdir-entry", count, elapsed, 0, before);
    cpu_result("readdir-entry", cpu_before, cpu_after); return 0;
}
static int workload(struct workload *w)
{
    if (directory(w, w->base, 0)) return -1;
#ifdef NVMETA_POSIX
    w->directory = open(w->base, O_RDONLY | O_DIRECTORY);
    if (w->directory < 0) return -1;
#endif
    if (checkpoint(w)) return -1;
    if (phase(w, "create-close", CREATE, w->count) || phase(w, "stat", STAT, w->count) ||
        phase(w, "open-close", OPEN_CLOSE, w->count) || enumerate(w) ||
        phase(w, "rename", RENAME, w->count) || phase(w, "unlink", UNLINK, w->count) ||
        phase(w, "mkdir", MKDIR, w->count / 8) || phase(w, "rmdir", RMDIR, w->count / 8)) return -1;
#ifdef NVMETA_POSIX
    close(w->directory);
#endif
    /* Keep an empty fixture directory for audit; all measured entries removed. */
    return 0;
}
#ifndef NVMETA_POSIX
static int on_owner(struct nvfuse_handle *h, void *argument)
{
    struct workload *w = argument; w->handle = h;
    if (!h->nvh_target->journal) return -EINVAL; /* Intended default-mode test. */
    return workload(w);
}
#endif
int main(int argc, char **argv)
{
    struct workload w = {.count = 4096, .batch = 128, .rounds = 16};
#ifdef NVMETA_POSIX
    w.sync_scope = "directory";
#else
    w.sync_scope = "filesystem";
#endif
    const char *scope = getenv("NVMETA_SYNC");
    const char *latency = getenv("NVMETA_LATENCY");
    if (latency && strcmp(latency, "0") && strcmp(latency, "1")) return 2;
    w.latency = latency && !strcmp(latency, "1");
    if (scope) w.sync_scope = scope;
    if (strcmp(w.sync_scope, "none") && strcmp(w.sync_scope, "filesystem") &&
        strcmp(w.sync_scope, "directory")) return 2;
#ifndef NVMETA_POSIX
    if (!strcmp(w.sync_scope, "directory")) return 2; /* No scoped primary fsync. */
#elif !defined(__linux__)
    if (strcmp(w.sync_scope, "directory")) return 2; /* syncfs is Linux-only. */
#endif
#ifdef NVMETA_POSIX
    if (argc < 2 || argc > 4) return 2;
    w.base = argv[1];
    if (argc > 2) w.count = strtoul(argv[2], NULL, 10);
    if (argc > 3) w.batch = strtoul(argv[3], NULL, 10);
#else
    if (argc < 4 || argc > 6) return 2;
    w.base = argv[3];
    if (argc > 4) w.count = strtoul(argv[4], NULL, 10);
    if (argc > 5) w.batch = strtoul(argv[5], NULL, 10);
#endif
    if (w.count < 8 || w.count > 100000 || !w.batch || w.batch > 128 || strlen(w.base) > 900) return 2;
#ifndef NVMETA_POSIX
    /* No per-phase credit admission in this diagnostic tool. Keep deferred
     * create phases well inside the primary single-slot journal capacity. */
    if (!strcmp(w.sync_scope, "none") && w.count > 512) return 2;
#endif
#ifdef NVMETA_POSIX
    int rc = workload(&w);
#else
    int rc = nvfio_start(argv[1], argv[2], 0); /* Never format in this tool. */
    if (!rc) {
        rc = nvfio_execute(on_owner, &w);
        if (rc) { fflush(NULL); _exit(1); }
    }
    if (!rc) rc = nvfio_stop();
#endif
    if (!rc) puts("PASS metadata workload; measured entries cleaned");
    return rc ? 1 : 0;
}
