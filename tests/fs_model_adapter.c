#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "test_backend.h"
#include "fs_model_adapter.h"
#include <fcntl.h>
/* Oracle inspection bypasses automatic TX wrappers: a negative getattr would
 * otherwise reload the entire image once for every absent model name. */
extern s32 nvfuse_getattr_impl(struct nvfuse_handle *,const char *,struct stat *);
extern s32 nvfuse_openfile_path_impl(struct nvfuse_handle *,const char *,int,int);
extern s32 nvfuse_readfile_impl(struct nvfuse_handle *,u32,s8 *,s32,nvfuse_off_t);
extern s32 nvfuse_closefile_impl(struct nvfuse_handle *,s32);
void *model_open(const char *p,int fresh,int meta) { return nvfuse_file_open_mode(p,fresh,128ULL*1024*1024,meta); }
int model_close(void *h) { return nvfuse_file_close(h); }
int model_mkdir(void *h,const char *p) { return nvfuse_mkdir_path(h,p,0755); }
int model_rmdir(void *h,const char *p) { return nvfuse_rmdir_path(h,p); }
int model_create(void *h,const char *p) {
    int fd=nvfuse_openfile_path(h,p,O_CREAT|O_EXCL|O_RDWR,0644);
    return fd<0?fd:nvfuse_closefile(h,fd);
}
int model_unlink(void *h,const char *p) { return nvfuse_rmfile_path(h,p); }
int model_rename(void *h,const char *a,const char *b) { return nvfuse_rename_path(h,a,b); }
int model_truncate(void *h,const char *p,uint64_t n) { return nvfuse_truncate_path(h,p,n); }
int model_write(void *h,const char *p,uint64_t o,void *b,unsigned n) {
    int fd=nvfuse_openfile_path(h,p,O_RDWR,0); if(fd<0) return fd;
    int rc=nvfuse_writefile(h,fd,b,n,o),close_rc=nvfuse_closefile(h,fd);
    return close_rc?close_rc:rc;
}
int model_read(void *h,const char *p,void *b,unsigned n) {
    int fd=nvfuse_openfile_path_impl(h,p,O_RDONLY,0); if(fd<0) return fd;
    int rc=nvfuse_readfile_impl(h,fd,b,n,0),close_rc=nvfuse_closefile_impl(h,fd);
    return close_rc?close_rc:rc;
}
int model_stat(void *h,const char *p,uint64_t *size,unsigned *links) {
    struct stat st; int rc=nvfuse_getattr_impl(h,p,&st);
    if(!rc) { *size=st.st_size; *links=(unsigned)st.st_nlink; }
    return rc;
}
int model_counts(void *h,uint64_t *blocks,uint64_t *inodes) {
    struct statvfs st; int rc=nvfuse_statvfs(h,"/",&st);
    if(!rc) { *blocks=st.f_bfree; *inodes=st.f_ffree; } return rc;
}
int model_directory_limit(void *arg) {
    struct nvfuse_handle *h=arg;
    struct nvfuse_superblock *sb=&h->nvh_sb;
    struct nvfuse_inode_ctx *ctx=nvfuse_read_inode(sb,NULL,ROOT_INO);
    uint32_t links=ctx->ictx_inode->i_links_count,ptr=ctx->ictx_inode->i_ptr;
    nvfuse_release_inode(sb,ctx,NVF_CLEAN);
    extern s32 nvfuse_createfile_impl(struct nvfuse_superblock *,inode_t,s8 *,inode_t *,mode_t,dev_t);
    extern s32 nvfuse_mkdir_impl(struct nvfuse_superblock *,inode_t,const s8 *,inode_t *,mode_t);
    const uint32_t limits[]={MAX_FILES_PER_DIR,MAX_FILES_PER_DIR+1u,UINT32_MAX};
    int valid=1;
    for(unsigned n=0;n<sizeof(limits)/sizeof(limits[0]);n++) {
        ctx=nvfuse_read_inode(sb,NULL,ROOT_INO);
        ctx->ictx_inode->i_links_count=limits[n];ctx->ictx_inode->i_ptr=limits[n]-1;
        nvfuse_release_inode(sb,ctx,NVF_CLEAN);
        /* Direct core guards keep the fabricated count out of an implicit
         * transaction and out of durable home images. */
        int file_rc=nvfuse_createfile_impl(sb,ROOT_INO,"forbidden",NULL,0644,0);
        int dir_rc=nvfuse_mkdir_impl(sb,ROOT_INO,"forbidden",NULL,0755);
        ctx=nvfuse_read_inode(sb,NULL,ROOT_INO);
        valid &= file_rc==-ENOSPC && dir_rc==-ENOSPC &&
            ctx->ictx_inode->i_links_count==limits[n] && ctx->ictx_inode->i_ptr==limits[n]-1;
        ctx->ictx_inode->i_links_count=links;ctx->ictx_inode->i_ptr=ptr;
        nvfuse_release_inode(sb,ctx,NVF_CLEAN);
    }
    return valid?0:-1;
}
