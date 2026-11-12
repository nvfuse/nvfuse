#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_indirect.h"
#include "nvfuse_tx_internal.h"
#include "nvfuse_direct.h"
#include <limits.h>
#include <stdint.h>
static int direct(struct nvfuse_handle *h,unsigned fd,void *buffer,unsigned count,int64_t offset,int write) {
    int rc=nvfuse_tx_enter(h);if(rc) return rc;
    struct io_target *t=h->nvh_target;struct nvfuse_superblock *sb=&h->nvh_sb;
    if(!t->journal || t->metadata_only) return nvfuse_tx_leave(h,-EOPNOTSUPP);
    if(fd<START_OPEN_FILE || fd>=MAX_OPEN_FILE || !sb->sb_file_table[fd].used) return nvfuse_tx_leave(h,-EBADF);
    if(offset<0 || offset>=MAX_FILE_SIZE || count>INT_MAX || count>=MAX_FILE_SIZE-offset ||
       (count && !buffer) || ((uintptr_t)buffer&(CLUSTER_SIZE-1)) ||
       (count&(CLUSTER_SIZE-1)) || (offset&(CLUSTER_SIZE-1))) return nvfuse_tx_leave(h,-EINVAL);
    if(write && nvj_version(t->journal)<2 && offset>sb->sb_file_table[fd].size)
        return nvfuse_tx_leave(h,-EOPNOTSUPP);
    if(write && nvj_full_data(t->journal) && count/CLUSTER_SIZE>=nvj_budget(t->journal))
        return nvfuse_tx_leave(h,-E2BIG);
    nvfuse_check_flush_dirty(sb,DIRTY_FLUSH_FORCE);
    if(nvj_error(t->journal)) return nvfuse_tx_leave(h,nvj_error(t->journal));
    struct nvfuse_file_table *file=&sb->sb_file_table[fd];
    if(write && count) nvfuse_invalidate_inode_suffix(sb,file->ino,(u32)(offset/CLUSTER_SIZE));
    unsigned done=0;
    while(done<count) {
        struct nvfuse_inode_ctx *ctx=nvfuse_read_inode(sb,NULL,file->ino);
        if(!ctx) { rc=-EIO;break; }
        struct nvfuse_inode *inode=ctx->ictx_inode;
        if(write) { rc=nvfuse_finish_inode_suffix(sb,ctx);
            if(rc) { nvfuse_release_inode(sb,ctx,NVF_CLEAN);break; }
        }
        if(!write && offset+done>=inode->i_size) { nvfuse_release_inode(sb,ctx,NVF_CLEAN);break; }
        u32 physical=0,logical=(u32)((offset+done)/CLUSTER_SIZE);
        rc=nvfuse_get_block(sb,ctx,(s32)logical,1,NULL,&physical,write?1:0);
        unsigned n=CLUSTER_SIZE;
        if(!write && n>inode->i_size-offset-done) n=(unsigned)(inode->i_size-offset-done);
        if(!rc) {
            if(!physical && !write) memset((char *)buffer+done,0,n);
            else if(!physical) rc=-EIO;
            else rc=nvj_transfer(t->journal,(uint64_t)physical*CLUSTER_SIZE,n,(char *)buffer+done,write);
        }
        if(!rc && write && inode->i_size<offset+done+n) {
            inode->i_size=offset+done+n;file->size=inode->i_size;
        }
        nvfuse_release_inode(sb,ctx,write?DIRTY:NVF_CLEAN);
        if(rc) break;
        done+=n;
        if(write && done%(16*CLUSTER_SIZE)==0) {
            nvfuse_check_flush_dirty(sb,DIRTY_FLUSH_FORCE);
            rc=nvj_error(t->journal);if(rc) break;
        }
    }
    if(!rc) file->rwoffset=offset+done;
    return nvfuse_tx_leave(h,rc?rc:(int)done);
}
int nvfuse_pread_direct(struct nvfuse_handle *h,unsigned fd,void *buf,unsigned count,int64_t offset) {
    return direct(h,fd,buf,count,offset,0);
}
int nvfuse_pwrite_direct(struct nvfuse_handle *h,unsigned fd,const void *buf,unsigned count,int64_t offset) {
    return direct(h,fd,(void *)buf,count,offset,1);
}
