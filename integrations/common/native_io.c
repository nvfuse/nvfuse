/* SPDX-License-Identifier: GPL-2.0-only */
/* One volume owner; batch positional I/O through bounded, reusable DMA slots.
 * Accepted requests are always drained before their buffers can be reused. */
#include "spdk/stdinc.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_spdk_runtime.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <libaio.h>
#define DEPTH 32
#define PIECE (256 * 1024)
struct piece { struct nvfio_request *req; void *dma, *destination; size_t bytes, capacity; int write, busy; };
struct channel {
    struct piece slots[DEPTH];
    struct nvfio_client *client;
    pthread_mutex_t mutex;
    uint64_t calls, requests, max_inflight;
};
static struct channel primary;
static int running;
static int wait_yield;
static int wait_block;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static void destroy_slots(struct channel *channel) {
    for(unsigned i=0;i<DEPTH;i++) {
        if(channel->slots[i].req)nvfio_request_free(channel->slots[i].req);
        if(channel->slots[i].dma)nvfio_dma_free(channel->slots[i].dma);
        memset(&channel->slots[i],0,sizeof(channel->slots[i]));
    }
}
static int initialize_channel(struct channel *channel) {
    unsigned queues=4;const char *setting=getenv("NVAI_CONTEXT_QUEUES");
    if(setting){char *end=NULL;errno=0;unsigned long value=strtoul(setting,&end,10);
        if(!*setting || *setting<'0' || *setting>'9' || *end || errno || value<1 || value>4)return -EINVAL;
        queues=(unsigned)value;}
    pthread_mutex_init(&channel->mutex,NULL);
    channel->client=nvfio_client_alloc(queues);
    if(channel==&primary)fprintf(stderr,"NVAI_CONTEXT_CONFIG queues=%u piece=%u depth=%u wait=%s\n",queues,PIECE,DEPTH,wait_block?"block":wait_yield?"yield":"pause");
    int rc=channel->client?0:-ENOMEM;
    for(unsigned i=0;i<DEPTH && !rc;i++) {
        channel->slots[i].req=nvfio_request_alloc(&channel->slots[i]);
        if(!channel->slots[i].req)rc=-ENOMEM;
    }
    if(rc){destroy_slots(channel);if(channel->client)nvfio_client_free(channel->client);pthread_mutex_destroy(&channel->mutex);}
    return rc;
}
void *nvai_context_create(void) {
    if(!running)return NULL;
    struct channel *channel=calloc(1,sizeof(*channel));if(!channel)return NULL;
    if(initialize_channel(channel)){free(channel);return NULL;}
    return channel;
}
int nvai_context_free(void *context) {
    struct channel *channel=context;if(!channel)return 0;
    int rc=nvfio_client_free(channel->client);if(rc)return rc;
    destroy_slots(channel);pthread_mutex_destroy(&channel->mutex);free(channel);return 0;
}
int nvai_start(int format) {
    pthread_mutex_lock(&mutex);
    if(running){pthread_mutex_unlock(&mutex);return -EBUSY;}
    const char *policy=getenv("NVAI_WAIT_POLICY");
    if(policy && strcmp(policy,"pause") && strcmp(policy,"yield") && strcmp(policy,"block")){
        pthread_mutex_unlock(&mutex);return -EINVAL;
    }
    wait_yield=policy && !strcmp(policy,"yield");
    wait_block=!policy || !strcmp(policy,"block");
    setenv("NVFUSE_SPDK_EMBEDDED","1",1);
    setenv("NVFUSE_IPC_SOCKET_ANY","1",1);
    int rc=nvfio_start(getenv("NVFUSE_SPDK_JSON"),getenv("NVFUSE_SPDK_BDF"),format);
    if(!rc) {
        rc=initialize_channel(&primary);
        if(rc)nvfio_stop();else running=1;
    }
    pthread_mutex_unlock(&mutex);return rc;
}
struct arena {const char *path;uint64_t bytes;};
static int arena_owner(struct nvfuse_handle *h,void *opaque) {
    struct arena *a=opaque;struct stat st;
    int rc=nvfuse_getattr(h,a->path,&st);
    if(rc && rc!=-ENOENT)return rc;
    int fd=nvfuse_openfile_path(h,a->path,O_CREAT|O_RDWR|O_DIRECT,0600);
    if(fd<0)return fd;
    if(rc==-ENOENT || (uint64_t)st.st_size<a->bytes){
        /* The SPDK owner callback prevents periodic commits while running.
         * Commit bounded allocation chunks before serving any requests. */
        const uint64_t chunk=UINT64_C(1)<<30;
        rc=0;
        for(uint64_t offset=0;offset<a->bytes && !rc;) {
            uint64_t bytes=a->bytes-offset;
            if(bytes>chunk)bytes=chunk;
            rc=nvfuse_fallocate(h,a->path,offset,bytes);
            if(!rc)rc=nvfuse_fsync(h,fd);
            offset+=bytes;
        }
        if(!rc)rc=nvfuse_ftruncate(h,fd,a->bytes);
        if(!rc)rc=nvfuse_fsync(h,fd);
        if(rc){nvfuse_closefile(h,fd);return rc;}
    }
    return fd;
}
int nvai_open_arena(const char *path,uint64_t bytes){
    if(!running || !path || !*path || !bytes || bytes%4096 || bytes>INT64_MAX)return -EINVAL;
    struct arena a={path,bytes};return nvfio_execute(arena_owner,&a);
}
int nvai_open_read(const char *path) {return running?nvfio_open(path,0):-EINVAL;}
/* Arrays remain owned by the caller through return. Partial failures drain all
 * accepted DMA; no failed write batch is published by the Python index. */
int nvai_transfer_batch_context(void *context,int fd,void **buffers,const size_t *lengths,
        const uint64_t *offsets,const int *writes,unsigned count) {
    struct channel *channel=context;
    if(!running || !channel || fd<0 || !count || count>1024)return -EINVAL;
    for(unsigned i=0;i<count;i++)
        if(!buffers[i] || !lengths[i] || lengths[i]%4096 || offsets[i]%4096 ||
           offsets[i]>INT64_MAX || lengths[i]>(uint64_t)INT64_MAX-offsets[i] ||
           (writes[i]!=0 && writes[i]!=1))return -EINVAL;
    pthread_mutex_lock(&channel->mutex);
    unsigned next=0,inflight=0;size_t done=0;int error=0;
    channel->calls++;
    while(next<count || inflight) {
        for(unsigned i=0;i<DEPTH && !error && next<count;i++) {
            struct piece *p=&channel->slots[i];if(p->busy)continue;
            size_t bytes=lengths[next]-done;if(bytes>PIECE)bytes=PIECE;
            /* Graph readers usually need 4 KiB, not 32 eager 256 KiB slots
             * per thread. Grow only an idle slot, retaining it for reuse.
             * An allocation failure still drains every accepted request. */
            if(p->capacity<bytes) {
                void *dma=nvfio_dma_alloc(bytes);
                if(!dma){error=-ENOMEM;break;}
                if(p->dma)nvfio_dma_free(p->dma);
                p->dma=dma;p->capacity=bytes;
            }
            p->destination=(char *)buffers[next]+done;p->bytes=bytes;p->write=writes[next];
            if(p->write)memcpy(p->dma,p->destination,bytes);
            int rc=nvfio_submit(channel->client,p->req,fd,p->dma,bytes,offsets[next]+done,p->write);
            if(rc==-EAGAIN)break;
            if(rc){error=rc;break;}
            p->busy=1;inflight++;channel->requests++;
            if(inflight>channel->max_inflight)channel->max_inflight=inflight;
            done+=bytes;if(done==lengths[next]){done=0;next++;}
        }
        if(error && !inflight)break;
        struct nvfio_request *completed[DEPTH];
        int n=nvfio_poll(channel->client,DEPTH,completed);
        if(!n && inflight && wait_block)
            n=nvfio_wait(channel->client,DEPTH,completed,1000);
        /* A fatal runtime that cannot retire accepted DMA cannot be recovered
         * safely by returning to a caller which will reuse those buffers. */
        if(n<0 && inflight){fprintf(stderr,"NVAI_FATAL_POLL %d\n",n);abort();}
        if(n<0){error=n;break;}
        for(int i=0;i<n;i++) {
            struct piece *p=nvfio_request_user(completed[i]);
            int rc=nvfio_request_error(completed[i]);
            if(rc && !error)error=-rc;
            if(!rc && !p->write)memcpy(p->destination,p->dma,p->bytes);
            p->busy=0;inflight--;
        }
        if(!n) {
            if(wait_yield)sched_yield();
            else {
#if defined(__x86_64__) || defined(__i386__)
                __asm__ __volatile__("pause");
#elif defined(__aarch64__)
                __asm__ __volatile__("yield");
#else
                atomic_signal_fence(memory_order_seq_cst);
#endif
            }
        }
    }
    pthread_mutex_unlock(&channel->mutex);return error;
}
int nvai_transfer_batch(int fd,void **buffers,const size_t *lengths,
        const uint64_t *offsets,const int *writes,unsigned count) {
    return nvai_transfer_batch_context(&primary,fd,buffers,lengths,offsets,writes,count);
}
int nvai_transfer(int fd,void *buffer,size_t length,uint64_t offset,int write) {
    return nvai_transfer_batch(fd,&buffer,&length,&offset,&write,1);
}
int nvai_sync(int fd){return nvfio_sync(primary.client,fd);}
int nvai_close(int fd){return nvfio_close(fd);}
uint64_t nvai_max_inflight(void){return primary.max_inflight;}
int nvai_stop(void){
    pthread_mutex_lock(&mutex);
    if(!running){pthread_mutex_unlock(&mutex);return 0;}
    fprintf(stderr,"NVAI_BATCH calls=%lu requests=%lu max_inflight=%lu\n",primary.calls,primary.requests,primary.max_inflight);
    int rc=nvfio_client_free(primary.client);
    if(rc){pthread_mutex_unlock(&mutex);return rc;}
    destroy_slots(&primary);pthread_mutex_destroy(&primary.mutex);memset(&primary,0,sizeof(primary));
    int stopped=nvfio_stop();running=0;
    pthread_mutex_unlock(&mutex);return rc?rc:stopped;
}
/* Matched XFS batching uses Linux libaio and reusable aligned host buffers.
 * This does not initialize SPDK or reserve hugepages. */
static io_context_t posix_ctx;
static void *posix_dma[DEPTH];
int nvai_posix_start(void) {
    int rc=io_setup(DEPTH,&posix_ctx);if(rc)return rc;
    for(unsigned i=0;i<DEPTH;i++) {
        rc=posix_memalign(&posix_dma[i],4096,PIECE);
        if(rc){for(unsigned j=0;j<i;j++)free(posix_dma[j]);io_destroy(posix_ctx);posix_ctx=0;return -rc;}
    }
    return 0;
}
int nvai_posix_batch(int fd,void **buffers,const size_t *lengths,
        const uint64_t *offsets,const int *writes,unsigned count) {
    if(!posix_ctx || !count || count>1024)return -EINVAL;
    for(unsigned i=0;i<count;i++)if(!buffers[i]||!lengths[i]||lengths[i]%4096||offsets[i]%4096 ||
      offsets[i]>INT64_MAX || lengths[i]>(uint64_t)INT64_MAX-offsets[i] || (writes[i]!=0&&writes[i]!=1))return -EINVAL;
    struct iocb cbs[DEPTH],*ptrs[DEPTH];struct io_event events[DEPTH];
    void *dest[DEPTH];size_t bytes[DEPTH];int wr[DEPTH];unsigned next=0;size_t done=0;
    while(next<count) {
        unsigned n=0;
        while(n<DEPTH && next<count) {
            bytes[n]=lengths[next]-done;if(bytes[n]>PIECE)bytes[n]=PIECE;
            dest[n]=(char *)buffers[next]+done;wr[n]=writes[next];
            if(wr[n]){memcpy(posix_dma[n],dest[n],bytes[n]);io_prep_pwrite(&cbs[n],fd,posix_dma[n],bytes[n],offsets[next]+done);}
            else io_prep_pread(&cbs[n],fd,posix_dma[n],bytes[n],offsets[next]+done);
            cbs[n].data=(void *)(uintptr_t)n;ptrs[n]=&cbs[n];
            done+=bytes[n];if(done==lengths[next]){done=0;next++;}n++;
        }
        int accepted=0,error=0;
        while((unsigned)accepted<n) {
            int rc=io_submit(posix_ctx,n-accepted,ptrs+accepted);
            if(rc==-EINTR)continue;
            if(rc<=0){error=rc?rc:-EIO;break;}
            accepted+=rc;
        }
        int retired=0;
        while(retired<accepted) {
            int rc=io_getevents(posix_ctx,1,accepted-retired,events,NULL);
            if(rc==-EINTR)continue;
            if(rc<0)abort();
            for(int k=0;k<rc;k++) {
                unsigned i=(uintptr_t)events[k].data;
                if(events[k].res!=(long)bytes[i] || events[k].res2){if(!error)error=-EIO;}
                else if(!wr[i])memcpy(dest[i],posix_dma[i],bytes[i]);
            }
            retired+=rc;
        }
        if(error)return error;
    }
    return 0;
}
int nvai_posix_stop(void) {
    int rc=io_destroy(posix_ctx);posix_ctx=0;
    for(unsigned i=0;i<DEPTH;i++){free(posix_dma[i]);posix_dma[i]=NULL;}
    return rc;
}
