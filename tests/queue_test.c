/* SPDX-License-Identifier: GPL-2.0-only */

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_tx.h"
#include "nvfuse_queue.h"
#include "nvfuse_direct.h"
#include <stdatomic.h>
#include <sched.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL queue %d: %s\n",__LINE__,#x);abort(); } } while(0)
#define THREADS 4u
#define ROUNDS 128u
struct producer { struct nvfuse_queue *q;unsigned fd,id;_Atomic unsigned *done; };
static struct nvfuse_future *submit(struct producer *p,enum nvfuse_queue_operation op,void *buf,unsigned n,int64_t at) {
    struct nvfuse_future *f;
    while(!(f=nvfuse_queue_submit(p->q,op,p->fd,buf,n,at))) { CHECK(errno==EAGAIN);sched_yield(); }
    return f;
}
static void *produce(void *arg) {
    struct producer *p=arg;void *buf=NULL;CHECK(!posix_memalign(&buf,4096,4096));
    CHECK(nvfuse_queue_poll(p->q,1)==-EPERM);CHECK(nvfuse_queue_destroy(p->q)==-EPERM);
    for(unsigned i=0;i<ROUNDS;i++) {
        memset(buf,(int)(1+p->id*31+i%23),4096);
        enum nvfuse_queue_operation op=i%2?NVFUSE_QUEUE_DIRECT_WRITE:NVFUSE_QUEUE_WRITE;
        CHECK(nvfuse_future_wait(submit(p,op,buf,4096,(int64_t)i*4096))==4096);
        memset(buf,0,4096);op=i%2?NVFUSE_QUEUE_READ:NVFUSE_QUEUE_DIRECT_READ;
        CHECK(nvfuse_future_wait(submit(p,op,buf,4096,(int64_t)i*4096))==4096);
        for(unsigned k=0;k<4096;k++) CHECK(((unsigned char *)buf)[k]==1+p->id*31+i%23);
    }
    free(buf);atomic_fetch_add(p->done,1);return NULL;
}
int main(int argc,char **argv) {
    CHECK(argc==2);struct nvfuse_handle *h=nvfuse_file_open(argv[1],1,128ULL*1024*1024);CHECK(h);
    int fds[THREADS];for(unsigned i=0;i<THREADS;i++) { char path[32];snprintf(path,sizeof(path),"/q%u",i);fds[i]=nvfuse_openfile_path(h,path,O_CREAT|O_RDWR,0644);CHECK(fds[i]>=0); }
    for(unsigned mode=0;mode<=2;mode++) {
        struct nvfuse_queue *q=nvfuse_queue_create(h,16,mode);CHECK(q);
        CHECK(nvfuse_file_close(h)==-EBUSY);
        _Atomic unsigned done=0;pthread_t threads[THREADS];struct producer p[THREADS];
        for(unsigned i=0;i<THREADS;i++) { p[i]=(struct producer){q,(unsigned)fds[i],i,&done};CHECK(!pthread_create(&threads[i],NULL,produce,&p[i])); }
        while(atomic_load(&done)<THREADS) { CHECK(nvfuse_queue_poll(q,8)>=0);sched_yield(); }
        for(unsigned i=0;i<THREADS;i++) CHECK(!pthread_join(threads[i],NULL));
        struct nvfuse_queue_stats stats;nvfuse_queue_get_stats(q,&stats);
        CHECK(stats.submitted==THREADS*ROUNDS*2 && stats.completed==stats.submitted && !stats.cancelled);
        CHECK(!nvfuse_queue_destroy(q));
    }
    void *buf=NULL,*read=NULL;CHECK(!posix_memalign(&buf,4096,4096));CHECK(!posix_memalign(&read,4096,4096));memset(buf,'X',4096);
    struct nvfuse_queue *q=nvfuse_queue_create(h,4,2);CHECK(q);
    struct nvfuse_future *a=nvfuse_queue_submit(q,NVFUSE_QUEUE_DIRECT_WRITE,fds[0],buf,4096,0);
    struct nvfuse_future *b=nvfuse_queue_submit(q,NVFUSE_QUEUE_WRITE,fds[1],buf,4096,0);
    struct nvfuse_future *bad=nvfuse_queue_submit(q,NVFUSE_QUEUE_WRITE,999,buf,4096,0);CHECK(a && b && bad);
    CHECK(nvfuse_queue_poll(q,4)==3);
    CHECK(nvfuse_future_wait(a)==-EBADF && nvfuse_future_wait(b)==-EBADF && nvfuse_future_wait(bad)==-EBADF);
    for(unsigned i=0;i<2;i++) {
        CHECK(nvfuse_pread_direct(h,(unsigned)fds[i],read,4096,0)==4096);
        for(unsigned k=0;k<4096;k++) CHECK(((unsigned char *)read)[k]==1+i*31);
    }
    a=nvfuse_queue_submit(q,NVFUSE_QUEUE_WRITE,fds[0],buf,4096,0);CHECK(a);
    nvfuse_queue_stop(q);CHECK(!nvfuse_queue_submit(q,NVFUSE_QUEUE_READ,fds[0],buf,4096,0) && errno==ECANCELED);
    CHECK(!nvfuse_queue_destroy(q));CHECK(nvfuse_future_wait(a)==-ECANCELED);
    /* Two descriptors for one inode: an older descriptor cannot shrink the
     * size published by a later extension. FIFO defines overlapping writes. */
    int alias=nvfuse_openfile_path(h,"/q0",O_RDWR,0);CHECK(alias>=0);
    q=nvfuse_queue_create(h,4,2);CHECK(q);
    memset(buf,'L',4096);memset(read,'R',4096);
    a=nvfuse_queue_submit(q,NVFUSE_QUEUE_DIRECT_WRITE,(unsigned)alias,buf,4096,ROUNDS*4096);
    b=nvfuse_queue_submit(q,NVFUSE_QUEUE_WRITE,(unsigned)fds[0],read,4096,0);CHECK(a && b);
    CHECK(nvfuse_queue_poll(q,4)==2);CHECK(nvfuse_future_wait(a)==4096 && nvfuse_future_wait(b)==4096);
    struct stat st;CHECK(!nvfuse_getattr(h,"/q0",&st) && st.st_size==(ROUNDS+1)*4096);
    CHECK(nvfuse_pread_direct(h,(unsigned)fds[0],read,4096,ROUNDS*4096)==4096);
    for(unsigned i=0;i<4096;i++) CHECK(((char *)read)[i]=='L');
    /* Restore the first oracle block and remove the test extension. */
    memset(buf,1,4096);CHECK(nvfuse_pwrite_direct(h,(unsigned)fds[0],buf,4096,0)==4096);
    CHECK(!nvfuse_ftruncate(h,alias,ROUNDS*4096));CHECK(!nvfuse_queue_destroy(q));CHECK(!nvfuse_closefile(h,alias));
    for(unsigned i=0;i<THREADS;i++) CHECK(!nvfuse_closefile(h,fds[i]));free(buf);free(read);
    CHECK(!nvfuse_file_close(h));h=nvfuse_file_open(argv[1],0,0);CHECK(h);
    for(unsigned i=0;i<THREADS;i++) {
        char path[32];snprintf(path,sizeof(path),"/q%u",i);int fd=nvfuse_openfile_path(h,path,O_RDONLY,0);CHECK(fd>=0);
        char bytes[4096];for(unsigned k=0;k<ROUNDS;k++) {
            CHECK(nvfuse_readfile(h,fd,bytes,sizeof(bytes),(int64_t)k*4096)==sizeof(bytes));
            for(unsigned n=0;n<sizeof(bytes);n++) CHECK((unsigned char)bytes[n]==1+i*31+k%23);
        }
        CHECK(!nvfuse_closefile(h,fd));
    }
    CHECK(!nvfuse_file_close(h));puts("PASS queue: 3072 multi-thread jobs, three commit modes, direct/cache coherence, group abort, shutdown and remount");
}
