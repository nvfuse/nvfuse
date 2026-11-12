/* Bounded parser for the existing inline string-xattr encoding. */
#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_tx_internal.h"
#include <string.h>
static uint32_t word(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void encode(unsigned char *p,uint32_t n) {
    for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(n>>(i*8));
}
/* op: get=0, list=1, set=2, remove=3. The public API accepts strings. */
int nvfuse_journal_xattr(struct nvfuse_handle *h,const char *path,const char *name,
                        const char *value,char *buf,size_t size,int op) {
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    struct nvfuse_inode_ctx *ctx=NULL;
    struct nvfuse_superblock *sb=&h->nvh_sb;
    size_t nl=0,vl=0;
    if(!path || (op!=1 && !name) || (op==2 && !value) || ((op<2) && !buf && size)) { rc=-EINVAL; goto done; }
    if(name) { nl=strnlen(name,257); if(!nl || nl>256) { rc=-EINVAL; goto done; } }
    if(value) { vl=strnlen(value,513); if(vl>512) { rc=-E2BIG; goto done; } }
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
        if(!n || n>256 || v>512 || len>capacity-at-4 ||
           data[at+8+n] || data[at+len-1] ||
           memchr(data+at+8,0,n) || memchr(data+at+9+n,0,v)) { rc=-EIO; goto done; }
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
            if(size<found_value+1) { rc=-ERANGE; goto done; }
            memcpy(buf,data+found+9+nl,found_value+1);
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
            memcpy(next+used+8,name,nl+1); memcpy(next+used+9+nl,value,vl+1);
        }
        memcpy(ctx->ictx_inode->xattr,next,sizeof(next)); rc=0;
        nvfuse_release_inode(sb,ctx,DIRTY); ctx=NULL;
    }
done:
    if(ctx) nvfuse_release_inode(sb,ctx,NVF_CLEAN);
    return op<2?nvfuse_tx_leave_lookup(h,rc):nvfuse_tx_leave(h,rc);
}
