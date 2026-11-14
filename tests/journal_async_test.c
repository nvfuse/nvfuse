/* SPDX-License-Identifier: GPL-2.0-only */

/* A deterministic gate proves the commit response precedes checkpoint, while
 * all protected images remain owned until retirement or recovery. */
#include "nvfuse_journal.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct disk { unsigned char *data; pthread_mutex_t lock;pthread_cond_t ready;int gate,entered,error; };
static unsigned slot(uint64_t offset) {
    uint64_t b=offset/NVJ_BLOCK;if(b<16)return (unsigned)b;
    assert(b>=NVJ_START && b<NVJ_GROUP_BLOCKS);return 16+(unsigned)(b-NVJ_START);
}
static int io(void *arg,uint64_t offset,size_t n,void *buf,int write) {
    struct disk *d=arg;assert(n==NVJ_BLOCK && !(offset%NVJ_BLOCK));
    if(write && !offset) {
        pthread_mutex_lock(&d->lock);
        d->entered=1;pthread_cond_broadcast(&d->ready);
        while(d->gate) pthread_cond_wait(&d->ready,&d->lock);
        int error=d->error;d->error=0;pthread_mutex_unlock(&d->lock);
        if(error) return -EIO;
    }
    unsigned char *p=d->data+(size_t)slot(offset)*NVJ_BLOCK;
    if(write) memcpy(p,buf,n);else memcpy(buf,p,n);return 0;
}
static int flush(void *arg){(void)arg;return 0;}
int main(void) {
    struct disk d={.lock=PTHREAD_MUTEX_INITIALIZER,.ready=PTHREAD_COND_INITIALIZER};
    d.data=calloc(16+NVJ_BLOCKS,NVJ_BLOCK);assert(d.data);
    struct nvj_ops ops={io,flush,&d};uint64_t bytes=(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK;
    assert(!nvj_format(&ops,bytes,0));struct nvj *j=NULL;assert(!nvj_open(&j,&ops,bytes,0));
    assert(!nvj_set_async_checkpoint(j,1));
    for(unsigned iteration=0;iteration<32;iteration++) {
        unsigned char b[NVJ_BLOCK];memset(b,(int)(iteration+1),sizeof(b));
        assert(!nvj_begin(j,1,0));assert(!nvj_transfer(j,0,sizeof(b),b,1));
        pthread_mutex_lock(&d.lock);d.gate=1;d.entered=0;pthread_mutex_unlock(&d.lock);
        assert(!nvj_commit(j));
        pthread_mutex_lock(&d.lock);
        while(!d.entered) pthread_cond_wait(&d.ready,&d.lock);
        /* This line is reachable only if commit returned without waiting for
         * the gated home write. Do not call joining APIs until release. */
        assert(d.data[0]==(unsigned char)iteration);
        d.gate=0;pthread_cond_broadcast(&d.ready);pthread_mutex_unlock(&d.lock);
        assert(!nvj_error(j));assert(nvj_stats(j)->commits==iteration+1);
        assert(!memcmp(d.data,b,NVJ_BLOCK));
    }
    unsigned char b[NVJ_BLOCK];memset(b,0xe3,sizeof(b));
    assert(!nvj_begin(j,1,0));assert(!nvj_transfer(j,0,sizeof(b),b,1));
    d.error=1;assert(!nvj_commit(j));assert(nvj_error(j)==-EIO && nvj_recovery_required(j));
    assert(nvj_begin(j,1,0)==-EIO);nvj_close(j);
    assert(!nvj_open(&j,&ops,bytes,0));assert(!memcmp(d.data,b,NVJ_BLOCK));
    assert(nvj_stats(j)->replays==1);assert(!nvj_set_async_checkpoint(j,1));
    assert(!nvj_begin(j,1,0));memset(b,0x9a,sizeof(b));assert(!nvj_transfer(j,0,sizeof(b),b,1));
    assert(!nvj_commit(j));nvj_close(j);assert(!memcmp(d.data,b,NVJ_BLOCK));
    pthread_cond_destroy(&d.ready);pthread_mutex_destroy(&d.lock);free(d.data);
    puts("PASS asynchronous checkpoint: gated durable responses, 32 slot reuses, post-commit EIO/replay and close drain");
}
