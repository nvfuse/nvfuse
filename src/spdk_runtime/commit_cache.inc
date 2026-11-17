/* Single cache executor during immutable journal commit; fs_gate remains held. */
static int commit_cache_enabled, data_cache_enabled;
/* Protected by admission release/acquire and its single client. Target identity
 * is an inode, so every file-descriptor alias shares the same exclusion. */
static inode_t commit_cache_excluded_ino;
static uint64_t data_cache_windows,data_cache_reads,data_cache_writes,data_cache_bytes;
static _Thread_local int commit_cache_armed;
struct __attribute__((aligned(64))) nvcc_runtime {
    _Atomic unsigned state;
    uint64_t reads,writes,bytes,misses,windows;
};
static struct nvcc_runtime commit_cache;
_Static_assert(sizeof(commit_cache)==64,"whole cacheline");
static int cached_copy(struct nvfuse_superblock *,int,void *,int,uint64_t,int *);
#ifdef NVFIO_TEST
static atomic_int cc_hold_owner,cc_owner_entered,cc_hold_client,cc_client_entered;
static int cc_pause_only_data;
void nvfio_test_data_only(int enabled){cc_pause_only_data=enabled;}
void nvfio_test_commit_pause(int owner,int client)
{atomic_store(&cc_owner_entered,0);atomic_store(&cc_client_entered,0);atomic_store(&cc_hold_owner,owner);atomic_store(&cc_hold_client,client);}
void nvfio_test_commit_release(int owner,int client)
{if(owner)atomic_store(&cc_hold_owner,0);if(client)atomic_store(&cc_hold_client,0);}
unsigned nvfio_test_commit_state(void){return atomic_load(&commit_cache.state);}
int nvfio_test_commit_entered(int client){return atomic_load(client?&cc_client_entered:&cc_owner_entered);}
#endif
static int commit_cache_open(struct nvfuse_handle *h,uint32_t excluded)
{
    if(!commit_cache_enabled || !commit_cache_armed || rt.clients ||
       !h->nvh_target->journal || h->nvh_target->journal_aio || nvj_version(h->nvh_target->journal)!=3 ||
       !nvfuse_process_model_is_standalone() || atomic_load_explicit(&rt.data_failed,memory_order_acquire))return 0;
    if(excluded && (!data_cache_enabled || excluded<NUM_RESV_INO))return 0;
    assert(!atomic_load_explicit(&commit_cache.state,memory_order_relaxed));
    commit_cache_excluded_ino=excluded;
    if(excluded)data_cache_windows++;
    commit_cache.windows++;
    atomic_store_explicit(&commit_cache.state,1,memory_order_release);
#ifdef NVFIO_TEST
    if(!cc_pause_only_data || excluded){atomic_store(&cc_owner_entered,1);while(atomic_load(&cc_hold_owner))usleep(100);}
#endif
    return 1;
}
static void commit_cache_close(void)
{nvcc_close(&commit_cache.state);commit_cache_excluded_ino=0;}
static int commit_cache_enter(void)
{
    /* Read-only failed probes avoid bouncing a line when no commit is open. */
    if(!commit_cache_enabled || atomic_load_explicit(&commit_cache.state,memory_order_relaxed)!=1 ||
       !nvcc_enter(&commit_cache.state))return 0;
#ifdef NVFIO_TEST
    atomic_store(&cc_client_entered,1);while(atomic_load(&cc_hold_client))usleep(100);
#endif
    return 1;
}
static int commit_cache_read(int fd,void *data,int count,uint64_t offset,int *result)
{
    if(count>65536 || !commit_cache_enter())return 0;
    int hit=cached_copy(&rt.handle->nvh_sb,fd,data,count,offset,result);
    if(hit){commit_cache.reads++;if(commit_cache_excluded_ino)data_cache_reads++;}else commit_cache.misses++;
    nvcc_leave(&commit_cache.state);return hit;
}
static int commit_cache_write(int fd,const void *data,size_t count,uint64_t offset,int *result)
{
    if(!count || count>8192 || !commit_cache_enter())return 0;
    if(commit_cache_excluded_ino &&
       nvfuse_get_file_table(&rt.handle->nvh_sb,fd)->ino==commit_cache_excluded_ino){
        commit_cache.misses++;nvcc_leave(&commit_cache.state);return 0;
    }
    int n=nvfuse_commit_cached_append(&rt.handle->nvh_sb,fd,data,(unsigned)count,offset);
    if(n && commit_cache_excluded_ino){data_cache_writes++;data_cache_bytes+=(unsigned)n;}
    if(n){commit_cache.writes++;commit_cache.bytes+=(unsigned)n;changed_epoch();*result=n;}
    else commit_cache.misses++;
    nvcc_leave(&commit_cache.state);return n>0;
}
/* Return 1 with ordinary fs_gate held, 2 with completed concurrent short/full
 * pwrite. Original bounded spin/sleep remains for every ineligible request. */
static int commit_cache_write_gate(int fd,const void *data,size_t count,uint64_t offset,int *result)
{
    if(!commit_cache_enabled){
        lock_fs_gate();return 1;
    }
    if(!pthread_mutex_trylock(&fs_gate))return 1;
    uint64_t start=spdk_get_ticks(),ticks=spdk_get_ticks_hz()*gate_spin_us/1000000;
    do{
        if(commit_cache_write(fd,data,count,offset,result))return 2;
        if(!pthread_mutex_trylock(&fs_gate))return 1;
        nvcc_pause();
    }while(spdk_get_ticks()-start<ticks);
    lock_fs_gate();
    return 1;
}
