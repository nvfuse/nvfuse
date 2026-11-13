/* Bounded parser for the existing inline length-prefixed xattr encoding. */
#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_tx_internal.h"
#include "nvfuse_xattr.h"
#include <string.h>
static uint32_t word(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void encode(unsigned char *p,uint32_t n) {
    for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(n>>(i*8));
}
/* Retain the legacy record layout (including its zero padding byte). */
static int xattr(struct nvfuse_handle *h,const char *path,const char *name,
                 const void *value,size_t vl,char *buf,size_t size,int op,
                 unsigned flags,int strings) {
    if(!h || !h->nvh_target || !h->nvh_target->journal) return -EOPNOTSUPP;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    struct nvfuse_inode_ctx *ctx=NULL;
    struct nvfuse_superblock *sb=&h->nvh_sb;
    size_t nl=0;
    if(!path || (op!=1 && !name) || (op==2 && !value && vl) || ((op<2) && !buf && size)) { rc=-EINVAL; goto done; }
    if(name) { nl=strnlen(name,257); if(!nl || nl>256) { rc=-EINVAL; goto done; } }
    if(flags & ~(NVFUSE_XATTR_CREATE | NVFUSE_XATTR_REPLACE) ||
       flags == (NVFUSE_XATTR_CREATE | NVFUSE_XATTR_REPLACE)) { rc=-EINVAL; goto done; }
    if(vl>sizeof(((struct nvfuse_inode *)0)->xattr)-14) { rc=-E2BIG; goto done; }
    if(!strcmp(path,"/")) ctx=nvfuse_read_inode(sb,NULL,sb->sb_root_ino);
    else {
        char filename[FNAME_SIZE]; struct nvfuse_dir_entry entry;
        rc=nvfuse_path_resolve(h,path,filename,&entry); if(rc) goto done;
        rc=nvfuse_lookup(sb,&ctx,&entry,filename,entry.d_ino); if(rc) goto done;
    }
    if(!ctx) { rc=-EIO; goto done; }
    const unsigned char *data=ctx->ictx_inode->xattr;
    const size_t capacity=sizeof(ctx->ictx_inode->xattr);
    size_t at=0,found=capacity,found_len=0,found_value=0,list_size=0;
    /* Validate every record before copying to user memory or modifying inode. */
    while(at+4<=capacity && word(data+at)) {
        if(at+8>capacity) { rc=-EIO; goto done; }
        uint32_t n=word(data+at),v=word(data+at+4);
        size_t len=8+(size_t)n+1+(size_t)v+1;
        if(!n || n>256 || v>capacity || len>capacity-at-4 ||
           data[at+8+n] || data[at+len-1] ||
           memchr(data+at+8,0,n)) { rc=-EIO; goto done; }
        if(name && n==nl && !memcmp(data+at+8,name,nl)) {
            if(found!=capacity) { rc=-EIO; goto done; }
            found=at; found_len=len; found_value=v;
        }
        list_size+=n+1; at+=len;
    }
    if(at+4>capacity) { rc=-EIO; goto done; }
    if(op==0) {
        if(found==capacity) { rc=-ENODATA; goto done; }
        rc=(int)found_value;
        if(size) {
            if(size<found_value+(strings?1:0)) { rc=-ERANGE; goto done; }
            memcpy(buf,data+found+9+nl,found_value);
            if(strings) buf[found_value]=0;
        }
    } else if(op==1) {
        rc=(int)list_size;
        if(size) {
            if(size<list_size) { rc=-ERANGE; goto done; }
            size_t written=0;
            for(size_t i=0;i<at;) {
                uint32_t n=word(data+i),v=word(data+i+4);
                memcpy(buf+written,data+i+8,n+1); written+=n+1; i+=8+n+1+v+1;
            }
        }
    } else {
        if(op==2 && (flags&NVFUSE_XATTR_CREATE) && found!=capacity) { rc=-EEXIST; goto done; }
        if(op==2 && (flags&NVFUSE_XATTR_REPLACE) && found==capacity) { rc=-ENODATA; goto done; }
        if(op==3 && found==capacity) { rc=-ENODATA; goto done; }
        size_t extra=op==2?8+nl+1+vl+1:0;
        if(at-found_len+extra+4>capacity) { rc=-ENOSPC; goto done; }
        unsigned char next[sizeof(ctx->ictx_inode->xattr)]; memset(next,0,sizeof(next));
        size_t used=at;
        if(found!=capacity) {
            memcpy(next,data,found); memcpy(next+found,data+found+found_len,at-found-found_len);
            used-=found_len;
        } else memcpy(next,data,at);
        if(op==2) {
            encode(next+used,(uint32_t)nl); encode(next+used+4,(uint32_t)vl);
            memcpy(next+used+8,name,nl+1);
            if(vl) memcpy(next+used+9+nl,value,vl);
        }
        memcpy(ctx->ictx_inode->xattr,next,sizeof(next)); rc=0;
        nvfuse_release_inode(sb,ctx,DIRTY); ctx=NULL;
    }
done:
    if(ctx) nvfuse_release_inode(sb,ctx,NVF_CLEAN);
    return op<2?nvfuse_tx_leave_lookup(h,rc):nvfuse_tx_leave(h,rc);
}

/* Compatibility API: legacy string reads still require room for a NUL. */
int nvfuse_journal_xattr(struct nvfuse_handle *h,const char *path,const char *name,
                        const char *value,char *buf,size_t size,int op) {
    size_t bytes=value?strnlen(value,513):0;
    if(op==2 && !value) return -EINVAL;
    if(bytes>512) return -E2BIG;
    return xattr(h,path,name,value,bytes,buf,size,op,0,1);
}
int nvfuse_setxattr(struct nvfuse_handle *h,const char *path,const char *name,
                    const void *value,size_t size,unsigned flags) {
    return xattr(h,path,name,value,size,NULL,0,2,flags,0);
}
int nvfuse_getxattr(struct nvfuse_handle *h,const char *path,const char *name,
                    void *value,size_t size) {
    return xattr(h,path,name,NULL,0,value,size,0,0,0);
}
int nvfuse_listxattr(struct nvfuse_handle *h,const char *path,char *list,size_t size) {
    return xattr(h,path,NULL,NULL,0,list,size,1,0,0);
}
int nvfuse_removexattr(struct nvfuse_handle *h,const char *path,const char *name) {
    return xattr(h,path,name,NULL,0,NULL,0,3,0,0);
}
