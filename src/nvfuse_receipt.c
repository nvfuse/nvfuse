/* SPDX-License-Identifier: GPL-2.0-only */

#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_tx_internal.h"
#include <string.h>
#define RECEIPTS 127u
static const unsigned char magic[8]={'N','V','T','X','K','E','Y','2'};
static int empty(const unsigned char *p,size_t n) {
    while(n--) if(*p++) return 0;
    return 1;
}
/* Validate the complete fixed page before returning or mutating any entry. */
static int page(struct nvfuse_handle *h,struct nvfuse_buffer_head **out) {
    struct nvfuse_buffer_head *bh=nvfuse_get_bh(&h->nvh_sb,NULL,ITABLE_INO,0,READ,NVFUSE_TYPE_META);
    if(!bh) return -ENOMEM;
    const unsigned char *p=(unsigned char *)bh->bh_buf;
    int rc=0;
    if(!empty(p,CLUSTER_SIZE)) {
        if(memcmp(p,magic,8) || p[8]!=1 || !empty(p+9,3) || p[12]!=RECEIPTS || !empty(p+13,19)) rc=-EIO;
        for(unsigned i=0;i<RECEIPTS && !rc;i++) {
            const unsigned char *e=p+32+i*32;
            if(e[0]>1 || !empty(e+1,3) || !empty(e+20,12) || (!e[0] && !empty(e,32))) rc=-EIO;
        }
    }
    if(rc) nvfuse_release_bh(&h->nvh_sb,bh,0,NVF_CLEAN);
    else *out=bh;
    return rc;
}
static int find(const unsigned char *p,const unsigned char *key,int *free_slot) {
    *free_slot=-1;
    for(unsigned i=0;i<RECEIPTS;i++) {
        const unsigned char *e=p+32+i*32;
        if(!e[0]) { if(*free_slot<0) *free_slot=(int)i; }
        else if(!memcmp(e+4,key,16)) return (int)i;
    }
    return -1;
}
int nvfuse_tx_query_key(struct nvfuse_handle *h,const unsigned char key[16],enum nvfuse_tx_outcome *out) {
    int state=nvfuse_tx_status(h); if(state<0) return state;
    if(state==NVFUSE_TX_RECOVERY_REQUIRED) return -EIO;
    if(!key || !out) return -EINVAL;
    struct io_target *t=h->nvh_target;
    if(nvj_version(t->journal)<2) return -EOPNOTSUPP;
    if(t->tx_keyed && !memcmp(t->tx_key,key,16)) { *out=NVFUSE_TX_IN_PROGRESS; return 0; }
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    struct nvfuse_buffer_head *bh=NULL; rc=page(h,&bh);
    if(!rc) {
        int free_slot; int slot=find((unsigned char *)bh->bh_buf,key,&free_slot);
        *out=slot>=0?NVFUSE_TX_COMMITTED:NVFUSE_TX_UNKNOWN;
        nvfuse_release_bh(&h->nvh_sb,bh,0,NVF_CLEAN);
    }
    return nvfuse_tx_leave(h,rc);
}
int nvfuse_tx_begin_key(struct nvfuse_handle *h,enum nvfuse_tx_mode mode,unsigned budget,const unsigned char key[16]) {
    int state=nvfuse_tx_status(h); if(state<0) return state;
    if(state==NVFUSE_TX_RECOVERY_REQUIRED) return -EIO;
    if(state!=NVFUSE_TX_IDLE || h->nvh_target->tx_depth) return -EBUSY;
    if(!key) return -EINVAL;
    if(nvj_version(h->nvh_target->journal)<2) return -EOPNOTSUPP;
    /* Read a slot before creating an explicit group. */
    int rc=nvfuse_tx_enter(h);if(rc) return rc;
    struct nvfuse_buffer_head *bh=NULL;rc=page(h,&bh);int free_slot=-1;
    if(!rc) {
        if(find((unsigned char *)bh->bh_buf,key,&free_slot)>=0) rc=-EALREADY;
        else if(free_slot<0) rc=-ENOSPC;
        nvfuse_release_bh(&h->nvh_sb,bh,0,NVF_CLEAN);
    }
    rc=nvfuse_tx_leave(h,rc);if(rc) return rc;
    rc=nvfuse_tx_begin(h,mode,budget);if(rc) return rc;
    rc=page(h,&bh);
    if(!rc) {
        unsigned char *p=(unsigned char *)bh->bh_buf;
        memcpy(p,magic,8);p[8]=1;p[12]=RECEIPTS;
        unsigned char *e=p+32+(unsigned)free_slot*32;e[0]=1;memcpy(e+4,key,16);
        nvfuse_release_bh(&h->nvh_sb,bh,0,DIRTY);
        memcpy(h->nvh_target->tx_key,key,16);h->nvh_target->tx_keyed=1;
    } else { int undo=nvfuse_tx_abort(h); if(undo) return undo; }
    return rc;
}
int nvfuse_tx_forget_key(struct nvfuse_handle *h,const unsigned char key[16]) {
    int state=nvfuse_tx_status(h);if(state<0) return state;
    if(state==NVFUSE_TX_RECOVERY_REQUIRED) return -EIO;
    if(state!=NVFUSE_TX_IDLE) return -EBUSY;
    if(!key) return -EINVAL;
    if(nvj_version(h->nvh_target->journal)<2) return -EOPNOTSUPP;
    int rc=nvfuse_tx_enter(h);if(rc) return rc;
    struct nvfuse_buffer_head *bh=NULL;rc=page(h,&bh);
    if(!rc) {
        int unused,slot=find((unsigned char *)bh->bh_buf,key,&unused);
        if(slot>=0) memset(bh->bh_buf+32+(unsigned)slot*32,0,32);
        nvfuse_release_bh(&h->nvh_sb,bh,0,slot>=0?DIRTY:NVF_CLEAN);
    }
    return nvfuse_tx_leave(h,rc);
}
