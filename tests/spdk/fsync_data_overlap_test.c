/* Dependency subsets and immutable-commit overlap on three shared-BG files. */
#define main inherited_cc_main
#include "commit_cache_v2_test.c"
#undef main

static int overlap_fd[3];
static const unsigned seed_bytes = 8192, a_bytes = 8192, c_bytes = 4101;
static int scope_fault, target_alias;
extern void nvfio_test_data_only(int);
static atomic_int data_control_done;
static int data_noop(struct nvfuse_handle*h,void*unused)
{(void)h;(void)unused;return 0;}
static void *data_control_worker(void*unused)
{(void)unused;int rc=nvfio_execute(data_noop,NULL);atomic_store(&data_control_done,1);return(void*)(intptr_t)rc;}
static void *data_read_worker(void*unused)
{
 (void)unused;char b[32];int rc=nvfio_pread(overlap_fd[2],b,sizeof(b),0);
 if(rc!=(int)sizeof(b))return(void*)(intptr_t)-EIO;
 for(unsigned i=0;i<sizeof(b);i++)if((unsigned char)b[i]!=0x33)return(void*)(intptr_t)-EIO;
 return NULL;
}
static const char *scope_path[] = {"/scope-overlap-a", "/scope-overlap-b", "/scope-overlap-c"};

static int scope_fill(struct nvfuse_handle *h, int f, unsigned at, unsigned n, int value)
{
    char *data = malloc(n); CHECK(data); memset(data, value, n);
    int result = nvfuse_writefile(h, f, data, n, at); free(data);
    CHECK(result == (int)n); return 0;
}

static int scope_seed(struct nvfuse_handle *h, void *unused)
{
    (void)unused;
    for (unsigned k = 0; k < 3; k++) {
        int f = nvfuse_openfile_path(h, scope_path[k], O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(f >= 0 && !scope_fill(h, f, 0, seed_bytes, 0x31 + k));
        CHECK(!nvfuse_closefile(h, f));
    }
    CHECK(!nvfuse_sync(h));
    printf("SCOPE_OVERLAP_SEED free=%lld\n", (long long)h->nvh_sb.sb_free_blocks);
    return 0;
}

static int scope_open(struct nvfuse_handle *h, void *mutate)
{
    for (unsigned k = 0; k < 3; k++) {
        overlap_fd[k] = nvfuse_openfile_path(h, scope_path[k], O_RDWR, 0);
        CHECK(overlap_fd[k] >= 0);
    }
    cc_fd = overlap_fd[0];
    target_alias=nvfuse_openfile_path(h,scope_path[0],O_RDWR,0);CHECK(target_alias>=0);
    if (mutate) {
        CHECK(!scope_fill(h, overlap_fd[1], seed_bytes, a_bytes, 0x62));
        CHECK(!scope_fill(h, overlap_fd[0], seed_bytes, a_bytes, 0x61));
        CHECK(!h->nvh_target->journal_file_sync_unsafe);
        CHECK(!nvj_pending_blocks(h->nvh_target->journal));
        if (scope_fault)
            nvj_set_io_fault_hook(h->nvh_target->journal, wb_fault_payload, h->nvh_target->journal);
    }
    return 0;
}

struct scope_expected { int a, c; };
static int scope_check(struct nvfuse_handle *h, void *arg)
{
    struct scope_expected *expected = arg;
    for (unsigned k = 0; k < 3; k++) {
        unsigned extra = k == 0 && expected->a ? a_bytes : k == 2 && expected->c ? c_bytes : 0;
        unsigned size = seed_bytes + extra;
        char *data = malloc(size + 1); CHECK(data);
        int f = nvfuse_openfile_path(h, scope_path[k], O_RDONLY, 0); CHECK(f >= 0);
        struct stat st; CHECK(!nvfuse_getattr(h, scope_path[k], &st));
        CHECK(st.st_size == size);
        CHECK(nvfuse_readfile(h, f, data, size + 1, 0) == (int)size);
        for (unsigned i = 0; i < size; i++)
            CHECK((unsigned char)data[i] == (i < seed_bytes ? 0x31 + k : 0x61 + k));
        free(data); CHECK(!nvfuse_closefile(h, f));
    }
    printf("SCOPE_OVERLAP_RECOVERED a=%d b=0 c=%d free=%lld\n",
           expected->a, expected->c, (long long)h->nvh_sb.sb_free_blocks);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 4); CHECK(!nvfio_start(argv[1], argv[2], 0));
    CHECK(!nvfio_test_pause_periodic_journal());
    const char *mode = argv[3];
    if (!strcmp(mode, "seed")) {
        CHECK(!nvfio_execute(scope_seed, NULL)); CHECK(!nvfio_stop()); return 0;
    }
    if (!strncmp(mode, "check-", 6)) {
        struct scope_expected expected = {strcmp(mode, "check-failed") != 0, !strcmp(mode, "check-next")};
        CHECK(!nvfio_execute(scope_check, &expected)); CHECK(!nvfio_stop());
        puts("PASS scope overlap original-image repeated replay"); return 0;
    }
    scope_fault = !strcmp(mode, "failed");
    nvfio_test_data_only(1);
    CHECK(!nvfio_execute(scope_open, (void *)1));
    uint64_t before[8], after[8]; CHECK(!nvfio_file_sync_stats(before));
    nvfio_test_commit_pause(1, 0); pthread_t sync;
    CHECK(!pthread_create(&sync, NULL, cc_sync_worker, NULL)); CHECK(!cc_wait(0));
    char data[4101]; memset(data, 0x63, sizeof(data));
    /* Every alias of the target is excluded even though its aligned EOF and
     * loaded allocation metadata would otherwise permit this cached append. */
    CHECK(!nvfio_test_cc_tryappend(overlap_fd[0],data,4096,seed_bytes+a_bytes));
    CHECK(!nvfio_test_cc_tryappend(target_alias,data,4096,seed_bytes+a_bytes));
    CHECK(nvfio_test_cc_tryappend(overlap_fd[2], data, sizeof(data), seed_bytes) == sizeof(data));
    if(!strcmp(mode,"quiesce")){
        nvfio_test_commit_pause(1,1);pthread_t client,control;
        CHECK(!pthread_create(&client,NULL,data_read_worker,NULL));CHECK(!cc_wait(1));
        nvfio_test_commit_release(1,0);unsigned spins=0;
        while((nvfio_test_commit_state()&1u)&&spins++<20000)usleep(100);
        CHECK(nvfio_test_commit_state()==2);
        CHECK(!pthread_create(&control,NULL,data_control_worker,NULL));usleep(20000);
        CHECK(!atomic_load(&cc_sync_done)&&!atomic_load(&data_control_done));
        nvfio_test_commit_release(0,1);void*rc;
        CHECK(!pthread_join(client,&rc)&&!rc);CHECK(!pthread_join(control,&rc)&&!rc);
    }
    nvfio_test_commit_release(1, 0); void *result;
    CHECK(!pthread_join(sync, &result));
    if (scope_fault) {
        CHECK((intptr_t)result < 0);
        puts("EXPECTED_IO_FAILURE scope-overlap-log-with-deferred-and-concurrent-allocations");
        fflush(NULL); _exit(86);
    }
    CHECK(!result); CHECK(!nvfio_file_sync_stats(after));
    CHECK(after[1] == before[1] + 1);
    if (!strcmp(mode, "next")) {
        CHECK(!nvfio_fsync(overlap_fd[2])); CHECK(!nvfio_file_sync_stats(after));
        CHECK(after[1] == before[1] + 2);
    } else CHECK(!strcmp(mode, "snapshot") || !strcmp(mode,"quiesce"));
    printf("EXPECTED_CRASH scope-overlap mode=%s hits=%llu allocations=%llu\n", mode,
           (unsigned long long)(after[1] - before[1]),
           (unsigned long long)(after[3] - before[3]));
    fflush(NULL); _exit(86);
}
