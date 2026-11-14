/* SPDX-License-Identifier: GPL-2.0-only */

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_tx.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "test_backend.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static void put(struct nvfuse_handle *h,const char *path,int value) {
    int fd=nvfuse_openfile_path(h,path,O_CREAT|O_RDWR,0644); CHECK(fd>=0);
    char bytes[8192]; memset(bytes,value,sizeof(bytes));
    CHECK(nvfuse_writefile(h,fd,bytes,sizeof(bytes),0)==sizeof(bytes));
    CHECK(nvfuse_closefile(h,fd)==0);
}
static void contents(struct nvfuse_handle *h,const char *path,int value) {
    int fd=nvfuse_openfile_path(h,path,O_RDONLY,0); CHECK(fd>=0);
    char bytes[8192]; CHECK(nvfuse_readfile(h,fd,bytes,sizeof(bytes),0)==sizeof(bytes));
    for(unsigned i=0;i<sizeof(bytes);i++) CHECK(bytes[i]==value);
    CHECK(nvfuse_closefile(h,fd)==0);
}
static void change(struct nvfuse_handle *h) {
    put(h,"/old",'B'); CHECK(nvfuse_rename_path(h,"/old","/new")==0); put(h,"/extra",'C');
}
#ifndef NVFUSE_SPDK_TRANSPORT
/* Independent on-disk accounting check, after the handle has been closed. */
static void accounting(const char *path) {
    int fd=open(path,O_RDONLY); CHECK(fd>=0);
    struct nvfuse_superblock_common sb;
    CHECK(pread(fd,&sb,sizeof(sb),0)==sizeof(sb));
    uint64_t blocks=0,inodes=0;
    for(unsigned bg=0;bg<sb.sb_bg_num;bg++) {
        struct nvfuse_bg_descriptor bd; unsigned char bits[NVJ_BLOCK];
        CHECK(pread(fd,&bd,sizeof(bd),((uint64_t)bg*NVJ_GROUP_BLOCKS+1)*NVJ_BLOCK)==sizeof(bd));
        CHECK(bd.bd_magic==NVFUSE_BD_MAGIC && bd.bd_id==bg);
        CHECK(pread(fd,bits,sizeof(bits),(uint64_t)bd.bd_dbitmap_start*NVJ_BLOCK)==sizeof(bits));
        unsigned free_blocks=0;
        for(unsigned i=0;i<bd.bd_max_blocks;i++) {
            int used=!!(bits[i/8] & (1u<<(i%8)));
            if(i<4100 || (!bg && i>=NVJ_START)) CHECK(used);
            free_blocks+=!used;
        }
        CHECK(free_blocks==bd.bd_free_blocks); blocks+=free_blocks;
        CHECK(pread(fd,bits,sizeof(bits),(uint64_t)bd.bd_ibitmap_start*NVJ_BLOCK)==sizeof(bits));
        unsigned free_inodes=0;
        for(unsigned i=0;i<bd.bd_max_inodes;i++) free_inodes+=!(bits[i/8] & (1u<<(i%8)));
        CHECK(free_inodes==bd.bd_free_inodes); inodes+=free_inodes;
    }
    CHECK(blocks==sb.sb_free_blocks && inodes==sb.sb_free_inodes); CHECK(close(fd)==0);
}
#endif
static void *wrong_thread(void *arg) {
    struct nvfuse_handle *h=arg; struct stat st;
    CHECK(nvfuse_tx_begin(h,NVFUSE_TX_METADATA,0)==-EPERM);
    CHECK(nvfuse_getattr(h,"/old",&st)==-EPERM);
    CHECK(nvfuse_file_close(h)==-EPERM);
    return NULL;
}
static const char *phase; static int nth;
static void crash(void *arg,const char *name,uint64_t block) {
    (void)arg; (void)block;
    if(!strcmp(name,phase) && !--nth) _exit(86);
}
int main(int argc,char **argv) {
    CHECK(argc>=3);
    struct timespec started,finished; clock_gettime(CLOCK_MONOTONIC,&started);
    struct nvfuse_handle *h=nvfuse_file_open(argv[2],!strcmp(argv[1],"setup"),128ULL*1024*1024); CHECK(h);
    clock_gettime(CLOCK_MONOTONIC,&finished);
    printf("MOUNT_MS %.3f\n",(finished.tv_sec-started.tv_sec)*1e3+(finished.tv_nsec-started.tv_nsec)*1e-6);
    if(!strcmp(argv[1],"setup")) put(h,"/old",'A');
    else if(!strcmp(argv[1],"old") || !strcmp(argv[1],"new")) {
        struct stat st;
        if(!strcmp(argv[1],"old")) { contents(h,"/old",'A'); CHECK(nvfuse_getattr(h,"/new",&st)<0); CHECK(nvfuse_getattr(h,"/extra",&st)<0); }
        else { contents(h,"/new",'B'); contents(h,"/extra",'C'); CHECK(nvfuse_getattr(h,"/old",&st)<0); }
    } else if(!strcmp(argv[1],"crash")) {
        CHECK(argc==5); phase=argv[3]; nth=atoi(argv[4]);
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_DATA,64)==0); change(h);
        CHECK(nvfuse_tx_set_hook(h,crash,NULL)==0);
        CHECK(nvfuse_tx_commit(h)==0); CHECK(!"hook did not fire");
    } else if(!strcmp(argv[1],"implicit-crash")) {
        CHECK(argc==5); phase=argv[3]; nth=atoi(argv[4]);
        CHECK(nvfuse_tx_set_hook(h,crash,NULL)==0);
        CHECK(nvfuse_rename_path(h,"/old","/new")==0); CHECK(!"hook did not fire");
    } else if(!strcmp(argv[1],"rename-old") || !strcmp(argv[1],"rename-new")) {
        struct stat st; int before=!strcmp(argv[1],"rename-old");
        contents(h,before?"/old":"/new",'A');
        CHECK(nvfuse_getattr(h,before?"/new":"/old",&st)<0);
    } else if(!strcmp(argv[1],"functional")) {
        /* Expected lookup misses are observations, not transaction failures. */
        struct stat lookup_st;
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_DATA,64)==0);
        CHECK(nvfuse_getattr(h,"/missing",&lookup_st)==-ENOENT);
        CHECK(nvfuse_openfile_path(h,"/missing",O_RDONLY,0)==-ENOENT);
        CHECK(nvfuse_opendir(h,"/missing")==-ENOENT);
        CHECK(nvfuse_tx_status(h)==NVFUSE_TX_ACTIVE);
        put(h,"/after-miss",'Q'); CHECK(nvfuse_tx_commit(h)==0);
        contents(h,"/after-miss",'Q'); CHECK(!nvfuse_unlink(h,"/after-miss"));
        /* A failed mutation still makes the transaction abort-only. */
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_DATA,64)==0);
        CHECK(nvfuse_unlink(h,"/missing")==-ENOENT);
        CHECK(nvfuse_tx_status(h)==NVFUSE_TX_ABORT_ONLY);
        CHECK(nvfuse_tx_commit(h)==-ENOENT); CHECK(nvfuse_tx_abort(h)==0);
        /* Repeated pure lookups must not issue durability barriers. */
        struct nvj_stats before_lookup,after_lookup;
        CHECK(!nvfuse_tx_get_stats(h,&before_lookup));
        for(unsigned i=0;i<100;i++) CHECK(!nvfuse_getattr(h,"/old",&lookup_st));
        CHECK(!nvfuse_tx_get_stats(h,&after_lookup));
        CHECK(before_lookup.flushes==after_lookup.flushes);
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,wrong_thread,h)); CHECK(!pthread_join(thread,NULL));
        int busy=nvfuse_openfile_path(h,"/old",O_RDONLY,0); CHECK(busy>=0);
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_METADATA,0)==-EBUSY); CHECK(!nvfuse_closefile(h,busy));
        busy=nvfuse_openfile_path(h,"/old",O_RDWR,0); CHECK(busy>=0);
        CHECK(!nvfuse_unlink(h,"/old"));
        char unlinked[8192]; CHECK(nvfuse_readfile(h,busy,unlinked,sizeof(unlinked),0)==sizeof(unlinked));
        for(unsigned i=0;i<sizeof(unlinked);i++) CHECK(unlinked[i]=='A');
        CHECK(nvfuse_writefile(h,busy,"Z",1,0)==1);
        CHECK(!nvfuse_closefile(h,busy)); put(h,"/old",'A');
        put(h,"/replace",'R');
        busy=nvfuse_openfile_path(h,"/replace",O_RDWR,0); CHECK(busy>=0);
        CHECK(!nvfuse_rename_path(h,"/old","/replace"));
        CHECK(nvfuse_readfile(h,busy,unlinked,sizeof(unlinked),0)==sizeof(unlinked));
        for(unsigned i=0;i<sizeof(unlinked);i++) CHECK(unlinked[i]=='R');
        contents(h,"/replace",'A'); CHECK(!nvfuse_closefile(h,busy));
        CHECK(!nvfuse_unlink(h,"/replace")); put(h,"/old",'A');
        CHECK(nvfuse_tx_begin(h,99,0)==-EINVAL);
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_DATA,NVJ_CAPACITY+1)==-EINVAL);
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_DATA,64)==0);
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_DATA,64)==-EBUSY);
        change(h); CHECK(nvfuse_sync(h)==-EBUSY);
        CHECK(nvfuse_tx_abort(h)==0); contents(h,"/old",'A');
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_METADATA,64)==0);
        put(h,"/old",'D'); CHECK(nvfuse_tx_abort(h)==0); contents(h,"/old",'D');
        put(h,"/old",'A');
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_DATA,1)==0);
        int fd=nvfuse_openfile_path(h,"/overflow",O_CREAT|O_RDWR,0644); CHECK(fd<0);
        CHECK(nvfuse_tx_status(h)==NVFUSE_TX_ABORT_ONLY);
        CHECK(nvfuse_tx_commit(h)==-E2BIG); CHECK(nvfuse_tx_abort(h)==0);
        struct stat st; CHECK(nvfuse_getattr(h,"/overflow",&st)<0);
        /* Exceed the physical log in one operation, including cache eviction.
         * The overflow must be abortable without a dropped staged metadata read. */
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_DATA,0)==0);
        fd=nvfuse_openfile_path(h,"/large",O_CREAT|O_RDWR,0644); CHECK(fd>=0);
        size_t huge_size=18u*1024*1024;
        char *huge=malloc(huge_size); CHECK(huge);
        /* A reused NVMe device may already contain a previous test pattern.
         * Fresh entropy prevents identical-image elision from defeating this test. */
        int random_fd=open("/dev/urandom",O_RDONLY); CHECK(random_fd>=0);
        size_t filled=0;
        while(filled<huge_size) { ssize_t n=read(random_fd,huge+filled,huge_size-filled); CHECK(n>0); filled+=(size_t)n; }
        CHECK(close(random_fd)==0);
        CHECK(nvfuse_writefile(h,fd,huge,huge_size,0)==-E2BIG); free(huge);
        CHECK(nvfuse_tx_status(h)==NVFUSE_TX_ABORT_ONLY);
        CHECK(nvfuse_tx_abort(h)==0); CHECK(nvfuse_getattr(h,"/large",&st)<0);
        contents(h,"/old",'A');
        /* Synchronous opens must use the transaction boundary, not legacy fsync. */
        fd=nvfuse_openfile_path(h,"/old",O_RDWR|O_SYNC,0); CHECK(fd>=0);
        CHECK(nvfuse_writefile(h,fd,"A",1,0)==1); CHECK(nvfuse_fsync(h,fd)==0);
        CHECK(nvfuse_closefile(h,fd)==0);
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_DATA,64)==0); change(h); CHECK(nvfuse_tx_commit(h)==0);
        contents(h,"/new",'B'); contents(h,"/extra",'C');
    } else CHECK(!"unknown mode");
    CHECK(nvfuse_file_close(h)==0);
#ifndef NVFUSE_SPDK_TRANSPORT
    accounting(argv[2]);
#endif
    printf("PASS transaction %s\n",argv[1]); return 0;
}
