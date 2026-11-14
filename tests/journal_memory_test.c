/* SPDX-License-Identifier: GPL-2.0-only */

/* Fail each journal allocation, including reopen/replay and overlay growth. */
#include "nvfuse_journal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
static unsigned fail_at,allocations,hits;
static int allocation_fails(void) {allocations++;if(fail_at && allocations==fail_at){hits++;return 1;}return 0;}
static void *memory_malloc(size_t n){return allocation_fails()?NULL:malloc(n);}
static void *memory_calloc(size_t n,size_t s){return allocation_fails()?NULL:calloc(n,s);}
static void *memory_realloc(void *p,size_t n){return allocation_fails()?NULL:realloc(p,n);}
static int fail_thread;
static int memory_pthread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*run)(void *),void *arg) {
    if(fail_thread) { fail_thread=0;return EAGAIN; }
    return pthread_create(thread,attr,run,arg);
}
#define pthread_create memory_pthread_create
#define malloc memory_malloc
#define calloc memory_calloc
#define realloc memory_realloc
#include "../src/nvfuse_journal.c"
#undef pthread_create
#undef malloc
#undef calloc
#undef realloc
#define SLOTS (NVJ_CAPACITY+2+NVJ_BLOCKS)
struct disk {unsigned char *bytes;int cut;};
static unsigned slot(uint64_t offset) {
    uint64_t b=offset/NVJ_BLOCK;
    if(b<NVJ_CAPACITY+2)return (unsigned)b;
    assert(b>=NVJ_START && b<NVJ_START+NVJ_BLOCKS);return NVJ_CAPACITY+2+(unsigned)(b-NVJ_START);
}
static int disk_io(void *arg,uint64_t offset,size_t n,void *buf,int write) {
    struct disk *d=arg;assert(offset%NVJ_BLOCK==0 && n==NVJ_BLOCK);unsigned s=slot(offset);
    if(write){memcpy(d->bytes+(size_t)s*NVJ_BLOCK,buf,n);if(d->cut && offset==0){d->cut=0;return -EIO;}}
    else memcpy(buf,d->bytes+(size_t)s*NVJ_BLOCK,n);return 0;
}
static int disk_flush(void *arg){(void)arg;return 0;}
static void arm(unsigned n){fail_at=n;allocations=hits=0;}
static void disarm(void){fail_at=0;}
static void verify(struct disk *d,unsigned n,unsigned char value){for(unsigned i=0;i<n;i++)for(unsigned k=0;k<NVJ_BLOCK;k++)assert(d->bytes[(size_t)i*NVJ_BLOCK+k]==value);}
static void stage(struct nvj *j,unsigned n){unsigned char b[NVJ_BLOCK];memset(b,0x5a,sizeof(b));for(unsigned i=0;i<n;i++)assert(nvj_transfer(j,(uint64_t)i*NVJ_BLOCK,NVJ_BLOCK,b,1)==0);}
int main(void) {
    struct disk d={.bytes=calloc(SLOTS,NVJ_BLOCK)};assert(d.bytes);struct nvj_ops ops={disk_io,disk_flush,&d};
    uint64_t bytes=(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK;assert(nvj_format(&ops,bytes,0)==0);
    assert(crc("123456789",9)==UINT32_C(0xe3069283));
    unsigned char *anchor=d.bytes+(size_t)slot((uint64_t)NVJ_START*NVJ_BLOCK)*NVJ_BLOCK;
    put32(anchor+8,1);seal(anchor);
    struct nvj *legacy=NULL; assert(!nvj_open(&legacy,&ops,bytes,0));
    assert(nvj_version(legacy)==1);nvj_close(legacy);
    uint64_t probed=0;assert(!nvj_probe(&ops,bytes,&probed,0) && probed==bytes);
    put32(anchor+8,99);seal(anchor);
    assert(nvj_probe(&ops,bytes,&probed,0)==-EINVAL);
    put32(anchor+8,2);seal(anchor);
    unsigned cases=0;struct nvj *j=NULL;
    for(unsigned n=1;;n++) {
        arm(n);int rc=nvj_open(&j,&ops,bytes,0);unsigned hit=hits;disarm();
        if(!hit){assert(rc==0);nvj_close(j);break;}
        assert(rc==-ENOMEM && !j);verify(&d,8,0);cases++;
    }
    assert(nvj_open(&j,&ops,bytes,0)==0);
    arm(1);assert(nvj_prepare_metadata(j,NVJ_CAPACITY)==-ENOMEM && hits==1);
    disarm();assert(!nvj_error(j) && !nvj_prepare_metadata(j,NVJ_CAPACITY));
    nvj_close(j);cases++;
    assert(nvj_open(&j,&ops,bytes,0)==0);
    for(unsigned i=0;i<32;i++) nvj_mark_metadata(j,i);
    arm(1); nvj_mark_metadata(j,32);
    assert(hits==1 && nvj_error(j)==-ENOMEM); disarm();
    nvj_discard(j); assert(nvj_begin(j,0,0)==-ENOMEM);
    unsigned char blocked[NVJ_BLOCK]; memset(blocked,0x3b,sizeof(blocked));
    assert(nvj_transfer(j,32ULL*NVJ_BLOCK,sizeof(blocked),blocked,1)==-ENOMEM);
    nvj_close(j); cases++;
    /* Grow the hash independently of image storage; discard must remove all
     * colliding keys, including block zero, without breaking probe chains. */
    assert(nvj_open(&j,&ops,bytes,0)==0 && nvj_begin(j,1,0)==0);
    assert(reserve(&j->index,8)==0); stage(j,4); arm(1);
    assert(nvj_transfer(j,4ULL*NVJ_BLOCK,sizeof(blocked),blocked,1)==-ENOMEM);
    assert(hits==1 && nvj_error(j)==-ENOMEM); disarm(); nvj_discard(j);
    assert(nvj_begin(j,1,0)==0); stage(j,8); nvj_discard(j); nvj_close(j); cases++;
    for(unsigned n=1;n<=2;n++) {
        assert(nvj_open(&j,&ops,bytes,0)==0 && nvj_begin(j,1,0)==0);stage(j,NVJ_CAPACITY);
        unsigned char b[NVJ_BLOCK];memset(b,0x5a,sizeof(b));arm(n);
        assert(nvj_transfer(j,(uint64_t)NVJ_CAPACITY*NVJ_BLOCK,NVJ_BLOCK,b,1)==-ENOMEM);assert(hits==1 && nvj_error(j)==-ENOMEM);disarm();
        assert(nvj_commit(j)==-ENOMEM);nvj_discard(j);verify(&d,NVJ_CAPACITY+1,0);nvj_close(j);cases++;
    }
    for(unsigned n=1;n<=3;n++) {
        assert(!nvj_open(&j,&ops,bytes,0) && !nvj_begin(j,1,0));stage(j,NVJ_CAPACITY-8);
        arm(n);assert(nvj_prepare_metadata(j,NVJ_CAPACITY)==-ENOMEM && hits==1);disarm();
        assert(!nvj_error(j));nvj_discard(j);assert(!nvj_begin(j,1,0));stage(j,8);nvj_discard(j);nvj_close(j);cases++;
    }
    assert(nvj_open(&j,&ops,bytes,0)==0 && nvj_begin(j,1,0)==0);stage(j,8);arm(1);
    assert(nvj_commit(j)==-ENOMEM && hits==1 && !nvj_recovery_required(j));disarm();
    nvj_discard(j);verify(&d,8,0);assert(nvj_begin(j,1,0)==0);stage(j,8);d.cut=1;assert(nvj_commit(j)==-EIO);nvj_close(j);cases++;
    unsigned char *committed=malloc((size_t)SLOTS*NVJ_BLOCK);assert(committed);memcpy(committed,d.bytes,(size_t)SLOTS*NVJ_BLOCK);
    for(unsigned n=1;;n++) {
        memcpy(d.bytes,committed,(size_t)SLOTS*NVJ_BLOCK);arm(n);int rc=nvj_open(&j,&ops,bytes,0);unsigned hit=hits;disarm();
        if(!hit){assert(rc==0);verify(&d,8,0x5a);nvj_close(j);break;}
        assert(rc==-ENOMEM && !j);assert(nvj_open(&j,&ops,bytes,0)==0);verify(&d,8,0x5a);nvj_close(j);
        assert(nvj_open(&j,&ops,bytes,0)==0 && nvj_begin(j,1,1)==0);
        unsigned char b[NVJ_BLOCK];memset(b,0x61,sizeof(b));assert(nvj_transfer(j,0,sizeof(b),b,1)==0 && nvj_commit(j)==0);nvj_close(j);cases++;
    }
    assert(!nvj_open(&j,&ops,bytes,0));assert(!nvj_set_async_checkpoint(j,1));
    assert(!nvj_begin(j,1,0));unsigned char fallback[NVJ_BLOCK];memset(fallback,0x77,sizeof(fallback));
    assert(!nvj_transfer(j,0,sizeof(fallback),fallback,1));fail_thread=1;
    assert(!nvj_commit(j) && !fail_thread && !nvj_recovery_required(j));
    assert(!memcmp(d.bytes,fallback,NVJ_BLOCK));nvj_close(j);
    printf("PASS journal ENOMEM: %u allocation failures; open, growth, commit, replay and reuse\n",cases);
    free(committed);free(d.bytes);return 0;
}
