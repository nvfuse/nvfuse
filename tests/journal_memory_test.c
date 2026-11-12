/* Fail each journal allocation, including reopen/replay and overlay growth. */
#include "nvfuse_journal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned fail_at,allocations,hits;
static int allocation_fails(void) {allocations++;if(fail_at && allocations==fail_at){hits++;return 1;}return 0;}
static void *memory_malloc(size_t n){return allocation_fails()?NULL:malloc(n);}
static void *memory_calloc(size_t n,size_t s){return allocation_fails()?NULL:calloc(n,s);}
static void *memory_realloc(void *p,size_t n){return allocation_fails()?NULL:realloc(p,n);}
#define malloc memory_malloc
#define calloc memory_calloc
#define realloc memory_realloc
#include "../nvfuse_journal.c"
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
    unsigned cases=0;struct nvj *j=NULL;
    for(unsigned n=1;;n++) {
        arm(n);int rc=nvj_open(&j,&ops,bytes,0);unsigned hit=hits;disarm();
        if(!hit){assert(rc==0);nvj_close(j);break;}
        assert(rc==-ENOMEM && !j);verify(&d,8,0);cases++;
    }
    for(unsigned n=1;n<=2;n++) {
        assert(nvj_open(&j,&ops,bytes,0)==0 && nvj_begin(j,1,0)==0);stage(j,NVJ_CAPACITY);
        unsigned char b[NVJ_BLOCK];memset(b,0x5a,sizeof(b));arm(n);
        assert(nvj_transfer(j,(uint64_t)NVJ_CAPACITY*NVJ_BLOCK,NVJ_BLOCK,b,1)==-ENOMEM);assert(hits==1 && nvj_error(j)==-ENOMEM);disarm();
        assert(nvj_commit(j)==-ENOMEM);nvj_discard(j);verify(&d,NVJ_CAPACITY+1,0);nvj_close(j);cases++;
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
    printf("PASS journal ENOMEM: %u allocation failures; open, growth, commit, replay and reuse\n",cases);
    free(committed);free(d.bytes);return 0;
}
