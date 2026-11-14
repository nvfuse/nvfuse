/* SPDX-License-Identifier: GPL-2.0-only */

/* Persistent zero-link files. All callers use the standalone owner thread. */
#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_tx_internal.h"

static inode_t head(struct nvfuse_superblock *sb) {
    struct nvfuse_inode_ctx *root=nvfuse_read_inode(sb,NULL,sb->sb_root_ino);
    inode_t ino=root->ictx_inode->resv2[0];
    nvfuse_release_inode(sb,root,NVF_CLEAN); return ino;
}
static void edge(struct nvfuse_superblock *sb,inode_t parent,inode_t next) {
    struct nvfuse_inode_ctx *ctx=nvfuse_read_inode(sb,NULL,parent);
    ctx->ictx_inode->resv2[0]=next; nvfuse_release_inode(sb,ctx,DIRTY);
}
int nvfuse_orphan_add(struct nvfuse_superblock *sb,inode_t ino) {
    struct nvfuse_inode_ctx *ctx=nvfuse_read_inode(sb,NULL,ino);
    int listed=!!(ctx->ictx_inode->resv0&1u);
    nvfuse_release_inode(sb,ctx,NVF_CLEAN);
    if(!listed) { inode_t next=head(sb); edge(sb,ino,next); edge(sb,sb->sb_root_ino,ino); }
    return 0;
}
void nvfuse_cleanup_add(struct nvfuse_superblock *sb,struct nvfuse_inode_ctx *ctx) {
    if(ctx->ictx_inode->resv0&1u) return;
    ctx->ictx_inode->resv2[0]=head(sb);
    ctx->ictx_inode->resv0|=1u;
    edge(sb,sb->sb_root_ino,ctx->ictx_inode->i_ino);
}
static int opened(struct nvfuse_superblock *sb,inode_t ino) {
    for(unsigned i=START_OPEN_FILE;i<MAX_OPEN_FILE;i++)
        if(sb->sb_file_table[i].used && sb->sb_file_table[i].ino==ino) return 1;
    return 0;
}
/* Read the raw inode before read_inode's legacy identity assertion. */
static int inspect(struct nvfuse_superblock *sb,inode_t ino,inode_t *next) {
    if(ino<NUM_RESV_INO || ino>=(uint64_t)sb->sb_bg_num*sb->sb_no_of_inodes_per_bg) return -EIO;
    struct nvfuse_buffer_head *bh=nvfuse_get_bh(sb,NULL,ITABLE_INO,ino/INODE_ENTRY_NUM,READ,NVFUSE_TYPE_META);
    if(!bh) return -EIO;
    struct nvfuse_inode *node=(struct nvfuse_inode *)bh->bh_buf+ino%INODE_ENTRY_NUM;
    int rc=node->i_ino!=ino || node->i_deleted ||
        (node->i_links_count && !(node->resv0&1u)) || (node->resv0&~1u) ||
        node->i_type!=NVFUSE_TYPE_FILE || node->i_size<0 || node->i_size>=MAX_FILE_SIZE ? -EIO:0;
    /* Check size before rounding it: malformed on-disk values may overflow. */
    if(!rc && (node->resv0&1u) &&
       (node->i_parent<(uint64_t)(node->i_size+CLUSTER_SIZE-1)/CLUSTER_SIZE ||
        node->i_parent>(uint64_t)(MAX_FILE_SIZE+CLUSTER_SIZE-1)/CLUSTER_SIZE)) rc=-EIO;
    *next=node->resv2[0]; nvfuse_release_bh(sb,bh,0,NVF_CLEAN); return rc;
}
static s64 last_tree(struct nvfuse_superblock *sb,uint32_t block,unsigned level,s64 base) {
    if(!block) return -1;
    if(!level) return base;
    struct nvfuse_buffer_head *bh=nvfuse_get_bh(sb,NULL,BLOCK_IO_INO,block,READ,NVFUSE_TYPE_META);
    if(!bh) return -2;
    uint32_t *words=(uint32_t *)bh->bh_buf;
    s64 stride=INT64_C(1)<<((level-1)*PTRS_PER_BLOCK_BITS),last=-1;
    for(int i=PTRS_PER_BLOCK-1;i>=0;i--) if(words[i]) {
        last=last_tree(sb,words[i],level-1,base+i*stride); break;
    }
    nvfuse_release_bh(sb,bh,0,NVF_CLEAN); return last;
}
static s64 last_mapping(struct nvfuse_superblock *sb,struct nvfuse_inode *node) {
    s64 base=DIRECT_BLOCKS+PTRS_PER_BLOCK+(s64)PTRS_PER_BLOCK*PTRS_PER_BLOCK;
    for(int level=3;level>=1;level--) {
        s64 last=last_tree(sb,node->i_blocks[DIRECT_BLOCKS+level-1],(unsigned)level,base);
        if(last!=-1) return last;
        if(level==3) base=DIRECT_BLOCKS+PTRS_PER_BLOCK;
        else base=DIRECT_BLOCKS;
    }
    for(int i=DIRECT_BLOCKS-1;i>=0;i--) if(node->i_blocks[i]) return i;
    return -1;
}
int nvfuse_orphan_reap(struct nvfuse_handle *h) {
    struct io_target *t=h->nvh_target;
    if(!t->journal || nvj_version(t->journal)<2 || t->tx_user || t->tx_depth) return 0;
    struct nvfuse_superblock *sb=&h->nvh_sb;
    for(;;) {
        inode_t first=head(sb),ino=first,next=0;
        uint64_t limit=(uint64_t)sb->sb_bg_num*sb->sb_no_of_inodes_per_bg;
        /* Validate the complete list before any destructive step. */
        while(ino && limit--) { int rc=inspect(sb,ino,&next); if(rc) return rc; ino=next; }
        if(ino) return -ELOOP;
        inode_t parent=sb->sb_root_ino;
        for(ino=first;ino;) {
            int rc=inspect(sb,ino,&next); if(rc) return rc;
            struct nvfuse_inode_ctx *candidate=nvfuse_read_inode(sb,NULL,ino);
            int linked=candidate->ictx_inode->i_links_count!=0;
            nvfuse_release_inode(sb,candidate,NVF_CLEAN);
            if(linked || !opened(sb,ino)) break;
            parent=ino; ino=next;
        }
        if(!ino) return 0;
        int rc=nvj_begin(t->journal,0,0); if(rc) return rc;
        struct nvfuse_inode_ctx *ctx=nvfuse_read_inode(sb,NULL,ino);
        /* At most 256 data mappings, their indirect paths and allocation
         * groups can change in one step, independently of total file size. */
        struct nvfuse_inode *node=ctx->ictx_inode;
        int linked=node->i_links_count!=0;
        s64 visible=node->i_size,remaining=visible;
        if((node->resv0&1u) && (s64)node->i_parent*CLUSTER_SIZE>remaining)
            remaining=(s64)node->i_parent*CLUSTER_SIZE;
        s64 floor=linked?((visible+CLUSTER_SIZE-1)/CLUSTER_SIZE)*CLUSTER_SIZE:0;
        s64 target=remaining>1024*1024 ? ((remaining-1)/(1024*1024))*(1024*1024):0;
        if(target<floor) target=floor;
        node->i_size=remaining;
        nvfuse_free_inode_size(sb,ctx,target);
        s64 last=last_mapping(sb,node);
        if(last==-2) { nvfuse_release_inode(sb,ctx,DIRTY); nvj_require_recovery(t->journal); return -EIO; }
        if((last+1)*CLUSTER_SIZE<target) target=(last+1)*CLUSTER_SIZE;
        if(target<floor) target=floor;
        if(linked) {
            node->i_size=visible; node->i_parent=(inode_t)(target/CLUSTER_SIZE);
        } else { node->i_size=target; node->i_parent=(inode_t)(target/CLUSTER_SIZE); }
        if(target<=floor) {
            node->resv0&=(uint16_t)~1u; node->i_parent=0; node->resv2[0]=0;
            if(linked) nvfuse_release_inode(sb,ctx,DIRTY);
            else nvfuse_relocate_delete_inode(sb,ctx);
            edge(sb,parent,next);
        } else nvfuse_release_inode(sb,ctx,DIRTY);
        nvfuse_check_flush_dirty(sb,DIRTY_FLUSH_FORCE);
        int sync_error=nvfuse_sync_superblock(sb);
        if(sync_error) nvj_fail(t->journal,sync_error<0?sync_error:-EIO);
        rc=nvj_error(t->journal);
        if(!rc) rc=nvj_commit(t->journal);
        if(rc) { nvj_require_recovery(t->journal); return rc; }
    }
}
