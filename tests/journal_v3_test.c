/* SPDX-License-Identifier: GPL-2.0-only */

/* Independent durability oracle. Writes may persist arbitrary sectors before
 * flush, including a tail/header before its body. Every acknowledged commit
 * must survive, and an interrupted transaction must replay atomically. */
#include "nvfuse_journal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define HOMES 128u
#define SLOTS (HOMES+NVJ_BLOCKS)
#define DISK_BYTES ((size_t)SLOTS*NVJ_BLOCK)
struct disk { unsigned char *live,*stable; unsigned char dirty[SLOTS]; unsigned step,cut,mode; int armed; };
static unsigned index_of(uint64_t off) {
    unsigned b=(unsigned)(off/NVJ_BLOCK);
    if(b<HOMES) return b;
    assert(b>=NVJ_START && b<NVJ_START+NVJ_BLOCKS); return HOMES+b-NVJ_START;
}
static int boundary(struct disk *d) { return d->armed && ++d->step==d->cut ? -EIO:0; }
static int io(void *ctx,uint64_t off,size_t bytes,void *buf,int write_op) {
    struct disk *d=ctx; unsigned char *p=buf;
    assert(!(off%NVJ_BLOCK) && !(bytes%NVJ_BLOCK) && bytes<=32u*NVJ_BLOCK);
    while(bytes) {
        unsigned s=index_of(off); unsigned char *v=d->live+(size_t)s*NVJ_BLOCK,*stable=d->stable+(size_t)s*NVJ_BLOCK;
        if(write_op) {
            memcpy(v,p,NVJ_BLOCK); d->dirty[s]=1;
            if(d->mode==1) memcpy(stable,p,NVJ_BLOCK);
            else if(d->mode==2) memcpy(stable,p,512);
            else if(d->mode==3) memcpy(stable+NVJ_BLOCK-512,p+NVJ_BLOCK-512,512);
            else if(d->mode==4) for(unsigned k=0;k<8;k+=2) memcpy(stable+k*512,p+k*512,512);
            int rc=boundary(d); if(rc) return rc;
        } else memcpy(p,v,NVJ_BLOCK);
        p+=NVJ_BLOCK; off+=NVJ_BLOCK; bytes-=NVJ_BLOCK;
    }
    return 0;
}
static int flush(void *ctx) {
    struct disk *d=ctx;
    /* Reordered persistence followed by completed flush. */
    for(unsigned i=SLOTS;i-->0;) if(d->dirty[i]) { memcpy(d->stable+(size_t)i*NVJ_BLOCK,d->live+(size_t)i*NVJ_BLOCK,NVJ_BLOCK); d->dirty[i]=0; }
    return boundary(d);
}
static void crash(struct disk *d) { d->armed=0; memcpy(d->live,d->stable,DISK_BYTES); memset(d->dirty,0,sizeof(d->dirty)); }
static int open_j(struct nvj **j,struct nvj_ops *ops) {
    int rc=nvj_open(j,ops,(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK,0);
    if(!rc) { assert(nvj_version(*j)==3); assert(!nvj_set_batch_io(*j,1)); assert(!nvj_set_compare_reads(*j,0)); }
    return rc;
}
static int change(struct nvj *j,unsigned version) {
    unsigned char image[NVJ_BLOCK]; memset(image,0,sizeof(image));
    /* Most bytes stay identical, ensuring compact delta replay is exercised. */
    memcpy(image+768,&version,sizeof(version));
    int rc=nvj_begin(j,1,0);
    if(!rc) rc=nvj_transfer(j,8u*NVJ_BLOCK,NVJ_BLOCK,image,1);
    if(!rc) rc=nvj_transfer(j,9u*NVJ_BLOCK,NVJ_BLOCK,image,1);
    if(!rc) rc=nvj_commit(j);
    return rc;
}
static unsigned state(struct disk *d) {
    assert(!memcmp(d->live+8u*NVJ_BLOCK,d->live+9u*NVJ_BLOCK,NVJ_BLOCK));
    unsigned version; memcpy(&version,d->live+8u*NVJ_BLOCK+768,sizeof(version));
    unsigned char image[NVJ_BLOCK]={0}; memcpy(image+768,&version,sizeof(version));
    assert(!memcmp(image,d->live+8u*NVJ_BLOCK,NVJ_BLOCK)); return version;
}
static int file_io(void *arg,uint64_t off,size_t bytes,void *buf,int write_op) {
    int fd=*(int *)arg; unsigned char *p=buf;
    while(bytes) {
        ssize_t n=write_op?pwrite(fd,p,bytes,(off_t)off):pread(fd,p,bytes,(off_t)off);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return n<0?-errno:-EIO;
        off+=(size_t)n; p+=n; bytes-=(size_t)n;
    }
    return 0;
}
static int file_flush(void *arg) { return fsync(*(int *)arg)?-errno:0; }
/* These homes collide in the initial active index. Clearing the first key
 * must not stop a later key's removal at the newly created probe-chain hole.
 * A subsequent partial update must read the committed image, never stale TX
 * bytes. Exercise both reset policies and both writable format versions. */
static void reset_cases(void) {
    const unsigned homes[]={59,135,846,2587};
    for(unsigned version=2;version<=3;version++) for(unsigned policy=0;policy<2;policy++) {
        assert(!setenv("NVFUSE_JOURNAL_SPARSE_RESET",policy?"1":"0",1));
        char name[]="/tmp/nvfuse-reset-XXXXXX";
        int fd=mkstemp(name); assert(fd>=0); assert(!unlink(name));
        uint64_t bytes=(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK;
        assert(!ftruncate(fd,(off_t)bytes)); struct nvj_ops ops={file_io,file_flush,&fd};
        assert(!nvj_format_version(&ops,bytes,0,NVJ_BLOCKS,version));
        struct nvj *j=NULL; assert(!nvj_open(&j,&ops,bytes,0));
        unsigned char block[NVJ_BLOCK],got[NVJ_BLOCK];
        for(unsigned round=0;round<4;round++) {
            assert(!nvj_begin(j,1,0));
            for(unsigned k=0;k<4;k++) {
                memset(block,(int)(k+1),sizeof(block));
                assert(!nvj_transfer(j,(uint64_t)homes[k]*NVJ_BLOCK,NVJ_BLOCK,block,1));
            }
            assert(!nvj_commit(j));
            assert(!nvj_begin(j,1,0));
            for(unsigned k=0;k<4;k++) {
                memset(block,0xee,sizeof(block));
                assert(!nvj_transfer(j,(uint64_t)homes[k]*NVJ_BLOCK,NVJ_BLOCK,block,1));
            }
            nvj_discard(j);
            assert(!nvj_begin(j,1,0));
            for(unsigned k=0;k<4;k++) {
                unsigned char patch=(unsigned char)(round+7);
                assert(!nvj_transfer(j,(uint64_t)homes[k]*NVJ_BLOCK+123,1,&patch,1));
                assert(!nvj_transfer(j,(uint64_t)homes[k]*NVJ_BLOCK,NVJ_BLOCK,got,0));
                memset(block,(int)(k+1),sizeof(block)); block[123]=patch;
                assert(!memcmp(block,got,NVJ_BLOCK));
            }
            assert(!nvj_commit(j));
        }
        nvj_close(j); assert(!nvj_open(&j,&ops,bytes,0)); nvj_close(j); assert(!close(fd));
    }
    assert(!unsetenv("NVFUSE_JOURNAL_SPARSE_RESET"));
    puts("PASS v2/v3 colliding active-index discard and partial restage, both reset policies");
}
/* Real sparse file bounds/encoding test, separate from the durability oracle.
 * Full-capacity records cross the fixed discovery anchor for larger extents. */
static void capacity_cases(void) {
    for(unsigned blocks=NVJ_BLOCKS;blocks<=16384;blocks*=2) {
        char name[]="/tmp/nvfuse-v3-capacity-XXXXXX";
        int fd=mkstemp(name); assert(fd>=0); assert(!unlink(name));
        uint64_t bytes=(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK; assert(!ftruncate(fd,(off_t)bytes));
        struct nvj_ops ops={file_io,file_flush,&fd};
        assert(!nvj_format_version(&ops,bytes,0,blocks,3));
        struct nvj *j=NULL; assert(!open_j(&j,&ops)); unsigned n=nvj_capacity(j);
        assert(n<nvj_journal_start(j));
        unsigned char *images=malloc((size_t)n*NVJ_BLOCK); assert(images);
        for(unsigned i=0;i<n;i++) memset(images+(size_t)i*NVJ_BLOCK,(unsigned char)(i%251),(size_t)NVJ_BLOCK);
        assert(nvj_begin(j,1,n+1)==-EINVAL);
        assert(!nvj_begin(j,1,n)); assert(!nvj_transfer(j,0,(size_t)n*NVJ_BLOCK,images,1)); assert(!nvj_commit(j));
        assert(!nvj_begin(j,0,1)); /* An earlier full-data image remains protected. */
        unsigned char patch=0xf9; images[100u*NVJ_BLOCK+233]=patch;
        assert(!nvj_transfer(j,100u*NVJ_BLOCK+233,1,&patch,1)); assert(!nvj_commit(j));
        nvj_close(j); assert(!open_j(&j,&ops));
        unsigned char actual[NVJ_BLOCK];
        for(unsigned i=0;i<n;i++) { assert(!nvj_transfer(j,(uint64_t)i*NVJ_BLOCK,NVJ_BLOCK,actual,0)); assert(!memcmp(actual,images+(size_t)i*NVJ_BLOCK,NVJ_BLOCK)); }
        uint64_t bound=0; assert(!nvj_probe(&ops,bytes,&bound,0) && bound==bytes);
        nvj_close(j); free(images); assert(!close(fd));
        printf("PASS v3 capacity: journal=%u blocks, full TX=%u images, anchor preserved, pressure checkpoint, partial protected-data update, reopen\n",blocks,n);
    }
}
int main(void) {
    reset_cases();
    struct disk d={0}; d.live=calloc(1,DISK_BYTES); d.stable=calloc(1,DISK_BYTES); assert(d.live && d.stable);
    struct nvj_ops ops={io,flush,&d}; uint64_t bytes=(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK;
    assert(!nvj_format_version(&ops,bytes,0,NVJ_BLOCKS,3));
    unsigned char *base=malloc(DISK_BYTES); assert(base); memcpy(base,d.stable,DISK_BYTES);
    unsigned cases=0;
    for(unsigned mode=0;mode<5;mode++) for(unsigned cut=1;;cut++) {
        memcpy(d.stable,base,DISK_BYTES); crash(&d); struct nvj *j=NULL; assert(!open_j(&j,&ops));
        d.step=0; d.cut=cut; d.mode=mode; d.armed=1;
        unsigned ack=0,attempted=0; int rc=0;
        for(unsigned v=1;v<=6 && !rc;v++) {
            attempted=v; rc=change(j,v); if(!rc) ack=v;
            if(!rc && v==3) rc=nvj_checkpoint(j);
        }
        unsigned steps=d.step; assert(!rc || rc==-EIO);
        if(rc) { assert(nvj_recovery_required(j)); assert(nvj_begin(j,1,0)==-EIO); }
        nvj_close(j); crash(&d); assert(!open_j(&j,&ops));
        unsigned got=state(&d); assert(got>=ack && got<=attempted); nvj_close(j);
        crash(&d); assert(!open_j(&j,&ops)); assert(state(&d)==got); nvj_close(j); cases++;
        if(!rc) { assert(cut>steps); break; }
    }
    printf("PASS v3 %u boundary/persistence cases: append, deltas, checkpoint, dual epoch controls, acknowledged prefix, atomic transaction, repeated recovery\n",cases);
    memcpy(d.stable,base,DISK_BYTES); crash(&d); d.mode=0;
    struct nvj *j=NULL; assert(!open_j(&j,&ops));
    for(unsigned v=1;v<=9000;v++) assert(!change(j,v)); /* Real pressure and multiple epoch reuse. */
    unsigned char out[NVJ_BLOCK]; assert(!nvj_transfer(j,8u*NVJ_BLOCK,NVJ_BLOCK,out,0));
    unsigned value; memcpy(&value,out+768,sizeof(value)); assert(value==9000);
    assert(nvj_needs_overlay(j,8u*NVJ_BLOCK,NVJ_BLOCK,0));
    uint64_t log_bytes=nvj_stats(j)->journal_bytes;
    assert(log_bytes<UINT64_C(9000)*2*NVJ_BLOCK);
    /* Discard an active mutation without losing earlier committed overlays. */
    assert(!nvj_begin(j,1,0)); memset(out,0x6b,sizeof(out)); assert(!nvj_transfer(j,8u*NVJ_BLOCK,NVJ_BLOCK,out,1)); nvj_discard(j);
    assert(!nvj_transfer(j,8u*NVJ_BLOCK,NVJ_BLOCK,out,0)); memcpy(&value,out+768,sizeof(value)); assert(value==9000);
    nvj_close(j); crash(&d); assert(!open_j(&j,&ops)); assert(state(&d)==9000); nvj_close(j);
    printf("PASS v3 9000 commits: actual log pressure, multiple epoch reuse, latest overlay, discard isolation, compact traffic=%llu bytes\n",(unsigned long long)log_bytes);
    free(base); free(d.live); free(d.stable); capacity_cases(); return 0;
}
