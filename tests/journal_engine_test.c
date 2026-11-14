/* SPDX-License-Identifier: GPL-2.0-only */

/* Independent volatile/durable storage oracle: an unflushed write can be lost,
 * persist entirely, or persist one sector. Crashes discard the volatile view. */
#include "nvfuse_journal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOME_SLOTS 128u
#define SLOTS (NVJ_BLOCKS+HOME_SLOTS)
struct disk { unsigned char *v,*d; unsigned char dirty[SLOTS]; int step,cut,partial,armed;
    unsigned calls, multi, home_calls, home_completed; int fail_home; };
static unsigned slot(uint64_t offset) {
    unsigned block=(unsigned)(offset/NVJ_BLOCK);
    if(block<HOME_SLOTS) return block;
    assert(block>=NVJ_START && block<NVJ_START+NVJ_BLOCKS);
    return HOME_SLOTS+block-NVJ_START;
}
static int boundary(struct disk *d) { return d->armed && ++d->step==d->cut ? -EIO : 0; }
static int io(void *ctx,uint64_t off,size_t n,void *buf,int write_op) {
    struct disk *d=ctx;
    d->calls++; if(n>NVJ_BLOCK) d->multi++;
    assert(n<=32u*NVJ_BLOCK);
    unsigned char *p=buf;
    while(n) {
        unsigned s=slot(off),within=(unsigned)(off%NVJ_BLOCK);
        size_t bytes=NVJ_BLOCK-within; if(bytes>n) bytes=n;
        if(write_op) {
            memcpy(d->v+(size_t)s*NVJ_BLOCK+within,p,bytes); d->dirty[s]=1;
            if(d->partial) {
                size_t persisted=d->partial==1?(bytes<512?bytes:512):bytes;
                memcpy(d->d+(size_t)s*NVJ_BLOCK+within,p,persisted);
            }
            /* A large request may tear/fail after ANY constituent block. */
            int rc=boundary(d); if(rc) return rc;
        } else memcpy(p,d->v+(size_t)s*NVJ_BLOCK+within,bytes);
        off+=bytes; p+=bytes; n-=bytes;
    }
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
/* Reverse completion order and keep draining after failure, as a synchronous
 * scatter callback must. Device persistence/tearing remains the independent
 * oracle above; this callback does not implement journal ordering. */
static int home_write(void *ctx,const struct nvj_home_write *writes,unsigned count) {
    struct disk *d=ctx; size_t bytes=0; int error=0;
    assert(count && count<=NVJ_HOME_BATCH_BLOCKS); d->home_calls++;
    for(unsigned i=0;i<count;i++) bytes+=writes[i].bytes;
    assert(bytes<=NVJ_HOME_BATCH_BLOCKS*NVJ_BLOCK);
    for(unsigned i=count;i-->0;) {
        int rc;
        if(d->fail_home && i==count/2) rc=-EIO;
        else { rc=io(ctx,writes[i].offset,writes[i].bytes,(void *)writes[i].buffer,1); d->home_completed++; }
        if(rc && !error) error=rc;
    }
    return error;
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
static int batch_mode;
static int home_mode;
static int compare_mode=1;
static int join_mode;
static int open_j(struct nvj **j,const struct nvj_ops *ops,uint64_t size,int meta) {
    int rc=nvj_open(j,ops,size,meta);
    if(!rc) { assert(nvj_set_batch_io(*j,batch_mode)==0); assert(nvj_set_compare_reads(*j,compare_mode)==0);
        assert(nvj_set_home_writer(*j,home_mode?home_write:NULL,ops->context)==0);
        assert(nvj_set_log_join(*j,join_mode)==0); nvj_set_profile(*j,1); }
    return rc;
}
int main(void) {
    struct disk *d=calloc(1,sizeof(*d)); assert(d);
    d->v=calloc(SLOTS,NVJ_BLOCK); d->d=calloc(SLOTS,NVJ_BLOCK); assert(d->v && d->d);
    struct nvj_ops ops={io,flush,d}; uint64_t size=(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK;
    memset(d->v+8*NVJ_BLOCK,'A',2*NVJ_BLOCK); memcpy(d->d,d->v,(size_t)SLOTS*NVJ_BLOCK);
    assert(nvj_format(&ops,size,0)==0);
    unsigned char *base=malloc((size_t)SLOTS*NVJ_BLOCK); assert(base); memcpy(base,d->d,(size_t)SLOTS*NVJ_BLOCK);
    unsigned char image[NVJ_BLOCK]; memset(image,'B',sizeof(image));
    /* Async bypass must never hide staged metadata, including blocks that
     * were metadata earlier and have since been reused as regular data. */
    struct nvj *probe=NULL; assert(nvj_open(&probe,&ops,size,0)==0);
    assert(nvj_begin(probe,0,0)==0);
    assert(!nvj_needs_overlay(probe,8*NVJ_BLOCK,NVJ_BLOCK,1));
    nvj_mark_metadata(probe,8);
    assert(nvj_needs_overlay(probe,8*NVJ_BLOCK,NVJ_BLOCK,1));
    assert(!nvj_needs_overlay(probe,8*NVJ_BLOCK,NVJ_BLOCK,0));
    assert(nvj_transfer(probe,8*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
    assert(nvj_needs_overlay(probe,8*NVJ_BLOCK,NVJ_BLOCK,0));
    assert(nvj_needs_overlay(probe,NVJ_START*NVJ_BLOCK,NVJ_BLOCK,0));
    assert(nvj_needs_overlay(probe,size,NVJ_BLOCK,1));
    assert(nvj_commit(probe)==0);
    assert(nvj_begin(probe,0,0)==0);
    uint64_t barriers=nvj_stats(probe)->flushes;
    nvj_note_data_write(probe);
    assert(nvj_commit(probe)==0 && nvj_stats(probe)->flushes==barriers+1);
    nvj_close(probe);
    int total=0;
    for(join_mode=0;join_mode<=1;join_mode++)
    for(home_mode=0;home_mode<=1;home_mode++)
    for(batch_mode=0;batch_mode<=1;batch_mode++) for(compare_mode=0;compare_mode<=1;compare_mode++) {
    int cases=0,steps=0;
    for(int persistence=0;persistence<3;persistence++) {
        for(int cut=1;;cut++) {
            memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d);
            struct nvj *j=NULL; assert(open_j(&j,&ops,size,0)==0);
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
            assert(open_j(&j,&ops,size,0)==0);
            int outcome=check_pair(d);
            if(!crashed) assert(outcome=='B');
            /* A second recovery and another transaction prove safe slot reuse. */
            nvj_close(j); powerloss(d); assert(open_j(&j,&ops,size,0)==0); check_pair(d);
            assert(nvj_begin(j,1,0)==0);
            assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
            assert(nvj_transfer(j,9*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
            assert(nvj_commit(j)==0); nvj_close(j); cases++;
            if(!crashed) { steps=completed_steps; break; }
        }
    }
    /* Preserve a committed log before checkpoint and corrupt one payload. */
    memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d); d->partial=0;
    struct nvj *j=NULL; assert(open_j(&j,&ops,size,0)==0); assert(nvj_begin(j,1,0)==0);
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
        int rc=open_j(&j,&ops,size,0); int interrupted=rc==-EIO;
        assert(rc==0 || interrupted); if(!rc) nvj_close(j);
        powerloss(d); assert(open_j(&j,&ops,size,0)==0);
        assert(check_pair(d)=='B'); nvj_close(j); replay_cases++;
        if(!interrupted) break;
    }
    memcpy(d->d,committed,(size_t)SLOTS*NVJ_BLOCK); powerloss(d); free(committed);
    d->d[(size_t)(HOME_SLOTS+NVJ_PAYLOAD)*NVJ_BLOCK+19]^=1; powerloss(d);
    assert(open_j(&j,&ops,size,0)==-EINVAL);
    assert(d->v[8*NVJ_BLOCK]=='A' && d->v[9*NVJ_BLOCK]=='A');
    /* Partial and overlapping data images must read from the overlay. */
    memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d);
    assert(open_j(&j,&ops,size,0)==0); assert(nvj_begin(j,1,1)==0);
    assert(nvj_transfer(j,8*NVJ_BLOCK+17,31,image,1)==0);
    unsigned char readback[NVJ_BLOCK];
    assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,readback,0)==0);
    for(unsigned i=0;i<NVJ_BLOCK;i++) assert(readback[i]==(i>=17 && i<48?'B':'A'));
    assert(nvj_transfer(j,9*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
    assert(nvj_commit(j)==-E2BIG); nvj_discard(j); check_pair(d); nvj_close(j);
    /* Data-only metadata-mode commits still require a persistence barrier. */
    assert(open_j(&j,&ops,size,0)==0); assert(nvj_begin(j,0,0)==0);
    assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
    assert(nvj_transfer(j,9*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
    assert(nvj_commit(j)==0); nvj_close(j); powerloss(d); assert(check_pair(d)=='B');
    printf("PASS journal engine: %d crash/persistence cases across %d boundaries; %d replay interruptions; corruption, overlay, budget, and fail-closed I/O checks\n",cases,steps,replay_cases);
    total+=cases;
    }
    /* Exercise multiple 128 KiB payload/home runs, byte identity, and transfer-call
     * reduction. No hooks: this must execute actual multi-block callbacks. */
    unsigned call_counts[2];
    join_mode=1;
    compare_mode=0;
    home_mode=1;
    for(batch_mode=0;batch_mode<=1;batch_mode++) {
        memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d); d->partial=0;
        struct nvj *large=NULL; assert(open_j(&large,&ops,size,0)==0);
        assert(nvj_begin(large,1,0)==0);
        assert(nvj_set_batch_io(large,1)==-EBUSY);
        assert(nvj_set_home_writer(large,home_write,d)==-EBUSY);
        assert(nvj_set_log_join(large,1)==-EBUSY);
        for(unsigned block=8;block<88;block++) {
            memset(image,(int)block,sizeof(image));
            assert(nvj_transfer(large,(uint64_t)block*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
        }
        d->calls=0; d->multi=0;
        assert(nvj_commit(large)==0); call_counts[batch_mode]=d->calls;
        assert(nvj_stats(large)->flushes==9);
        assert(nvj_stats(large)->profile_ns[NVJ_P_LOG_WRITE]>0);
        assert((d->multi>0)==batch_mode);
        nvj_close(large); powerloss(d);
        for(unsigned block=8;block<88;block++) for(unsigned k=0;k<NVJ_BLOCK;k++)
            assert(d->v[(size_t)block*NVJ_BLOCK+k]==block);
    }
    assert(call_counts[1]<call_counts[0]);
    printf("PASS both I/O modes: %d crash/persistence cases; 80-block commit transfers %u -> %u\n",total,call_counts[0],call_counts[1]);
    batch_mode=1;
    /* The merged request must save exactly one transfer when profitable,
     * preserve every persisted byte, and stay off for unprofitable tails. */
    const unsigned counts[]={1,16,17,32,33,48,64,80};
    unsigned char *reference=malloc((size_t)SLOTS*NVJ_BLOCK); assert(reference);
    for(unsigned k=0;k<sizeof(counts)/sizeof(counts[0]);k++) {
        unsigned calls[2],count=counts[k];
        int profitable=count%32 && count%32<=16;
        for(join_mode=0;join_mode<=1;join_mode++) {
            memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d); d->partial=0;
            struct nvj *j=NULL; assert(open_j(&j,&ops,size,0)==0);
            assert(nvj_set_log_join(j,2)==-EINVAL);
            assert(nvj_begin(j,1,0)==0);
            for(unsigned b=8;b<8+count;b++) {
                memset(image,(int)b,sizeof(image));
                assert(nvj_transfer(j,(uint64_t)b*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
            }
            d->calls=0;
            assert(nvj_commit(j)==0); calls[join_mode]=d->calls;
            assert(nvj_stats(j)->flushes==9);
            assert(nvj_stats(j)->log_join_commits==(unsigned)(join_mode && profitable));
            assert(nvj_stats(j)->log_join_blocks==(unsigned)(join_mode && profitable?(count<16?count:16):0));
            nvj_close(j); powerloss(d);
            if(!join_mode) memcpy(reference,d->d,(size_t)SLOTS*NVJ_BLOCK);
            else assert(!memcmp(reference,d->d,(size_t)SLOTS*NVJ_BLOCK));
        }
        assert(calls[0]-calls[1]==(unsigned)profitable);
    }
    free(reference); join_mode=1;
    puts("PASS joined log: eight boundary sizes, exact request savings, persisted byte identity and unchanged flushes");
    /* Multiple scatter windows, including a partially failed window: later
     * checkpoint writes can complete before earlier ones. A failed checkpoint
     * must keep the committed redo log for recovery, never retire it. */
    for(int fail=0;fail<=1;fail++) {
        memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d); d->partial=1;
        struct nvj *j=NULL; assert(open_j(&j,&ops,size,0)==0);
        assert(nvj_begin(j,1,0)==0);
        for(unsigned block=8;block<128;block+=2) {
            memset(image,(int)block,sizeof(image));
            assert(nvj_transfer(j,(uint64_t)block*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
        }
        d->home_calls=d->home_completed=0; d->fail_home=fail;
        assert(nvj_commit(j)==(fail?-EIO:0)); d->fail_home=0;
        assert(d->home_calls==(unsigned)(fail?1:2));
        assert(d->home_completed==(unsigned)(fail?31:60));
        assert(nvj_stats(j)->home_requests==(unsigned)(fail?32:60));
        assert(nvj_stats(j)->flushes==(unsigned)(fail?6:9));
        assert(nvj_recovery_required(j)==fail);
        nvj_close(j); powerloss(d); assert(open_j(&j,&ops,size,0)==0);
        nvj_close(j); powerloss(d);
        for(unsigned block=8;block<128;block+=2) for(unsigned k=0;k<NVJ_BLOCK;k++)
            assert(d->v[(size_t)block*NVJ_BLOCK+k]==block);
    }
    puts("PASS scatter checkpoint: reversed completion, bounded windows, partial write failure, full drain and retained-log recovery");
    /* Compare suppression is optional. A complete image needs no old bytes;
     * partial images must retain their read-modify-write and overlap semantics. */
    for(compare_mode=0;compare_mode<=1;compare_mode++) {
        memcpy(d->d,base,(size_t)SLOTS*NVJ_BLOCK); powerloss(d); d->partial=0;
        struct nvj *j=NULL; assert(open_j(&j,&ops,size,0)==0);
        nvj_mark_metadata(j,8); nvj_mark_metadata(j,9);
        assert(nvj_begin(j,0,1)==0);
        assert(nvj_set_compare_reads(j,1)==-EBUSY);
        memset(image,'A',sizeof(image)); d->calls=0;
        assert(nvj_transfer(j,8*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
        assert(d->calls==(unsigned)compare_mode);
        assert(nvj_stats(j)->compare_reads==(unsigned)compare_mode);
        assert(nvj_stats(j)->compare_equal==(unsigned)compare_mode);
        /* An unchanged full image consumes one credit only without compare. */
        assert(nvj_transfer(j,9*NVJ_BLOCK,NVJ_BLOCK,image,1)==0);
        assert(nvj_commit(j)==(compare_mode?0:-E2BIG));
        nvj_discard(j);
        assert(nvj_begin(j,0,0)==0); d->calls=0; memset(image,'B',sizeof(image));
        assert(nvj_transfer(j,8*NVJ_BLOCK+17,31,image,1)==0);
        assert(d->calls==1);
        assert(nvj_transfer(j,8*NVJ_BLOCK+29,31,image,1)==0);
        assert(d->calls==1);
        assert(nvj_commit(j)==0); nvj_close(j); powerloss(d);
        for(unsigned k=0;k<NVJ_BLOCK;k++) assert(d->v[8*NVJ_BLOCK+k]==(k>=17 && k<60?'B':'A'));
    }
    puts("PASS compare policy: full images avoid reads, partial/overlap preserve bytes, unchanged-image budget remains fail-closed");
    free(base); free(d->v); free(d->d); free(d); return 0;
}
