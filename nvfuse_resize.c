#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_indirect.h"
#include "nvfuse_tx_internal.h"
/* A shrink pending inside an explicit group must be materialized before
 * subsequent growth/write can make its old mappings reachable again. */
int nvfuse_finish_inode_suffix(struct nvfuse_superblock *sb,struct nvfuse_inode_ctx *ctx) {
    struct nvfuse_inode *node=ctx->ictx_inode;
    if(!(node->resv0&1u)) return 0;
    s64 visible=node->i_size;
    s64 floor=(visible+CLUSTER_SIZE-1)/CLUSTER_SIZE;
    if((s64)node->i_parent-floor>256) return -E2BIG;
    node->i_size=(s64)node->i_parent*CLUSTER_SIZE;
    nvfuse_free_inode_size(sb,ctx,visible); node->i_size=visible;
    node->i_parent=(inode_t)((visible+CLUSTER_SIZE-1)/CLUSTER_SIZE);
    return 0;
}
/* Caller holds the inode. Holes never acquire a cache entry or physical block. */
int nvfuse_resize_inode(struct nvfuse_superblock *sb,struct nvfuse_inode_ctx *ctx,s64 size) {
    struct nvfuse_inode *inode=ctx->ictx_inode;
    if(size<0 || size>=MAX_FILE_SIZE) return -EINVAL;
    if(nvj_version(sb->target->journal)<2 && size>inode->i_size) return -EOPNOTSUPP;
    if(inode->i_type!=NVFUSE_TYPE_FILE) return -EISDIR;
    s64 old=inode->i_size;
    if(size>old) { int rc=nvfuse_finish_inode_suffix(sb,ctx);if(rc) return rc; }
    s64 boundary=size<old?size:old;
    if(size!=old && (boundary&(CLUSTER_SIZE-1))) {
        u32 pblock=0;
        int rc=nvfuse_get_block(sb,ctx,NVFUSE_SIZE_TO_BLK(boundary),1,NULL,&pblock,0);
        if(rc) return rc;
        if(pblock && !sb->target->metadata_only) {
            /* The tail initialization belongs to the structural resize. Protect
             * it even in ordered metadata mode so an uncommitted shrink cannot
             * clear bytes that remain visible under the old committed size. */
            nvj_mark_metadata(sb->target->journal,pblock);
            struct nvfuse_buffer_head *bh=nvfuse_get_bh(sb,ctx,inode->i_ino,NVFUSE_SIZE_TO_BLK(boundary),READ,NVFUSE_TYPE_DATA);
            if(!bh) return -ENOMEM;
            unsigned at=(unsigned)(boundary&(CLUSTER_SIZE-1));
            memset(bh->bh_buf+at,0,CLUSTER_SIZE-at);
            nvfuse_release_bh(sb,bh,0,DIRTY);
        }
    }
    if(size<old) {
        if(nvj_version(sb->target->journal)>=2) {
            inode_t cursor=(inode_t)((old+CLUSTER_SIZE-1)/CLUSTER_SIZE);
            if(!(inode->resv0&1u) || inode->i_parent<cursor) inode->i_parent=cursor;
            nvfuse_cleanup_add(sb,ctx);
        } else nvfuse_free_inode_size(sb,ctx,size);
    }
    inode->i_size=size;
    for(unsigned i=START_OPEN_FILE;i<MAX_OPEN_FILE;i++)
        if(sb->sb_file_table[i].used && sb->sb_file_table[i].ino==inode->i_ino)
            sb->sb_file_table[i].size=size;
    return 0;
}

int nvfuse_allocate_range(struct nvfuse_handle *h,const char *path,s64 start,s64 length) {
    if(start<0 || length<=0 || start>=MAX_FILE_SIZE || length>=MAX_FILE_SIZE-start) return -EINVAL;
    struct nvfuse_superblock *sb=&h->nvh_sb;
    char filename[FNAME_SIZE];struct nvfuse_dir_entry entry;
    int rc=nvfuse_path_resolve(h,path,filename,&entry);if(rc) return rc;
    struct nvfuse_inode_ctx *ctx=NULL;
    rc=nvfuse_lookup(sb,&ctx,&entry,filename,entry.d_ino);if(rc) return rc;
    inode_t ino=ctx->ictx_inode->i_ino;
    if(ctx->ictx_inode->i_type!=NVFUSE_TYPE_FILE) { nvfuse_release_inode(sb,ctx,NVF_CLEAN);return -EISDIR; }
    s64 end=start+length;
    if(ctx->ictx_inode->i_size<end) rc=nvfuse_resize_inode(sb,ctx,end);
    nvfuse_release_inode(sb,ctx,rc?NVF_CLEAN:DIRTY);if(rc) return rc;
    u32 first=(u32)(start/CLUSTER_SIZE),last=(u32)((end+CLUSTER_SIZE-1)/CLUSTER_SIZE);
    for(u32 block=first;block<last;block++) {
        ctx=nvfuse_read_inode(sb,NULL,ino);u32 physical=0;
        rc=nvfuse_get_block(sb,ctx,(s32)block,1,NULL,&physical,0);
        if(!rc && !physical) {
            rc=nvfuse_get_block(sb,ctx,(s32)block,1,NULL,NULL,1);
            if(!rc && !sb->target->metadata_only) {
                struct nvfuse_buffer_head *bh=nvfuse_get_bh(sb,ctx,ino,block,WRITE,NVFUSE_TYPE_DATA);
                if(!bh) rc=-ENOMEM;
                else { memset(bh->bh_buf,0,CLUSTER_SIZE);nvfuse_release_bh(sb,bh,0,DIRTY); }
            }
        }
        nvfuse_release_inode(sb,ctx,rc?NVF_CLEAN:DIRTY);if(rc) return rc;
        if((block-first)%16==15 || block+1==last) {
            nvfuse_check_flush_dirty(sb,DIRTY_FLUSH_FORCE);
            if(nvj_error(sb->target->journal)) return nvj_error(sb->target->journal);
        }
    }
    return 0;
}
