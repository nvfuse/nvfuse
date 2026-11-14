/* SPDX-License-Identifier: GPL-2.0-only */

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_buffer_cache.h"
#include "test_backend.h"
#include "fs_model_adapter.h"
#include <fcntl.h>
#include <unistd.h>
#include "nvfuse_tx.h"
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

static const char *model_crash_phase;
static void model_crash_hook(void *arg,const char *phase,uint64_t block) {
    (void)arg;(void)block;
    if(!strcmp(phase,model_crash_phase)) _exit(86);
}
int model_set_crash(void *h,const char *phase) {
    model_crash_phase=phase;return nvfuse_tx_set_hook(h,model_crash_hook,NULL);
}
void model_hash(const char *name,uint32_t hash[2]) { nvfuse_dir_hash((s8 *)name,hash,hash+1); }

int model_list(void *h,const char *path,char *names,unsigned capacity,unsigned *count) {
    struct stat st;if(nvfuse_getattr(h,path,&st))return -1;
    int ino=nvfuse_opendir(h,path);if(ino<0)return ino;
    unsigned n=(unsigned)st.st_nlink;if(n<2 || n-2>capacity)return -1;
    struct dirent entry;
    for(unsigned k=0;k<n;k++) {
        if(!nvfuse_readdir(h,ino,&entry,k))return -1;
        if(k<2) {
            if(strcmp(entry.d_name,k?"..":"."))return -1;
        } else strcpy(names+(k-2)*FNAME_SIZE,entry.d_name);
    }
    if(nvfuse_readdir(h,ino,&entry,n))return -1;
    *count=n-2;return 0;
}

int model_parent(void *h,const char *path,const char *parent) {
    int dir=nvfuse_opendir(h,path),expected=nvfuse_opendir(h,parent);
    if(dir<0 || expected<0)return -1;
    struct dirent e;if(!nvfuse_readdir(h,dir,&e,1) || strcmp(e.d_name,"..") || e.d_ino!=(ino_t)expected)return -1;
    char p[4097];if(snprintf(p,sizeof(p),"%s/..",path)>=(int)sizeof(p))return -1;
    return nvfuse_opendir(h,p)==expected?0:-1;
}
static int model_corrupt_parent(void *arg,const char *path,int clear) {
    struct nvfuse_handle *h=arg;int ino=nvfuse_opendir(h,path);if(ino<0)return ino;
    if(nvfuse_tx_begin(h,NVFUSE_TX_METADATA,0))return -1;
    struct nvfuse_inode_ctx *ctx=nvfuse_read_inode(&h->nvh_sb,NULL,ino);
    if(clear)ctx->ictx_inode->i_parent=0;
    if(ctx->ictx_inode->i_size) {
        struct nvfuse_buffer_head *bh=nvfuse_get_bh(&h->nvh_sb,ctx,ino,0,READ,NVFUSE_TYPE_META);
        ((struct nvfuse_dir_entry *)bh->bh_buf)[1].d_ino=ino;
        nvfuse_release_bh(&h->nvh_sb,bh,HEAD,DIRTY);
    }
    nvfuse_release_inode(&h->nvh_sb,ctx,DIRTY);
    return nvfuse_tx_commit(h);
}

int model_legacy_parent(void *h,const char *p) { return model_corrupt_parent(h,p,1); }
int model_legacy_dot(void *h,const char *p) { return model_corrupt_parent(h,p,0); }
