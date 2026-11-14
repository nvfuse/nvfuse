/* SPDX-License-Identifier: GPL-2.0-only */

#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_tx_internal.h"
#include "nvfuse_direct.h"
#include "nvfuse_queue.h"
#include <limits.h>
#define QUEUE_MAX 128u
struct nvfuse_future {
    pthread_mutex_t lock;pthread_cond_t ready;int done,result;
    enum nvfuse_queue_operation operation;unsigned fd,bytes;void *buffer;int64_t offset;
};
struct nvfuse_queue {
    struct nvfuse_handle *h;pthread_t owner;pthread_mutex_t lock;
    struct nvfuse_future **ring;unsigned depth,head,count,mode;int stopped,polling;
    struct nvfuse_queue_stats stats;
};
static void finish(struct nvfuse_future *f,int result) {
    pthread_mutex_lock(&f->lock);f->result=result;f->done=1;
    pthread_cond_signal(&f->ready);pthread_mutex_unlock(&f->lock);
}
struct nvfuse_queue *nvfuse_queue_create(struct nvfuse_handle *h,unsigned depth,unsigned mode) {
    int state=nvfuse_tx_status(h);
    if(state<0 || state!=NVFUSE_TX_IDLE || !depth || depth>QUEUE_MAX || mode>2) {
        errno=state<0?-state:state!=NVFUSE_TX_IDLE?EBUSY:EINVAL;return NULL;
    }
    struct nvfuse_queue *q=calloc(1,sizeof(*q));if(!q) return NULL;
    q->ring=calloc(depth,sizeof(*q->ring));if(!q->ring) { free(q);return NULL; }
    int rc=pthread_mutex_init(&q->lock,NULL);if(rc) { free(q->ring);free(q);errno=rc;return NULL; }
    h->nvh_target->tx_queues++;
    q->h=h;q->owner=pthread_self();q->depth=depth;q->mode=mode;return q;
}
struct nvfuse_future *nvfuse_queue_submit(struct nvfuse_queue *q,enum nvfuse_queue_operation operation,
                                        unsigned fd,void *buf,unsigned bytes,int64_t offset) {
    if(!q || operation<NVFUSE_QUEUE_READ || operation>NVFUSE_QUEUE_DIRECT_WRITE || bytes>INT_MAX ||
       (bytes && !buf) || offset<0) { errno=EINVAL;return NULL; }
    struct nvfuse_future *f=calloc(1,sizeof(*f));if(!f) return NULL;
    int rc=pthread_mutex_init(&f->lock,NULL);if(rc) { free(f);errno=rc;return NULL; }
    rc=pthread_cond_init(&f->ready,NULL);if(rc) { pthread_mutex_destroy(&f->lock);free(f);errno=rc;return NULL; }
    f->operation=operation;f->fd=fd;f->buffer=buf;f->bytes=bytes;f->offset=offset;
    pthread_mutex_lock(&q->lock);
    if(q->stopped || q->count==q->depth) {
        int error=q->stopped?ECANCELED:EAGAIN;pthread_mutex_unlock(&q->lock);
        pthread_cond_destroy(&f->ready);pthread_mutex_destroy(&f->lock);free(f);errno=error;return NULL;
    }
    q->ring[(q->head+q->count)%q->depth]=f;q->count++;q->stats.submitted++;
    pthread_mutex_unlock(&q->lock);return f;
}
int nvfuse_future_wait(struct nvfuse_future *f) {
    if(!f) return -EINVAL;
    pthread_mutex_lock(&f->lock);while(!f->done) pthread_cond_wait(&f->ready,&f->lock);
    int result=f->result;pthread_mutex_unlock(&f->lock);
    pthread_cond_destroy(&f->ready);pthread_mutex_destroy(&f->lock);free(f);return result;
}
int nvfuse_queue_poll(struct nvfuse_queue *q,unsigned maximum) {
    if(!q || !maximum) return -EINVAL;
    if(!pthread_equal(q->owner,pthread_self())) return -EPERM;
    struct nvfuse_future *batch[QUEUE_MAX];int results[QUEUE_MAX];
    pthread_mutex_lock(&q->lock);
    if(q->polling) { pthread_mutex_unlock(&q->lock);return -EBUSY; }
    unsigned n=q->count;if(n>maximum) n=maximum;
    for(unsigned i=0;i<n;i++) { batch[i]=q->ring[q->head];q->head=(q->head+1)%q->depth;q->count--; }
    q->polling=1;pthread_mutex_unlock(&q->lock);
    int rc=0;
    if(n && q->mode) rc=nvfuse_tx_begin_group(q->h,(enum nvfuse_tx_mode)q->mode);
    int started=n && q->mode && !rc;
    for(unsigned i=0;i<n;i++) {
        struct nvfuse_future *f=batch[i];int result=rc;
        if(!rc) switch(f->operation) {
            case NVFUSE_QUEUE_READ:result=nvfuse_readfile(q->h,f->fd,f->buffer,(s32)f->bytes,f->offset);break;
            case NVFUSE_QUEUE_WRITE:result=nvfuse_writefile(q->h,f->fd,f->buffer,f->bytes,f->offset);break;
            case NVFUSE_QUEUE_DIRECT_READ:result=nvfuse_pread_direct(q->h,f->fd,f->buffer,f->bytes,f->offset);break;
            case NVFUSE_QUEUE_DIRECT_WRITE:result=nvfuse_pwrite_direct(q->h,f->fd,f->buffer,f->bytes,f->offset);break;
        }
        results[i]=result;if(q->mode && result<0 && !rc) rc=result;
    }
    if(started) {
        if(!rc) rc=nvfuse_tx_commit(q->h);
        if(rc && nvfuse_tx_status(q->h)!=NVFUSE_TX_RECOVERY_REQUIRED) {
            int undo=nvfuse_tx_abort(q->h);if(undo && undo!=-EINVAL) rc=undo;
        }
    }
    for(unsigned i=0;i<n;i++) finish(batch[i],q->mode && rc?rc:results[i]);
    pthread_mutex_lock(&q->lock);q->stats.completed+=n;q->polling=0;pthread_mutex_unlock(&q->lock);
    return (int)n;
}
void nvfuse_queue_stop(struct nvfuse_queue *q) {
    if(!q) return;
    pthread_mutex_lock(&q->lock);q->stopped=1;
    while(q->count) {
        struct nvfuse_future *f=q->ring[q->head];q->head=(q->head+1)%q->depth;q->count--;
        q->stats.cancelled++;finish(f,-ECANCELED);
    }
    pthread_mutex_unlock(&q->lock);
}
int nvfuse_queue_destroy(struct nvfuse_queue *q) {
    if(!q) return -EINVAL;
    if(!pthread_equal(q->owner,pthread_self())) return -EPERM;
    nvfuse_queue_stop(q);q->h->nvh_target->tx_queues--;pthread_mutex_destroy(&q->lock);free(q->ring);free(q);return 0;
}
void nvfuse_queue_get_stats(struct nvfuse_queue *q,struct nvfuse_queue_stats *out) {
    if(q && out) { pthread_mutex_lock(&q->lock);*out=q->stats;pthread_mutex_unlock(&q->lock); }
}
