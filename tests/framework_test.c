#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "test_backend.h"
#include "tree_verify.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
#define DATA_SIZE (2 * 1024 * 1024 + 137)
#define META_SIZE (32 * 1024 * 1024 + 137)
#define FILES 160
static uint32_t rng;
static uint32_t next_random(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int directory_payload(struct nvfuse_handle *h,uint32_t seed,int create) {
    if(create) {
        CHECK(!nvfuse_mkdir_path(h,"/verify",0755));
        CHECK(!nvfuse_mkdir_path(h,"/verify/nested",0755));
        CHECK(!nvfuse_mkdir_path(h,"/verify/empty",0755));
    }
    const char *paths[]={"/verify/alpha","/verify/nested/beta"};
    for(unsigned k=0;k<2;k++) {
        unsigned char expected[4097],actual[4097];
        for(unsigned i=0;i<sizeof(expected);i++)expected[i]=(unsigned char)(seed+i*13+k*37);
        int fd=nvfuse_openfile_path(h,paths[k],create?O_CREAT|O_RDWR:O_RDONLY,0644);CHECK(fd>=0);
        if(create)CHECK(nvfuse_writefile(h,fd,(char *)expected,sizeof(expected),0)==sizeof(expected));
        CHECK(nvfuse_readfile(h,fd,(char *)actual,sizeof(actual),0)==sizeof(actual));
        CHECK(!memcmp(expected,actual,sizeof(actual)));CHECK(!nvfuse_closefile(h,fd));
    }
    return 0;
}
static int workload(struct nvfuse_handle *h, uint32_t seed, int create)
{
    if(!create)CHECK(!test_cold_parents(h,ROOT_INO,ROOT_INO,0));
    unsigned char *model = calloc(1, DATA_SIZE), *actual = malloc(DATA_SIZE);
    CHECK(model && actual);
    rng = seed;
    int fd = nvfuse_openfile_path(h, "/payload", create ? O_CREAT | O_RDWR : O_RDONLY, 0644);
    CHECK(fd >= 0);
    if (create) CHECK(nvfuse_writefile(h, fd, (char *)model, DATA_SIZE, 0) == DATA_SIZE);
    const unsigned sizes[] = {1, 511, 512, 513, 4095, 4096, 4097, 8191, 65536};
    for (unsigned i = 0; i < 500; i++) {
        unsigned size = sizes[i % (sizeof(sizes) / sizeof(sizes[0]))];
        unsigned offset = next_random() % (DATA_SIZE - size + 1);
        for (unsigned j = 0; j < size; j++) {
            unsigned char byte = (i % 4 == 0) ? 0 : (i % 4 == 1) ? 255 :
                                 (i % 4 == 2) ? (j & 1 ? 0xaa : 0x55) : next_random();
            model[offset + j] = byte;
        }
        if (create) {
            CHECK(nvfuse_writefile(h, fd, (char *)model + offset, size, offset) == (int)size);
            CHECK(nvfuse_readfile(h, fd, (char *)actual, size, offset) == (int)size);
            CHECK(!memcmp(actual, model + offset, size));
        }
    }
    CHECK(nvfuse_readfile(h, fd, (char *)actual, DATA_SIZE, 0) == DATA_SIZE);
    CHECK(!memcmp(actual, model, DATA_SIZE));
    memset(actual, 0xa7, 64);
    CHECK(nvfuse_readfile(h, fd, (char *)actual, 64, DATA_SIZE - 1) == 1);
    CHECK(actual[0] == model[DATA_SIZE - 1] && actual[1] == 0xa7);
    CHECK(nvfuse_readfile(h, fd, (char *)actual, 1, DATA_SIZE) == 0);
    CHECK(nvfuse_readfile(h, fd, (char *)actual, -1, 0) == -EINVAL);
    CHECK(nvfuse_readfile(h, fd, (char *)actual, 1, -1) == -EINVAL);
    if (create) {
        CHECK(nvfuse_writefile(h, fd, (char *)actual, 1, -1) == -EINVAL);
        CHECK(nvfuse_fsync(h, fd) == 0);
    }
    CHECK(nvfuse_closefile(h, fd) == 0);
    free(model); free(actual);
    CHECK(!directory_payload(h,seed,create));
    CHECK(test_verify_tree(h)==0);
    return 0;
}

static int metadata(struct nvfuse_handle *h, int stage)
{
    char data[65536];
    struct stat st;
    if (stage == 0) {
        CHECK(nvfuse_mkdir_path(h, "/tree", 0755) == 0);
        memset(data, 0x5a, sizeof(data));
        int fd = nvfuse_openfile_path(h, "/tree/large", O_CREAT | O_RDWR, 0644);
        CHECK(fd >= 0);
        for (unsigned off = 0; off < META_SIZE;) {
            unsigned n = META_SIZE - off;
            if (n > sizeof(data)) n = sizeof(data);
            CHECK(nvfuse_writefile(h, fd, data, n, off) == (int)n);
            off += n;
        }
        CHECK(h->nvh_target->payload_bytes_discarded == (h->nvh_target->metadata_only ? META_SIZE : 0));
        if (h->nvh_target->metadata_only)
            CHECK(nvfuse_readfile_aio(h, fd, data, 1, 0) == -EOPNOTSUPP);
        CHECK(nvfuse_fsync(h, fd) == 0);
        CHECK(nvfuse_closefile(h, fd) == 0);
        for (int i = 0; i < FILES; i++) {
            char path[64]; snprintf(path, sizeof(path), "/tree/item-%03d", i);
            fd = nvfuse_openfile_path(h, path, O_CREAT | O_RDWR, 0644);
            CHECK(fd >= 0);
            CHECK(nvfuse_writefile(h, fd, data, i + 1, 0) == i + 1);
            CHECK(nvfuse_closefile(h, fd) == 0);
        }
        CHECK(nvfuse_rename_path(h, "/tree/large", "/tree/renamed") == 0);
    }
    if (stage <= 1) {
        CHECK(nvfuse_getattr(h, "/tree/large", &st) < 0);
        CHECK(nvfuse_getattr(h, "/tree/renamed", &st) == 0 && st.st_size == META_SIZE);
        int fd = nvfuse_openfile_path(h, "/tree/renamed", O_RDONLY, 0);
        CHECK(fd >= 0);
        const unsigned offsets[] = {0, 4093, 11 * 4096, (11 + 1024) * 4096, META_SIZE - 19};
        for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            memset(data, 0xa7, sizeof(data));
            int expected = META_SIZE - offsets[i];
            if (expected > (int)sizeof(data)) expected = sizeof(data);
            CHECK(nvfuse_readfile(h, fd, data, sizeof(data), offsets[i]) == expected);
            for (int j = 0; j < expected; j++) CHECK(data[j] == (h->nvh_target->metadata_only ? 0 : 0x5a));
            if (expected < (int)sizeof(data)) CHECK((unsigned char)data[expected] == 0xa7);
        }
        CHECK(nvfuse_closefile(h, fd) == 0);
        for (int i = 0; i < FILES; i++) {
            char path[64]; snprintf(path, sizeof(path), "/tree/item-%03d", i);
            CHECK(nvfuse_getattr(h, path, &st) == 0 && st.st_size == i + 1);
        }
    } else if (stage == 2) {
        struct statvfs before, after;
        CHECK(nvfuse_statvfs(h, "/", &before) == 0);
        CHECK(nvfuse_rmfile_path(h, "/tree/renamed") == 0);
        for (int i = 0; i < FILES; i++) {
            char path[64]; snprintf(path, sizeof(path), "/tree/item-%03d", i);
            CHECK(nvfuse_rmfile_path(h, path) == 0);
        }
        CHECK(nvfuse_rmdir_path(h, "/tree") == 0);
        CHECK(nvfuse_statvfs(h, "/", &after) == 0);
        CHECK(after.f_bfree > before.f_bfree && after.f_ffree > before.f_ffree);
    } else {
        CHECK(nvfuse_getattr(h, "/tree", &st) < 0);
        CHECK(nvfuse_getattr(h, "/tree/renamed", &st) < 0);
    }
    return 0;
}

static int queues(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 2 * 1024 * 1024) == 0);
    struct io_target target = {.fd = fd, .size_bytes = 2 * 1024 * 1024};
    const int depths[] = {1, 2, 8, 31, 127};
    for (unsigned d = 0; d < sizeof(depths) / sizeof(depths[0]); d++) {
        int depth = depths[d];
        struct reactor_task *task = reactor_alloc_task(&target, depth);
        CHECK(task);
        struct io_job *jobs = calloc(depth, sizeof(*jobs)), **ptrs = calloc(depth, sizeof(*ptrs));
        char *buffers = calloc(depth, 4096);
        CHECK(jobs && ptrs && buffers);
        for (int round = 0; round < 10; round++) {
            for (int i = 0; i < depth; i++) {
                memset(buffers + i * 4096, (round + i) & 255, 4096);
                jobs[i] = (struct io_job){.offset = i * 4096, .bytes = 4096,
                    .buf = buffers + i * 4096, .req_type = SPDK_BDEV_IO_TYPE_WRITE};
                ptrs[i] = &jobs[i];
            }
            CHECK(reactor_submit_reqs(&target, task, ptrs, depth) == 0);
            CHECK(reactor_cq_size(task) == depth);
            CHECK(reactor_submit_reqs(&target, task, ptrs, 1) == -EINVAL);
            CHECK(reactor_cq_get_reqs(task, ptrs, depth, depth) == depth);
            for (int i = 0; i < depth; i++) CHECK(ptrs[i] == &jobs[i] && jobs[i].complete && !jobs[i].ret);
            for (int i = 0; i < depth; i++) {
                memset(buffers + i * 4096, 0, 4096);
                jobs[i].req_type = SPDK_BDEV_IO_TYPE_READ; jobs[i].complete = 0;
            }
            CHECK(reactor_submit_reqs(&target, task, ptrs, depth) == 0);
            CHECK(reactor_cq_get_reqs(task, ptrs, depth, depth) == depth);
            for (int i = 0; i < depth; i++) {
                CHECK(jobs[i].complete);
                for (int j = 0; j < 4096; j++) CHECK((unsigned char)buffers[i * 4096 + j] == ((round + i) & 255));
            }
        }
        jobs[0].offset = target.size_bytes; CHECK(reactor_submit_reqs(&target, task, ptrs, 1) == -EINVAL);
        jobs[0].offset = -1; CHECK(reactor_submit_reqs(&target, task, ptrs, 1) == -EINVAL);
        jobs[0].offset = 0; jobs[0].bytes = 0; CHECK(reactor_submit_reqs(&target, task, ptrs, 1) == -EINVAL);
        CHECK(reactor_cq_size(task) == 0);
        reactor_free_task(&target, task); free(jobs); free(ptrs); free(buffers);
    }
    CHECK(close(fd) == 0);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    const char *mode = argv[1], *path = argv[2];
    uint32_t seed = strtoul(argv[3], NULL, 0);
    CHECK(seed != 0);
    int result = 0;
    if (!strcmp(mode, "queues")) result = queues(path);
    else if (!strcmp(mode, "abort-range") || !strcmp(mode, "abort-short-read")) {
        int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        struct io_target target = {.fd = fd, .size_bytes = 4096};
        char data[4096];
        reactor_sync_read_blk(&target, !strcmp(mode, "abort-range") ? 1 : 0, 1, data);
        return 1;
    } else if (!strcmp(mode, "reject-full") || !strcmp(mode, "reject-meta")) {
        struct nvfuse_handle *h = nvfuse_file_open_mode(path, 0, 0, !strcmp(mode, "reject-meta"));
        CHECK(!h && errno == EINVAL);
    } else {
        int meta = !strncmp(mode, "meta-", 5);
        int stored = !strncmp(mode, "stored-", 7);
        int create = !strcmp(mode, "data-create") || !strcmp(mode, "meta-create") || !strcmp(mode, "stored-create");
        struct nvfuse_handle *h = nvfuse_file_open_mode(path, create, 256ULL * 1024 * 1024, meta);
        CHECK(h);
        if (meta || stored) {
            int stage = strstr(mode, "create") ? 0 : strstr(mode, "verify") ? 1 :
                        strstr(mode, "delete") ? 2 : 3;
            result = metadata(h, stage);
        } else result = workload(h, seed, create);
        CHECK(nvfuse_file_close(h) == 0);
    }
    CHECK(result == 0);
    printf("PASS %s seed=%" PRIu32 "\n", mode, seed);
    return 0;
}
