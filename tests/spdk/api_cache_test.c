/* SPDX-License-Identifier: GPL-2.0-only */

/* Exercise the caller cache against owner mutations, eviction and AIO. */
#include "spdk/stdinc.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_buffer_cache.h"
#define NVFIO_TEST
#include "nvfuse_spdk_runtime.h"
#include "nvfuse_primary_journal.h"
#define nvfio_execute nvfio_execute_serialized
#include <stdatomic.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return -EIO; } } while (0)
#define BYTES (80 * CLUSTER_SIZE + 731)
static int fd, direct_fd;
static atomic_int failed, done, owner_held;
static int hold_owner(struct nvfuse_handle *h, void *unused)
{
    (void)h; (void)unused;
    atomic_store(&owner_held,1);
    /* Prevent the owner poller from submitting the AIO write until the later
     * API read is queued. This forces the formerly racy pending-write case. */
    for(unsigned i=0;i<50000;i++) {
        if(nvfio_test_api_pending())return 0;
        usleep(100);
    }
    return -ETIMEDOUT;
}
static void *hold_thread(void *unused)
{
    (void)unused;
    return (void *)(intptr_t)nvfio_test_control_call(hold_owner,NULL);
}
static int setup(struct nvfuse_handle *h, void *arg)
{
    int create = *(int *)arg;
    fd = nvfuse_openfile_path(h,"/primary-epoch-cache-fixture",create ? O_CREAT|O_RDWR : O_RDONLY,0600);
    CHECK(fd >= 0);
    if (create) {
        char *data = malloc(BYTES); CHECK(data);
        for (unsigned i=0;i<BYTES;i++) data[i]=(char)(i*37+i/251);
        int n=nvfuse_writefile(h,fd,data,BYTES,0); free(data); CHECK(n==BYTES);
        CHECK(!nvfuse_fsync(h,fd));
    }
    return 0;
}
static int verify_bytes(void)
{
    char *data=malloc(BYTES+8192); CHECK(data);
    /* First read after reopen is cold; the second uses cached pages. */
    for (unsigned pass=0;pass<2;pass++) {
        CHECK(nvfio_pread(fd,data,BYTES+8192,0)==BYTES);
        for (unsigned i=0;i<BYTES;i++) CHECK(data[i]==(char)(i*37+i/251));
        for (unsigned i=4093;i<BYTES;i+=7919) {
            unsigned n=BYTES-i<8192?BYTES-i:8192;
            CHECK(nvfio_pread(fd,data,8192,i)==(int)n);
            for(unsigned j=0;j<n;j++) CHECK(data[j]==(char)((i+j)*37+(i+j)/251));
        }
    }
    CHECK(nvfio_pread(fd,data,8192,BYTES)==0);
    CHECK(nvfio_pread(fd,NULL,0,0)==0);
    CHECK(nvfio_pread(-1,data,1,0)==-EINVAL);
    CHECK(nvfio_pread(fd,NULL,1,0)==-EINVAL);
    CHECK(nvfio_pread(fd,data,(size_t)INT32_MAX+1,0)==-EINVAL);
    CHECK(nvfio_pread(fd,data,1,UINT64_MAX)==-EINVAL);
    free(data);return 0;
}
static int invalidate(struct nvfuse_handle *h,void *unused)
{
    (void)unused;
    CHECK(!nvfuse_fsync(h,fd));
    u64 key; nvfuse_make_pbno_key(nvfuse_get_file_table(&h->nvh_sb,fd)->ino,0,&key,NVFUSE_BP_TYPE_DATA);
    nvfuse_move_bc_to_unused_list(&h->nvh_sb,key);
    return 0;
}
static int close_fd(struct nvfuse_handle *h,void *unused)
{
    (void)unused;return nvfuse_closefile(h,fd);
}
static int access_test(struct nvfuse_handle *h,void *unused)
{
    (void)unused;fd=nvfuse_openfile_path(h,"/primary-epoch-cache-fixture",O_WRONLY,0);CHECK(fd>=0);return 0;
}
static int mutate(struct nvfuse_handle *h,void *arg)
{
    unsigned round=*(unsigned*)arg;
    CHECK(!nvfuse_ftruncate(h,fd,0));
    if(round%3) {
        char data[8192];memset(data,1+round%251,sizeof(data));
        CHECK(nvfuse_writefile(h,fd,data,sizeof(data),0)==sizeof(data));
    }
    if(round%17==0)CHECK(!nvfuse_fsync(h,fd));
    return 0;
}
static void *reader(void *unused)
{
    (void)unused;char data[8192];unsigned reads=0;
    do {
        int n=nvfio_pread(fd,data,sizeof(data),0);
        if(n!=0 && n!=sizeof(data)){atomic_store(&failed,1);break;}
        for(int j=1;j<n;j++)if(data[j]!=data[0])atomic_store(&failed,1);
        reads++;
    }while(!atomic_load(&done)||reads<3000);
    return NULL;
}
static int prepare_concurrent(struct nvfuse_handle *h,void *unused)
{
    (void)unused;fd=nvfuse_openfile_path(h,"/primary-epoch-cache-concurrent",O_CREAT|O_RDWR,0600);CHECK(fd>=0);
    unsigned round=1;return mutate(h,&round);
}
static int concurrency(void)
{
    CHECK(!nvfio_execute(prepare_concurrent,NULL));
    pthread_t readers[4];
    for(unsigned i=0;i<4;i++)CHECK(!pthread_create(&readers[i],NULL,reader,NULL));
    int error=0;
    for(unsigned i=0;i<1000;i++)if(nvfio_execute(mutate,&i)){error=1;break;}
    atomic_store(&done,1);
    for(unsigned i=0;i<4;i++)CHECK(!pthread_join(readers[i],NULL));
    CHECK(!error&&!atomic_load(&failed));CHECK(!nvfio_execute(close_fd,NULL));return 0;
}
static int prepare_aio(struct nvfuse_handle *h,void *unused)
{
    (void)unused;fd=nvfuse_openfile_path(h,"/primary-epoch-cache-aio",O_CREAT|O_RDWR|O_DIRECT,0600);CHECK(fd>=0);
    char data[8192];memset(data,0x11,sizeof(data));
    CHECK(nvfuse_writefile(h,fd,data,sizeof(data),0)==sizeof(data));CHECK(!nvfuse_fsync(h,fd));
    /* This case tests dispatch/draining with cold pages. Mixing direct writes
     * into already-cached buffered pages requires separate coherence rules. */
    for(unsigned i=0;i<2;i++) {
        u64 key;nvfuse_make_pbno_key(nvfuse_get_file_table(&h->nvh_sb,fd)->ino,i,&key,NVFUSE_BP_TYPE_DATA);
        nvfuse_move_bc_to_unused_list(&h->nvh_sb,key);
    }
    direct_fd = fd;
    fd=nvfuse_openfile_path(h,"/primary-epoch-cache-aio",O_RDONLY,0);CHECK(fd>=0);
    return 0;
}
static int aio(void)
{
    CHECK(!nvfio_execute(prepare_aio,NULL));
    struct nvfio_client *client=nvfio_client_alloc(1);CHECK(client);
    struct nvfio_request *request=nvfio_request_alloc(NULL),*completed[1];CHECK(request);
    void *data=nvfio_dma_alloc(8192);CHECK(data);memset(data,0x5a,8192);
    char actual[8192];
    CHECK(nvfio_pread(direct_fd,actual,sizeof(actual),0)==-EINVAL);
    pthread_t holder;CHECK(!pthread_create(&holder,NULL,hold_thread,NULL));
    while(!atomic_load(&owner_held))usleep(100);
    CHECK(!nvfio_submit(client,request,direct_fd,data,8192,0,1));
    /* A live client forces dispatch/drain, even if DMA has completed already. */
    CHECK(nvfio_pread(fd,actual,sizeof(actual),0)==sizeof(actual));
    void *held_result;CHECK(!pthread_join(holder,&held_result)&&!held_result);
    for(unsigned i=0;i<sizeof(actual);i++)CHECK((unsigned char)actual[i]==0x5a);
    int n;do {n=nvfio_poll(client,1,completed);}while(n==0);
    CHECK(n==1&&completed[0]==request&&!nvfio_request_error(request));
    CHECK(!nvfio_sync(client,direct_fd));CHECK(!nvfio_close(direct_fd));CHECK(!nvfio_client_free(client));
    nvfio_request_free(request);nvfio_dma_free(data);
    CHECK(nvfio_pread(fd,actual,sizeof(actual),0)==sizeof(actual));
    for(unsigned i=0;i<sizeof(actual);i++)CHECK((unsigned char)actual[i]==0x5a);
    CHECK(!nvfio_execute(close_fd,NULL));return 0;
}
#define WB_VERIFY_BYTES (1024*1024+731)
static int wb_fd,wb_action;
static unsigned wb_written;
static uint64_t wb_overlap_at;
static atomic_int wb_mutation_started;
static atomic_int wb_mutation_done;
static char wb_pattern(unsigned i){return(char)(i*41+i/233+37);}
static int wb_open(struct nvfuse_handle*h,void*arg)
{
    int create=*(int*)arg;
    wb_fd=nvfuse_openfile_path(h,"/primary-durable",create?O_RDWR|O_CREAT:O_RDONLY,0600);CHECK(wb_fd>=0);
    if(create)CHECK(!nvfuse_ftruncate(h,wb_fd,0));return 0;
}
static int wb_open_retire(struct nvfuse_handle*h,void*unused)
{
    (void)unused;wb_fd=nvfuse_openfile_path(h,"/primary-retire",O_RDWR|O_CREAT,0600);CHECK(wb_fd>=0);
    CHECK(!nvfuse_ftruncate(h,wb_fd,0));return 0;
}
static int wb_close(struct nvfuse_handle*h,void*unused){(void)unused;return nvfuse_closefile(h,wb_fd);}
static int wb_append(unsigned bytes)
{
    char buffer[32768];for(unsigned i=0;i<bytes;i++)buffer[i]=wb_pattern(wb_written+i);
    int n=nvfio_pwrite(wb_fd,buffer,bytes,wb_written);if(n!=(int)bytes)return -EIO;
    wb_written+=bytes;return 0;
}
static int wb_start_paused(void)
{
    wb_written=0;nvfio_test_buffered_wb_pause(1);int error=0;
    for(unsigned i=0;i<64;i++){
        if(wb_append(32768)){error=1;break;}
        if(nvfio_test_buffered_wb_pending())break;
    }
    if(!nvfio_test_buffered_wb_pending())error=1;
    unsigned attempts=0;while(!nvfio_test_buffered_wb_entered()&&attempts++<10000)usleep(100);
    if(!nvfio_test_buffered_wb_entered())error=1;
    if(error)nvfio_test_buffered_wb_pause(0);
    return error?-EIO:0;
}
static int wb_mutate(struct nvfuse_handle*h,void*unused)
{
    (void)unused;
    if(wb_action==1)CHECK(!nvfuse_ftruncate(h,wb_fd,0));
    else{
        CHECK(!nvfuse_closefile(h,wb_fd));CHECK(!nvfuse_unlink(h,"/primary-retire"));
        wb_fd=nvfuse_openfile_path(h,"/primary-reused",O_RDWR|O_CREAT,0600);CHECK(wb_fd>=0);
        CHECK(!nvfuse_ftruncate(h,wb_fd,0));
    }
    char b[8192];memset(b,0x5a,sizeof(b));CHECK(nvfuse_writefile(h,wb_fd,b,sizeof(b),0)==sizeof(b));
    CHECK(!nvfuse_fsync(h,wb_fd));return 0;
}
static void *wb_mutation_thread(void*unused)
{
    (void)unused;int result;atomic_store(&wb_mutation_started,1);
    if(wb_action==0){char z=wb_pattern((unsigned)wb_overlap_at);result=nvfio_pwrite(wb_fd,&z,1,wb_overlap_at);if(result==1)result=0;}
    else if(wb_action==3){struct nvfio_client*c=nvfio_client_alloc(1);result=c?nvfio_client_free(c):-EIO;}
    else if(wb_action==4){char b;int n=nvfio_pread(wb_fd,&b,1,wb_overlap_at);result=n==1&&b==wb_pattern((unsigned)wb_overlap_at)?0:-EIO;}
    else result=nvfio_execute(wb_mutate,NULL);
    atomic_store(&wb_mutation_done,1);return(void*)(intptr_t)result;
}
static int wb_check_mutation_wait(int action)
{
    wb_action=action;atomic_store(&wb_mutation_done,0);atomic_store(&wb_mutation_started,0);
    wb_overlap_at=nvfio_test_buffered_wb_offset();CHECK(wb_overlap_at!=UINT64_MAX);
    printf("WB_EXCLUSION action=%d pinned_offset=%" PRIu64 "\n",action,wb_overlap_at);pthread_t worker;
    int started=pthread_create(&worker,NULL,wb_mutation_thread,NULL);
    if(started){nvfio_test_buffered_wb_pause(0);return -EIO;}
    unsigned tries=0;while(!atomic_load(&wb_mutation_started)&&tries++<10000)usleep(100);
    int started_ok=atomic_load(&wb_mutation_started);usleep(20000);int early=atomic_load(&wb_mutation_done);
    nvfio_test_buffered_wb_pause(0);void*result;CHECK(!pthread_join(worker,&result)&&!result);CHECK(started_ok&&!early);
    CHECK(!nvfio_test_buffered_wb_pending());return 0;
}
static int wb_verify_durable(void)
{
    char data[65536];
    for(unsigned off=0;off<WB_VERIFY_BYTES;){unsigned n=WB_VERIFY_BYTES-off;if(n>sizeof(data))n=sizeof(data);
        CHECK(nvfio_pread(wb_fd,data,n,off)==(int)n);
        for(unsigned i=0;i<n;i++)CHECK(data[i]==((off+i)==0?'Z':wb_pattern(off+i)));off+=n;
    }
    CHECK(nvfio_pread(wb_fd,data,1,WB_VERIFY_BYTES)==0);return 0;
}
static int wb_unrelated_fd;
static atomic_int wb_unrelated_done;
static int wb_unrelated_setup(struct nvfuse_handle*h,void*unused)
{
    (void)unused;wb_unrelated_fd=nvfuse_openfile_path(h,"/wb-extra-loaded-read",O_CREAT|O_RDWR,0600);CHECK(wb_unrelated_fd>=0);
    CHECK(!nvfuse_ftruncate(h,wb_unrelated_fd,0));char data[4096];memset(data,0x33,sizeof(data));
    CHECK(nvfuse_writefile(h,wb_unrelated_fd,data,sizeof(data),0)==sizeof(data));CHECK(!nvfuse_fsync(h,wb_unrelated_fd));return 0;
}
static int wb_unrelated_close(struct nvfuse_handle*h,void*unused)
{(void)unused;return nvfuse_closefile(h,wb_unrelated_fd);}
static void *wb_unrelated_read(void*unused)
{
    (void)unused;char b[8];int rc=nvfio_pread(wb_unrelated_fd,b,sizeof(b),17);
    int ok=rc==sizeof(b);for(unsigned i=0;ok&&i<sizeof(b);i++)ok=b[i]==0x33;
    atomic_store(&wb_unrelated_done,1);return(void*)(intptr_t)(ok?0:-EIO);
}
static int wb_unrelated_test(void)
{
    CHECK(!nvfio_execute(wb_unrelated_setup,NULL));char b[8];CHECK(nvfio_pread(wb_unrelated_fd,b,sizeof(b),17)==sizeof(b));
    CHECK(!nvfio_execute(wb_open_retire,NULL));CHECK(!wb_start_paused());
    atomic_store(&wb_unrelated_done,0);pthread_t worker;int rc=pthread_create(&worker,NULL,wb_unrelated_read,NULL);
    if(rc){nvfio_test_buffered_wb_pause(0);return -EIO;}
    unsigned tries=0;while(!atomic_load(&wb_unrelated_done)&&tries++<5000)usleep(100);
    int early=atomic_load(&wb_unrelated_done);nvfio_test_buffered_wb_pause(0);void*result;
    CHECK(!pthread_join(worker,&result)&&!result&&early);CHECK(!nvfio_fsync(wb_fd));CHECK(!nvfio_execute(wb_close,NULL));
    CHECK(!nvfio_execute(wb_unrelated_close,NULL));printf("PASS loaded unrelated read completed during paused buffered DMA\n");return 0;
}
static int buffered_wb_test(int create)
{
    if(!nvfio_test_buffered_wb_enabled())return 0;
    CHECK(!nvfio_execute(wb_open,&create));
    if(create){
        CHECK(!wb_start_paused());CHECK(!wb_check_mutation_wait(0));
        char z='Z';CHECK(nvfio_pwrite(wb_fd,&z,1,0)==1);
        while(wb_written<WB_VERIFY_BYTES){unsigned n=WB_VERIFY_BYTES-wb_written;if(n>32768)n=32768;CHECK(!wb_append(n));}
        CHECK(!nvfio_fsync(wb_fd));CHECK(!nvfio_test_buffered_wb_pending());
    }
    char invalid='x';CHECK(nvfio_pwrite(-1,&invalid,1,0)==-EINVAL);
    CHECK(nvfio_pwrite(wb_fd,NULL,1,0)==-EINVAL);CHECK(nvfio_pwrite(wb_fd,&invalid,1,UINT64_MAX)==-EINVAL);
    if(!create)CHECK(nvfio_pwrite(wb_fd,&invalid,1,0)==-EBADF);
    CHECK(!wb_verify_durable());CHECK(!nvfio_execute(wb_close,NULL));
    CHECK(nvfio_pwrite(wb_fd,&invalid,1,0)==-EBADF);
    if(!create){printf("PASS buffered writeback reopened bytes and EOF\n");return 0;}
    for(int action=1;action<=4;action++){
        CHECK(!nvfio_execute(wb_open_retire,NULL));CHECK(!wb_start_paused());CHECK(!wb_check_mutation_wait(action));
        if(action<3){char data[8193];CHECK(nvfio_pread(wb_fd,data,sizeof(data),0)==8192);for(unsigned i=0;i<8192;i++)CHECK(data[i]==0x5a);}
        CHECK(!nvfio_fsync(wb_fd));CHECK(!nvfio_execute(wb_close,NULL));
    }
    CHECK(!wb_unrelated_test());
    printf("PASS buffered writeback overlap/truncate/unlink/AIO exclusion\n");return 0;
}
static int buffered_wb_crash_main(int argc,char**argv)
{
    (void)argc;CHECK(!nvfio_start(argv[1],argv[2],0));int create=!strcmp(argv[3],"wb-crash");
    int result=buffered_wb_test(create);if(result){fflush(NULL);_exit(88);}
    if(create){printf("EXPECTED_CRASH buffered-fsync-returned\n");fflush(NULL);_exit(86);}
    CHECK(!nvfio_stop());return 0;
}

#define WB_FAULT_OLD_BYTES (16*CLUSTER_SIZE+17)
static int wb_fault_fd;
static char wb_fault_pattern(unsigned i){return(char)(i*17+i/211+59);}
static int wb_fault_open(struct nvfuse_handle*h,void*arg)
{
    const char*mode=arg;int seed=!strcmp(mode,"wb-fault-seed"),check=!strcmp(mode,"wb-fault-check");
    wb_fault_fd=nvfuse_openfile_path(h,"/wb-extra-fault-prefix",seed?O_CREAT|O_RDWR:check?O_RDONLY:O_RDWR,0600);CHECK(wb_fault_fd>=0);
    if(seed)CHECK(!nvfuse_ftruncate(h,wb_fault_fd,0));return 0;
}
static int wb_fault_close(struct nvfuse_handle*h,void*unused)
{(void)unused;return nvfuse_closefile(h,wb_fault_fd);}
static int wb_fault_payload(void*arg,uint64_t offset,size_t bytes,int write_op)
{
    (void)bytes;int payload=nvj_version(arg)==3?
        offset>=(uint64_t)(NVJ_START+3)*CLUSTER_SIZE&&offset<(uint64_t)(NVJ_START+NVJ_BLOCKS)*CLUSTER_SIZE:
        offset==(uint64_t)(NVJ_START+NVJ_PAYLOAD)*CLUSTER_SIZE;
    return write_op&&payload?-EIO:0;
}
static int wb_fault_sync(struct nvfuse_handle*h,void*unused)
{
    (void)unused;nvj_set_io_fault_hook(h->nvh_target->journal,wb_fault_payload,h->nvh_target->journal);
    int rc=nvfuse_fsync(h,wb_fault_fd);CHECK(rc<0&&nvj_recovery_required(h->nvh_target->journal));
    printf("EXPECTED_IO_FAILURE buffered-data-drained metadata-payload-failed recovery_required=1\n");fflush(NULL);_exit(87);
}
static int wb_fault_case(const char*mode)
{
    CHECK(nvfio_test_buffered_wb_enabled());
    CHECK(!nvfio_test_pause_periodic_journal());
    CHECK(!nvfio_execute(wb_fault_open,(void*)mode));
    if(!strcmp(mode,"wb-fault-seed")){
        char*data=malloc(WB_FAULT_OLD_BYTES);CHECK(data);for(unsigned i=0;i<WB_FAULT_OLD_BYTES;i++)data[i]=wb_fault_pattern(i);
        CHECK(nvfio_pwrite(wb_fault_fd,data,WB_FAULT_OLD_BYTES,0)==WB_FAULT_OLD_BYTES);free(data);CHECK(!nvfio_fsync(wb_fault_fd));
    }else if(!strcmp(mode,"wb-fault-crash")){
        char data[32768];unsigned appended=0;int pending=0;
        while(appended<2*1024*1024){for(unsigned i=0;i<sizeof(data);i++)data[i]=wb_fault_pattern(WB_FAULT_OLD_BYTES+appended+i);
            CHECK(nvfio_pwrite(wb_fault_fd,data,sizeof(data),WB_FAULT_OLD_BYTES+appended)==sizeof(data));appended+=sizeof(data);
            if(nvfio_test_buffered_wb_pending()){pending=1;break;}}
        CHECK(pending);printf("WB_FAULT actual_pending=1 appended=%u\n",appended);
        return nvfio_execute(wb_fault_sync,NULL); /* Generic barrier must drain DMA before faulted metadata publication. */
    }else{
        char*data=malloc(WB_FAULT_OLD_BYTES+1);CHECK(data);CHECK(nvfio_pread(wb_fault_fd,data,WB_FAULT_OLD_BYTES+1,0)==WB_FAULT_OLD_BYTES);
        for(unsigned i=0;i<WB_FAULT_OLD_BYTES;i++)CHECK(data[i]==wb_fault_pattern(i));CHECK(nvfio_pread(wb_fault_fd,data,1,WB_FAULT_OLD_BYTES)==0);free(data);
        printf("PASS failed buffered fsync recovered original prefix and EOF\n");
    }
    CHECK(!nvfio_execute(wb_fault_close,NULL));return 0;
}
static int wb_fault_main(char**argv)
{
    if (nvfio_start(argv[1], argv[2], 0)) _exit(88);
    int rc = wb_fault_case(argv[3]);
    if (rc) { fflush(NULL); _exit(88); }
    CHECK(!nvfio_stop());printf("PASS buffered fault fixture %s\n",argv[3]);return 0;
}
int main(int argc,char **argv)
{
    if(argc==4&&!strncmp(argv[3],"wb-fault-",9))return wb_fault_main(argv);
    if(argc==4&&!strncmp(argv[3],"wb-",3))return buffered_wb_crash_main(argc,argv);
    if(argc!=4){fprintf(stderr,"usage: %s config bdf create|check\n",argv[0]);return 2;}
    int create=!strcmp(argv[3],"create"),rc=nvfio_start(argv[1],argv[2],0);
    if(rc)return 1;
    rc=nvfio_execute(setup,&create);
    if(!rc)rc=verify_bytes();
    if(!rc)rc=nvfio_execute(invalidate,NULL);
    if(!rc)rc=verify_bytes();
    if(!rc)rc=nvfio_execute(close_fd,NULL);
    char byte;
    if(!rc&&nvfio_pread(fd,&byte,1,0)!=-EBADF)rc=-EIO;
    if(!rc)rc=nvfio_execute(access_test,NULL);
    if(!rc&&nvfio_pread(fd,&byte,1,0)!=-EBADF)rc=-EIO;
    if(!rc)rc=nvfio_execute(close_fd,NULL);
    if(!rc&&create)rc=concurrency();
    if(!rc&&create)rc=aio();
    if(!rc)rc=buffered_wb_test(create);
    int stop=nvfio_stop();if(!rc)rc=stop;
    if(!rc)printf("PASS API caller cache %s\n",argv[3]);
    if(rc){fflush(NULL);_exit(1);}
    return 0;
}
