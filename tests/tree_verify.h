/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_TEST_TREE_VERIFY_H
#define NVFUSE_TEST_TREE_VERIFY_H
/* Walk the API namespace, including lazy empty directories, and read every
 * regular-file byte. Test-specific byte oracles are checked separately. */
#include "nvfuse_api.h"
#include "nvfuse_buffer_cache.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Inspect stored parents before path lookup can backfill legacy metadata. */
static int test_cold_parents(struct nvfuse_handle *h,inode_t ino,inode_t parent,unsigned depth)
{
    if(depth>512)return -1;
    struct nvfuse_superblock *sb=&h->nvh_sb;
    struct nvfuse_inode_ctx *ctx=nvfuse_read_inode(sb,NULL,ino);if(!ctx)return -1;
    struct nvfuse_inode *inode=ctx->ictx_inode;
    if(inode->i_type!=NVFUSE_TYPE_DIRECTORY){nvfuse_release_inode(sb,ctx,NVF_CLEAN);return 0;}
    if(inode->i_parent!=parent && !(ino==sb->sb_root_ino && !inode->i_parent)) {
        fprintf(stderr,"COLD PARENT FAIL inode=%u stored=%u expected=%u\n",ino,inode->i_parent,parent);
        nvfuse_release_inode(sb,ctx,NVF_CLEAN);return -1;
    }
    unsigned count=inode->i_links_count;
    inode_t *children=calloc(count,sizeof(*children));if(!children){nvfuse_release_inode(sb,ctx,NVF_CLEAN);return -1;}
    unsigned used=0;int rc=0;
    for(unsigned blk=0;blk<(unsigned)NVFUSE_SIZE_TO_BLK(inode->i_size);blk++) {
        struct nvfuse_buffer_head *bh=nvfuse_get_bh(sb,ctx,ino,blk,READ,NVFUSE_TYPE_META);
        if(!bh){rc=-1;break;}
        struct nvfuse_dir_entry *e=(struct nvfuse_dir_entry *)bh->bh_buf;
        if(!blk && (e[0].d_ino!=ino || e[1].d_ino!=parent))rc=-1;
        for(unsigned k=0;k<DIR_ENTRY_NUM;k++)if(e[k].d_flag==DIR_USED && (blk || k>1)) {
            if(used>=count)rc=-1;else children[used++]=e[k].d_ino;
        }
        nvfuse_release_bh(sb,bh,HEAD,NVF_CLEAN);
    }
    nvfuse_release_inode(sb,ctx,NVF_CLEAN);
    for(unsigned k=0;!rc && k<used;k++)rc=test_cold_parents(h,children[k],ino,depth+1);
    free(children);return rc;
}
static int test_walk_tree(struct nvfuse_handle *h, const char *path, inode_t parent,
                          unsigned depth, uint64_t *files, uint64_t *directories)
{
#define WALK_CHECK(x) do { if (!(x)) { fprintf(stderr,"TREE FAIL %s line=%d: %s\n",path,__LINE__,#x); return -1; } } while(0)
    WALK_CHECK(depth <= 512);
    int ino=nvfuse_opendir(h,path);WALK_CHECK(ino>=ROOT_INO);
    struct stat st;WALK_CHECK(!nvfuse_getattr(h,path,&st) && S_ISDIR(st.st_mode));
    unsigned count=(unsigned)st.st_nlink;WALK_CHECK(count>=2 && count<=MAX_FILES_PER_DIR);
    struct dirent *entries=calloc(count,sizeof(*entries));WALK_CHECK(entries);
    for(unsigned k=0;k<count;k++) {
        WALK_CHECK(nvfuse_readdir(h,ino,entries+k,k));
        WALK_CHECK(entries[k].d_name[0] && !strchr(entries[k].d_name,'/'));
        for(unsigned j=0;j<k;j++)WALK_CHECK(strcmp(entries[j].d_name,entries[k].d_name));
    }
    struct dirent end;WALK_CHECK(!nvfuse_readdir(h,ino,&end,count));
    WALK_CHECK(!strcmp(entries[0].d_name,".") && entries[0].d_ino==(ino_t)ino && entries[0].d_type==DT_DIR);
    WALK_CHECK(!strcmp(entries[1].d_name,"..") && entries[1].d_ino==(ino_t)parent && entries[1].d_type==DT_DIR);
    char child[4097];
    WALK_CHECK(snprintf(child,sizeof(child),"%s%s..",path,strcmp(path,"/")?"/":"")<(int)sizeof(child));
    WALK_CHECK(nvfuse_opendir(h,child)==(int)parent);
    (*directories)++;
    for(unsigned k=2;k<count;k++) {
        WALK_CHECK(snprintf(child,sizeof(child),"%s%s%s",path,strcmp(path,"/")?"/":"",entries[k].d_name)<(int)sizeof(child));
        WALK_CHECK(!nvfuse_getattr(h,child,&st) && st.st_ino==entries[k].d_ino);
        if(S_ISDIR(st.st_mode)) {
            WALK_CHECK(entries[k].d_type==DT_DIR);
            WALK_CHECK(!test_walk_tree(h,child,ino,depth+1,files,directories));
        } else {
            WALK_CHECK(S_ISREG(st.st_mode) && entries[k].d_type==DT_REG);
            int fd=nvfuse_openfile_path(h,child,O_RDONLY,0);WALK_CHECK(fd>=0);
            char bytes[65536];
            for(uint64_t offset=0;offset<(uint64_t)st.st_size;) {
                unsigned n=(uint64_t)st.st_size-offset>sizeof(bytes)?sizeof(bytes):(unsigned)((uint64_t)st.st_size-offset);
                WALK_CHECK(nvfuse_readfile(h,fd,bytes,n,offset)==(int)n);offset+=n;
            }
            WALK_CHECK(nvfuse_readfile(h,fd,bytes,1,st.st_size)==0);
            WALK_CHECK(!nvfuse_closefile(h,fd));(*files)++;
        }
    }
    free(entries);return 0;
#undef WALK_CHECK
}
static int test_verify_tree(struct nvfuse_handle *h)
{
    uint64_t files=0,dirs=0;
    if(test_cold_parents(h,nvfuse_get_root_ino(h),nvfuse_get_root_ino(h),0))return -1;
    if(test_walk_tree(h,"/",nvfuse_get_root_ino(h),0,&files,&dirs))return -1;
    printf("PASS readdir tree files=%llu directories=%llu; every listed file opened and read\n",
           (unsigned long long)files,(unsigned long long)dirs);
    return 0;
}
#endif
