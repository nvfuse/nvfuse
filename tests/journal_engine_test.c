/* Independent volatile/durable storage oracle: an unflushed write can be lost,
 * persist entirely, or persist one sector. Crashes discard the volatile view. */
#include "nvfuse_journal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SLOTS (NVJ_BLOCKS+16)
struct disk { unsigned char *v,*d; unsigned char dirty[SLOTS]; int step,cut,partial,armed; };
static unsigned slot(uint64_t offset) {
    unsigned block=(unsigned)(offset/NVJ_BLOCK);
    if(block<16) return block;
    assert(block>=NVJ_START && block<NVJ_START+NVJ_BLOCKS);
    return 16+block-NVJ_START;
}
static int boundary(struct disk *d) { return d->armed && ++d->step==d->cut ? -EIO : 0; }
static int io(void *ctx,uint64_t off,size_t n,void *buf,int write_op) {
    struct disk *d=ctx; unsigned s=slot(off),within=(unsigned)(off%NVJ_BLOCK);
    assert(within+n<=NVJ_BLOCK);
    if(write_op) {
        memcpy(d->v+(size_t)s*NVJ_BLOCK+within,buf,n); d->dirty[s]=1;
        if(d->partial) {
            size_t persisted=d->partial==1?(n<512?n:512):n;
            memcpy(d->d+(size_t)s*NVJ_BLOCK+within,buf,persisted);
        }
        return boundary(d);
    } else memcpy(buf,d->v+(size_t)s*NVJ_BLOCK+within,n);
    return 0;
}
static int flush(void *ctx) {
    struct disk *d=ctx;
    /* Reverse order deliberately; flush completion must still persist all. */
    for(unsigned i=SLOTS;i-->0;) if(d->dirty[i]) {
        memcpy(d->d+(size_t)i*NVJ_BLOCK,d->v+(size_t)i*NVJ_BLOCK,NVJ_BLOCK); d->dirty[i]=0;
    }
    return boundary(d);
}
static void powerloss(struct disk *d) {
    d->armed=0; memcpy(d->v,d->d,(size_t)SLOTS*NVJ_BLOCK); memset(d->dirty,0,sizeof(d->dirty));
}
static int check_pair(struct disk *d) {
    unsigned char *a=d->v+8*NVJ_BLOCK,*b=d->v+9*NVJ_BLOCK;
    assert(a[0]==b[0]); assert(a[0]=='A' || a[0]=='B');
    for(unsigned i=0;i<NVJ_BLOCK;i++) assert(a[i]==a[0] && b[i]==b[0]);
    return a[0];
}
int main(void) {
    struct disk *d=calloc(1,sizeof(*d)); assert(d);
    d->v=calloc(SLOTS,NVJ_BLOCK); d->d=calloc(SLOTS,NVJ_BLOCK); assert(d->v && d->d);
    struct nvj_ops ops={io,flush,d}; uint64_t size=(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK;
    memset(d->v+8*NVJ_BLOCK,'A',2*NVJ_BLOCK); memcpy(d->d,d->v,(size_t)SLOTS*NVJ_BLOCK);
    assert(nvj_format(&ops,size,0)==0);
    unsigned char *base=malloc((size_t)SLOTS*NVJ_BLOCK); assert(base); memcpy(base,d->d,(size_t)SLOTS*NVJ_BLOCK);
    unsigned char image[NVJ_BLOCK]; memset(image,'B',sizeof(image));
    int cases=0,steps=0;
    for(int persistence=0;persistence<3;persistence++) {
        for(int cut=1;;cut++) {
            memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d);
            struct nvj *j=NULL; assert(nvj_open(&j,&ops,size,0)==0);
            assert(nvj_begin(j,1,0)==0);
            assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
            assert(nvj_transfer(j,9*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
            d->step=0; d->cut=cut; d->partial=persistence; d->armed=1;
            int rc=nvj_commit(j),crashed=rc==-EIO;
            assert(rc==0 || crashed);
            if(crashed) {
                assert(nvj_recovery_required(j));
                assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,image,1)==-EIO);
                assert(nvj_begin(j,1,0)==-EIO);
            }
            int completed_steps=d->step;
            powerloss(d); nvj_close(j); j=NULL;
            assert(nvj_open(&j,&ops,size,0)==0);
            int outcome=check_pair(d);
            if(!crashed) assert(outcome=='B');
            /* A second recovery and another transaction prove safe slot reuse. */
            nvj_close(j); powerloss(d); assert(nvj_open(&j,&ops,size,0)==0); check_pair(d);
            assert(nvj_begin(j,1,0)==0);
            assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
            assert(nvj_transfer(j,9*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
            assert(nvj_commit(j)==0); nvj_close(j); cases++;
            if(!crashed) { steps=completed_steps; break; }
        }
    }
    /* Preserve a committed log before checkpoint and corrupt one payload. */
    memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d); d->partial=0;
    struct nvj *j=NULL; assert(nvj_open(&j,&ops,size,0)==0); assert(nvj_begin(j,1,0)==0);
    assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
    assert(nvj_transfer(j,9*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
    d->step=0; d->cut=1+4+NVJ_DESCRIPTORS+2+1+2; d->armed=1;
    assert(nvj_commit(j)==-EIO);
    powerloss(d); nvj_close(j);
    unsigned char *committed=malloc((size_t)SLOTS*NVJ_BLOCK); assert(committed);
    memcpy(committed,d->d,(size_t)SLOTS*NVJ_BLOCK);
    int replay_cases=0;
    for(int cut=1;;cut++) {
        memcpy(d->d,committed,(size_t)SLOTS*NVJ_BLOCK); powerloss(d);
        d->step=0; d->cut=cut; d->armed=1;
        int rc=nvj_open(&j,&ops,size,0); int interrupted=rc==-EIO;
        assert(rc==0 || interrupted); if(!rc) nvj_close(j);
        powerloss(d); assert(nvj_open(&j,&ops,size,0)==0);
        assert(check_pair(d)=='B'); nvj_close(j); replay_cases++;
        if(!interrupted) break;
    }
    memcpy(d->d,committed,(size_t)SLOTS*NVJ_BLOCK); powerloss(d); free(committed);
    d->d[(size_t)(16+NVJ_PAYLOAD)*NVJ_BLOCK+19]^=1; powerloss(d);
    assert(nvj_open(&j,&ops,size,0)==-EINVAL);
    assert(d->v[8*NVJ_BLOCK]=='A' && d->v[9*NVJ_BLOCK]=='A');
    /* Partial and overlapping data images must read from the overlay. */
    memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d);
    assert(nvj_open(&j,&ops,size,0)==0); assert(nvj_begin(j,1,1)==0);
    assert(nvj_transfer(j,8*NVJ_BLOCK+17,31,image,1)==0);
    unsigned char readback[NVJ_BLOCK];
    assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,readback,0)==0);
    for(unsigned i=0;i<NVJ_BLOCK;i++) assert(readback[i]==(i>=17 && i<48?'B':'A'));
    assert(nvj_transfer(j,9*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
    assert(nvj_commit(j)==-E2BIG); nvj_discard(j); check_pair(d); nvj_close(j);
    /* Data-only metadata-mode commits still require a persistence barrier. */
    assert(nvj_open(&j,&ops,size,0)==0); assert(nvj_begin(j,0,0)==0);
    assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
    assert(nvj_transfer(j,9*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
    assert(nvj_commit(j)==0); nvj_close(j); powerloss(d); assert(check_pair(d)=='B');
    printf("PASS journal engine: %d crash/persistence cases across %d boundaries; %d replay interruptions; corruption, overlay, budget, and fail-closed I/O checks\n",cases,steps,replay_cases);
    free(base); free(d->v); free(d->d); free(d); return 0;
}
