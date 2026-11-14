/* Standalone buffered-data writeback. The exclusive fs_gate is
 * held throughout these functions. Only the existing I/O reactor executes
 * bdev requests. Metadata and journal transaction boundaries are unchanged. */
#define NVWB_MAX AIO_MAX_QDEPTH
#define NVWB_MIN 16u
#define NVWB_CHECK_BYTES (64u*1024u)
#define NVWB_FILE_BYTES (256u*1024u)
static int buffered_wb_enabled;
static unsigned buffered_wb_window=512;
static int buffered_wb_cursor_enabled;
static struct {
    struct reactor_task *task;
    struct io_job *jobs[NVWB_MAX];
    struct nvfuse_buffer_cache *pages[NVWB_MAX];
    unsigned count,issued,seen;
    uint64_t batches,submitted,completed,drains,overlaps,scans,cursor_skips;
    uint64_t scan_end[MAX_OPEN_FILE];
    inode_t scan_inode[MAX_OPEN_FILE];
    uint64_t since_sync[MAX_OPEN_FILE],check_bytes[MAX_OPEN_FILE];
} buffered_wb;

#ifdef NVFIO_TEST
static atomic_int wb_test_pause,wb_test_entered;
void nvfio_test_buffered_wb_pause(int pause){if(pause)atomic_store(&wb_test_entered,0);atomic_store(&wb_test_pause,pause);}
int nvfio_test_buffered_wb_entered(void){return atomic_load(&wb_test_entered);}
#ifndef NVFUSE_USE_CEPH_SPDK
static void buffered_wb_test_cb(struct spdk_bdev_io*io,bool success,void*arg)
#else
static void buffered_wb_test_cb(struct spdk_bdev_io*io,enum spdk_bdev_io_status success,void*arg)
#endif
{
    atomic_store(&wb_test_entered,1);
    while(atomic_load(&wb_test_pause))usleep(100);
    reactor_bio_cb(io,success,arg);
}
#endif

static void buffered_wb_poll(int wait)
{
    if(!buffered_wb.count)return;
    struct io_job *done[NVWB_MAX];
    int n=reactor_cq_get_reqs(buffered_wb.task,done,wait?1:0,
                             buffered_wb.issued-buffered_wb.seen);
    assert(n>=0);
    for(int i=0;i<n;i++)assert(!done[i]->ret);
    buffered_wb.seen+=(unsigned)n;
    if(buffered_wb.seen!=buffered_wb.issued)return;
    struct nvfuse_superblock *sb=&rt.handle->nvh_sb;
    struct nvfuse_buffer_manager *bm=sb->sb_bm;
    SPINLOCK_LOCK(&bm->bm_lock);
    for(unsigned i=0;i<buffered_wb.count;i++){
        struct nvfuse_buffer_cache *bc=buffered_wb.pages[i];
        SPINLOCK_LOCK(&bc->bc_lock);
        /* No operation that could modify/free these pages can pass the
         * generic barrier or typed-write overlap check before this drain. */
        assert(rte_atomic32_read(&bc->bc_ref)==1 && bc->bc_dirty &&
               !bc->bc_flush && bc->bc_list_type==BUFFER_TYPE_REF);
        bc->bc_dirty=0;nvfuse_dec_bc_ref(bc);
        nvfuse_move_buffer_list_nolock(sb,bc,BUFFER_TYPE_CLEAN,INSERT_HEAD);
        SPINLOCK_UNLOCK(&bc->bc_lock);
    }
    SPINLOCK_UNLOCK(&bm->bm_lock);
    buffered_wb.completed+=buffered_wb.count;
    nvfuse_release_jobs(sb,buffered_wb.jobs,buffered_wb.count);
    buffered_wb.count=buffered_wb.issued=buffered_wb.seen=0;
}
static void buffered_wb_drain(void)
{
    if(buffered_wb.count)buffered_wb.drains++;
    while(buffered_wb.count)buffered_wb_poll(1);
}
static void buffered_wb_finish(void)
{
    buffered_wb_drain();
    if(buffered_wb.task){
        reactor_wait_idle(rt.handle->nvh_target);
        reactor_free_task(rt.handle->nvh_target,buffered_wb.task);
        buffered_wb.task=NULL;
    }
    printf("NVFUSE_BUFFERED_WB enabled=%d batches=%" PRIu64 " submitted=%" PRIu64
           " completed=%" PRIu64 " drains=%" PRIu64 " overlaps=%" PRIu64 " window=%u scans=%" PRIu64 " cursor=%d cursor_skips=%" PRIu64 "\n",
           buffered_wb_enabled,buffered_wb.batches,buffered_wb.submitted,
           buffered_wb.completed,buffered_wb.drains,buffered_wb.overlaps,buffered_wb_window,buffered_wb.scans,buffered_wb_cursor_enabled,buffered_wb.cursor_skips);
}
static void buffered_wb_overlap(inode_t ino,uint64_t offset,size_t bytes)
{
    if(!bytes||!buffered_wb.count)return;
    uint64_t first=offset/CLUSTER_SIZE,last=(offset+bytes-1)/CLUSTER_SIZE;
    u64 firstkey,lastkey;nvfuse_make_pbno_key(ino,first,&firstkey,NVFUSE_BP_TYPE_DATA);
    nvfuse_make_pbno_key(ino,last,&lastkey,NVFUSE_BP_TYPE_DATA);
    for(unsigned i=0;i<buffered_wb.count;i++)if(buffered_wb.pages[i]->bc_bno>=firstkey&&buffered_wb.pages[i]->bc_bno<=lastkey){
        buffered_wb.overlaps++;buffered_wb_drain();return;
    }
}
static void buffered_wb_submit(struct nvfuse_superblock *sb,int fd,inode_t ino,uint64_t end)
{
    if(buffered_wb.count||!nvfuse_process_model_is_standalone()||rt.clients||sb->target->journal_aio)return;
    struct nvfuse_inode_ctx *ictx=nvfuse_read_inode(sb,NULL,ino);
    int regular=S_ISREG(ictx->ictx_inode->i_mode);
    nvfuse_release_inode(sb,ictx,NVF_CLEAN);
    if(!regular)return;
    struct io_target *target=sb->target;
    uint64_t last=end/CLUSTER_SIZE,first=last>buffered_wb_window?last-buffered_wb_window:0;
    if(buffered_wb_cursor_enabled){
        /* Hints only reduce search work. They never authorize a DMA buffer,
         * clear a dirty page, or skip the ordinary fsync data flush. Every
         * submitted page still passes current inode/key/state/overlay checks. */
        if(buffered_wb.scan_inode[fd]!=ino||last<buffered_wb.scan_end[fd]){
            buffered_wb.scan_inode[fd]=ino;buffered_wb.scan_end[fd]=0;
        }
        if(buffered_wb.scan_end[fd]>first){
            buffered_wb.cursor_skips+=buffered_wb.scan_end[fd]-first;
            first=buffered_wb.scan_end[fd];
        }
    }
    if(last-first<NVWB_MIN)return;
    struct nvfuse_buffer_manager *bm=sb->sb_bm;
    unsigned count=0;
    SPINLOCK_LOCK(&bm->bm_lock);
    for(uint64_t block=first;block<last;block++){
        buffered_wb.scans++;
        u64 key;nvfuse_make_pbno_key(ino,block,&key,NVFUSE_BP_TYPE_DATA);
        struct nvfuse_buffer_cache *bc=nvfuse_hash_lookup(bm,key);
        if(!bc||rte_atomic32_read(&bc->bc_ref))continue;
        SPINLOCK_LOCK(&bc->bc_lock);
        int ready=bc->bc_load&&bc->bc_dirty&&!bc->bc_flush&&!bc->bc_locked&&
            bc->bc_pno&&!rte_atomic32_read(&bc->bc_bh_count)&&bc->bc_list_type==BUFFER_TYPE_DIRTY;
        if(ready&&!nvj_needs_overlay(target->journal,(uint64_t)bc->bc_pno*CLUSTER_SIZE,CLUSTER_SIZE,1))
            buffered_wb.pages[count++]=bc;
        SPINLOCK_UNLOCK(&bc->bc_lock);
    }
    if(count<NVWB_MIN){SPINLOCK_UNLOCK(&bm->bm_lock);return;}
    for(unsigned i=0;i<count;i++){
        struct nvfuse_buffer_cache *bc=buffered_wb.pages[i];
        SPINLOCK_LOCK(&bc->bc_lock);nvfuse_inc_bc_ref(bc);
        nvfuse_move_buffer_list_nolock(sb,bc,BUFFER_TYPE_REF,INSERT_HEAD);
        SPINLOCK_UNLOCK(&bc->bc_lock);
    }
    SPINLOCK_UNLOCK(&bm->bm_lock);
    if(!buffered_wb.task){
        buffered_wb.task=reactor_alloc_task(target,NVWB_MAX);assert(buffered_wb.task);
        if(reactor_task_enable_polling(target,buffered_wb.task))abort();
    }
    nvfuse_make_jobs(sb,buffered_wb.jobs,count);
    struct io_job *vectors[NVWB_MAX];unsigned issued=0;
    for(unsigned i=0;i<count;i++){
        struct nvfuse_buffer_cache *bc=buffered_wb.pages[i];
        struct io_job *job=buffered_wb.jobs[i];
        job->offset=(uint64_t)bc->bc_pno*CLUSTER_SIZE;job->bytes=CLUSTER_SIZE;
        job->ret=0;job->req_type=SPDK_BDEV_IO_TYPE_WRITE;job->buf=bc->bc_buf;
        job->complete=0;job->iov[0]=(struct iovec){bc->bc_buf,CLUSTER_SIZE};job->iovcnt=1;job->cb=reactor_bio_cb;
#ifdef NVFIO_TEST
        job->cb=buffered_wb_test_cb;
#endif
        struct io_job *previous=issued?vectors[issued-1]:NULL;
        if(previous&&previous->iovcnt<REACTOR_BUFFER_IOVS&&previous->offset+previous->bytes==job->offset){
            previous->iov[previous->iovcnt++]=job->iov[0];previous->bytes+=job->bytes;
        }else vectors[issued++]=job;
    }
    /* This path stages nothing protected. Submission records ordered data
     * writes; every publication/lifecycle callback drains this batch first.
     * Internal eviction may stage metadata, but cannot publish a transaction. */
    buffered_wb.count=count;buffered_wb.issued=issued;buffered_wb.seen=0;
    if(reactor_submit_reqs(target,buffered_wb.task,vectors,issued))abort();
    buffered_wb.batches++;buffered_wb.submitted+=count;
    if(buffered_wb_cursor_enabled)buffered_wb.scan_end[fd]=last;
}
struct buffered_write_request{int fd;const void*data;size_t bytes;uint64_t offset;};
static int buffered_write_work(struct nvfuse_handle*h,void*arg)
{struct buffered_write_request*r=arg;return nvfuse_writefile(h,r->fd,r->data,r->bytes,r->offset);}
int nvfio_pwrite(int fd,const void*data,size_t bytes,uint64_t offset)
{
    if(fd<START_OPEN_FILE||fd>=MAX_OPEN_FILE||bytes>INT32_MAX||offset>INT64_MAX||bytes>(uint64_t)INT64_MAX-offset||(!data&&bytes))return -EINVAL;
    struct buffered_write_request request={fd,data,bytes,offset};lock_fs_gate();
    if(!rt.handle){pthread_mutex_unlock(&fs_gate);return -EIO;}
    if(rt.clients){pthread_mutex_unlock(&fs_gate);return nvfio_execute(buffered_write_work,&request);}
    struct nvfuse_file_table *of=nvfuse_get_file_table(&rt.handle->nvh_sb,fd);
    if(!of->used||(of->flags&O_ACCMODE)==O_RDONLY){pthread_mutex_unlock(&fs_gate);return -EBADF;}
    if(of->flags&O_DIRECT){pthread_mutex_unlock(&fs_gate);return -EINVAL;}
    buffered_wb_poll(0);
    int sync_flags=of->flags&(O_SYNC|O_DSYNC);
    if(sync_flags)buffered_wb_drain();else buffered_wb_overlap(of->ino,offset,bytes);
    errno=0;int result=buffered_write_work(rt.handle,&request);changed_epoch();
    if(result>0&&buffered_wb_enabled&&rt.handle->nvh_target->journal&&!rt.handle->nvh_target->journal_aio&&!sync_flags){
        /* These byte counters are scheduling hints, never an identity/lifetime
         * proof. Current inode and cache keys govern every submitted page. */
        uint64_t n=(unsigned)result;
        if(buffered_wb.since_sync[fd]<NVWB_FILE_BYTES)buffered_wb.since_sync[fd]+=n;
        buffered_wb.check_bytes[fd]+=n;
        if(buffered_wb.since_sync[fd]>=NVWB_FILE_BYTES&&buffered_wb.check_bytes[fd]>=NVWB_CHECK_BYTES){
            buffered_wb.check_bytes[fd]=0;buffered_wb_submit(&rt.handle->nvh_sb,fd,of->ino,offset+n);
        }
    }
    pthread_mutex_unlock(&fs_gate);return result;
}
struct buffered_sync_request{int fd;};
static int buffered_sync_work(struct nvfuse_handle*h,void*arg)
{struct buffered_sync_request*r=arg;return r->fd<0?nvfuse_sync(h):nvfuse_fsync(h,r->fd);}
int nvfio_fsync(int fd)
{
    if(fd < -1 || (fd >= 0 && fd < START_OPEN_FILE) || fd >= MAX_OPEN_FILE)return -EINVAL;
    struct buffered_sync_request request={fd};lock_fs_gate();
    if(!rt.handle){pthread_mutex_unlock(&fs_gate);return -EIO;}
    if(rt.clients){pthread_mutex_unlock(&fs_gate);return nvfio_execute(buffered_sync_work,&request);}
    buffered_wb_drain();errno=0;int result=buffered_sync_work(rt.handle,&request);changed_epoch();
    if(fd>=0&&fd<MAX_OPEN_FILE){buffered_wb.since_sync[fd]=0;buffered_wb.check_bytes[fd]=0;buffered_wb.scan_end[fd]=0;buffered_wb.scan_inode[fd]=0;}
    else if(fd<0){memset(buffered_wb.since_sync,0,sizeof(buffered_wb.since_sync));memset(buffered_wb.check_bytes,0,sizeof(buffered_wb.check_bytes));memset(buffered_wb.scan_end,0,sizeof(buffered_wb.scan_end));memset(buffered_wb.scan_inode,0,sizeof(buffered_wb.scan_inode));}
    pthread_mutex_unlock(&fs_gate);return result;
}
#ifdef NVFIO_TEST
int nvfio_test_buffered_wb_enabled(void){return buffered_wb_enabled;}
uint64_t nvfio_test_buffered_wb_offset(void){lock_fs_gate();uint64_t o=buffered_wb.count?(uint64_t)(uint32_t)buffered_wb.pages[0]->bc_bno*CLUSTER_SIZE:UINT64_MAX;pthread_mutex_unlock(&fs_gate);return o;}
int nvfio_test_buffered_wb_pending(void){lock_fs_gate();int n=buffered_wb.count;pthread_mutex_unlock(&fs_gate);return n;}
#endif
