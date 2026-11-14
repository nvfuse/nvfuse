/* SPDX-License-Identifier: GPL-2.0-only */

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_tx.h"
#include "nvfuse_direct.h"
#include "nvfuse_indirect.h"
#include "nvfuse_xattr.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define LARGE (5u*1024*1024+123)
#define DATA_SIZE (24u*1024*1024+123)
#define CUT_SIZE (1024u*1024+37)
static const char *cut; static unsigned nth,prepares,transaction;
static void crash(void *unused,const char *phase,uint64_t block) {
    (void)unused;(void)block;
    if(!strcmp(phase,"prepare-flush") && !(prepares++%2)) transaction++;
    if(!strcmp(phase,cut) && transaction==nth) _exit(86);
}
static void put(struct nvfuse_handle *h,const char *path,size_t size,int pattern) {
    int fd=nvfuse_openfile_path(h,path,O_CREAT|O_RDWR,0644); CHECK(fd>=0);
    char data[65536]; memset(data,pattern,sizeof(data));
    for(size_t at=0;at<size;) { size_t n=size-at; if(n>sizeof(data)) n=sizeof(data);
        CHECK(nvfuse_writefile(h,fd,data,(u32)n,(s64)at)==(int)n); at+=n; }
    CHECK(!nvfuse_closefile(h,fd));
}
static void verify(struct nvfuse_handle *h,const char *path,size_t size,int pattern) {
    int fd=nvfuse_openfile_path(h,path,O_RDONLY,0); CHECK(fd>=0);
    char data[65536];
    for(size_t at=0;at<size;) { size_t n=size-at; if(n>sizeof(data)) n=sizeof(data);
        CHECK(nvfuse_readfile(h,fd,data,(s32)n,(s64)at)==(int)n);
        for(size_t k=0;k<n;k++) CHECK((unsigned char)data[k]==pattern); at+=n; }
    CHECK(nvfuse_readfile(h,fd,data,1,(s64)size)==0); CHECK(!nvfuse_closefile(h,fd));
}
static void sparse(struct nvfuse_handle *h) {
    int fd=nvfuse_openfile_path(h,"/sparse",O_CREAT|O_RDWR,0644); CHECK(fd>=0);
    const s64 offsets[]={17,(s64)DIRECT_BLOCKS*CLUSTER_SIZE+29,
        (s64)(DIRECT_BLOCKS+PTRS_PER_BLOCK)*CLUSTER_SIZE+41,
        (DIRECT_BLOCKS+PTRS_PER_BLOCK+(s64)PTRS_PER_BLOCK*PTRS_PER_BLOCK)*CLUSTER_SIZE+53,
        MAX_FILE_SIZE-2};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        char value=(char)('A'+i),b[100];
        CHECK(nvfuse_writefile(h,fd,&value,1,offsets[i])==1);
        CHECK(nvfuse_readfile(h,fd,b,1,offsets[i])==1 && b[0]==value);
        CHECK(nvfuse_readfile(h,fd,b,1,offsets[i]-1)==1 && b[0]==0);
        CHECK(nvfuse_readfile(h,fd,b,100,(i?offsets[i]-100:0))==(i?100:18));
        for(unsigned k=0;k<(i?100:17);k++) CHECK(b[k]==0);
    }
    CHECK(!nvfuse_ftruncate(h,fd,7)); CHECK(!nvfuse_ftruncate(h,fd,10000));
    char zeros[10000];memset(zeros,0x7f,sizeof(zeros));
    CHECK(nvfuse_readfile(h,fd,zeros,sizeof(zeros),0)==sizeof(zeros));
    for(unsigned i=0;i<sizeof(zeros);i++) CHECK(!zeros[i]);
    CHECK(nvfuse_writefile(h,fd,"secret",6,3)==6);
    CHECK(!nvfuse_ftruncate(h,fd,5)); CHECK(!nvfuse_ftruncate(h,fd,9));
    CHECK(nvfuse_readfile(h,fd,zeros,9,0)==9 && zeros[3]=='s' && zeros[4]=='e');
    for(unsigned i=5;i<9;i++) CHECK(!zeros[i]);
    CHECK(!nvfuse_closefile(h,fd));
    CHECK(!nvfuse_fallocate(h,"/sparse",45001,10000));
    fd=nvfuse_openfile_path(h,"/sparse",O_RDWR,0); CHECK(fd>=0);
    CHECK(nvfuse_readfile(h,fd,zeros,10000,45001)==10000);
    for(unsigned i=0;i<10000;i++) CHECK(!zeros[i]);
    CHECK(nvfuse_readfile(h,fd,zeros,9,0)==9 && zeros[3]=='s' && zeros[4]=='e');
    CHECK(!nvfuse_ftruncate(h,fd,9)); CHECK(!nvfuse_closefile(h,fd));
    CHECK(!nvfuse_tx_begin(h,NVFUSE_TX_DATA,64));
    fd=nvfuse_openfile_path(h,"/sparse",O_RDWR,0);CHECK(fd>=0);
    CHECK(!nvfuse_ftruncate(h,fd,MAX_FILE_SIZE-1));
    CHECK(nvfuse_writefile(h,fd,"Z",1,offsets[3])==1); CHECK(!nvfuse_tx_abort(h));
    struct stat st; CHECK(!nvfuse_getattr(h,"/sparse",&st) && st.st_size==9);
    fd=nvfuse_openfile_path(h,"/sparse",O_RDWR,0);CHECK(fd>=0);
    CHECK(nvfuse_writefile(h,fd,"Z",1,MAX_FILE_SIZE-2)==1);
    CHECK(!nvfuse_unlink(h,"/sparse")); CHECK(!nvfuse_closefile(h,fd));
}
static void xattrs(struct nvfuse_handle *h) {
    char buf[4096],name[257],value[513]; memset(name,'n',256);name[256]=0; memset(value,'v',512);value[512]=0;
    CHECK(!nvfuse_set_xattr(h,"/keep","user.test","hello"));
    CHECK(nvfuse_get_xattr(h,"/keep","user.test",NULL,0)==5);
    CHECK(nvfuse_get_xattr(h,"/keep","user.test",buf,5)==-ERANGE);
    CHECK(nvfuse_get_xattr(h,"/keep","user.test",buf,sizeof(buf))==5 && !strcmp(buf,"hello"));
    CHECK(!nvfuse_set_xattr(h,"/keep",name,value));
    CHECK(!nvfuse_set_xattr(h,"/keep","user.empty",""));
    CHECK(nvfuse_get_xattr(h,"/keep","user.empty",buf,1)==0 && !buf[0]);
    CHECK(nvfuse_list_xattr(h,"/keep",NULL,0)==10+257+11);
    CHECK(nvfuse_list_xattr(h,"/keep",buf,sizeof(buf))==278);
    CHECK(!strcmp(buf,"user.test") && !strcmp(buf+10,name) && !strcmp(buf+267,"user.empty"));
    CHECK(!nvfuse_tx_begin(h,NVFUSE_TX_METADATA,64));
    CHECK(nvfuse_get_xattr(h,"/keep","missing",buf,sizeof(buf))==-ENODATA);
    CHECK(nvfuse_tx_status(h)==NVFUSE_TX_ACTIVE);
    CHECK(!nvfuse_set_xattr(h,"/keep","user.test","aborted"));
    CHECK(!nvfuse_remove_xattr(h,"/keep",name)); CHECK(!nvfuse_tx_abort(h));
    CHECK(nvfuse_get_xattr(h,"/keep","user.test",buf,sizeof(buf))==5 && !strcmp(buf,"hello"));
    CHECK(nvfuse_get_xattr(h,"/keep",name,buf,sizeof(buf))==512 && !strcmp(buf,value));
    CHECK(!nvfuse_remove_xattr(h,"/keep",name)); CHECK(!nvfuse_remove_xattr(h,"/keep","user.empty"));
    /* Fill the inline area, then verify that failure preserves prior entries. */
    for(unsigned i=0;i<6;i++) { char key[32];snprintf(key,sizeof(key),"user.%u",i);CHECK(!nvfuse_set_xattr(h,"/keep",key,value)); }
    CHECK(!nvfuse_tx_begin(h,NVFUSE_TX_METADATA,64));
    CHECK(nvfuse_set_xattr(h,"/keep",name,value)==-ENOSPC); CHECK(!nvfuse_tx_abort(h));
    CHECK(nvfuse_get_xattr(h,"/keep","user.0",buf,sizeof(buf))==512 && !strcmp(buf,value));
    CHECK(!nvfuse_set_xattr(h,"/","user.root","root"));
}
static const unsigned char request_key[16]={0x51,0x20,0x91,0x74,1};
static void truncate_group(struct nvfuse_handle *h) {
    put(h,"/truncate-tx",8192,'S');
    for(unsigned commit=0;commit<2;commit++) {
        CHECK(!nvfuse_tx_begin(h,NVFUSE_TX_DATA,64));
        int fd=nvfuse_openfile_path(h,"/truncate-tx",O_RDWR,0);CHECK(fd>=0);
        CHECK(!nvfuse_ftruncate(h,fd,3)); CHECK(!nvfuse_ftruncate(h,fd,8192));
        CHECK(nvfuse_writefile(h,fd,"Z",1,4098)==1);
        char data[8192];CHECK(nvfuse_readfile(h,fd,data,sizeof(data),0)==sizeof(data));
        for(unsigned i=0;i<sizeof(data);i++) CHECK(data[i]==(i<3?'S':i==4098?'Z':0));
        CHECK(!nvfuse_closefile(h,fd));
        if(commit) CHECK(!nvfuse_tx_commit(h));
        else { CHECK(!nvfuse_tx_abort(h));verify(h,"/truncate-tx",8192,'S'); }
    }
    CHECK(!nvfuse_unlink(h,"/truncate-tx"));
}
struct io_failure { uint64_t offset; int write,remaining; };
static int fail_payload(void *arg,uint64_t offset,size_t bytes,int write) {
    struct io_failure *f=arg;(void)bytes;
    if(offset==f->offset && write==f->write && f->remaining) { f->remaining--;return -EIO; }
    return 0;
}
static void direct_io(struct nvfuse_handle *h) {
    void *data=NULL,*readback=NULL;CHECK(!posix_memalign(&data,CLUSTER_SIZE,8192));CHECK(!posix_memalign(&readback,CLUSTER_SIZE,8192));
    memset(data,'D',8192);
    int fd=nvfuse_openfile_path(h,"/direct",O_CREAT|O_RDWR,0644);CHECK(fd>=0);
    CHECK(nvfuse_pwrite_direct(h,fd,data,8192,0)==8192);
    char b[8192];CHECK(nvfuse_readfile(h,fd,b,sizeof(b),0)==sizeof(b));
    CHECK(!memcmp(data,b,8192));CHECK(nvfuse_writefile(h,fd,"B",1,11)==1);
    CHECK(nvfuse_pread_direct(h,fd,readback,8192,0)==8192 && ((char *)readback)[11]=='B');
    memset(data,'N',8192);CHECK(nvfuse_pwrite_direct(h,fd,data,4096,0)==4096);
    CHECK(nvfuse_readfile(h,fd,b,sizeof(b),0)==sizeof(b));
    for(unsigned i=0;i<sizeof(b);i++) CHECK(b[i]==(i<4096?'N':'D'));
    struct nvfuse_inode_ctx *ctx=nvfuse_read_inode(&h->nvh_sb,NULL,h->nvh_sb.sb_file_table[fd].ino);
    u32 physical=0;CHECK(!nvfuse_get_block(&h->nvh_sb,ctx,0,1,NULL,&physical,0) && physical);
    nvfuse_release_inode(&h->nvh_sb,ctx,NVF_CLEAN);
    struct io_failure fault={(uint64_t)physical*CLUSTER_SIZE,0,1};
    nvj_set_io_fault_hook(h->nvh_target->journal,fail_payload,&fault);
    CHECK(nvfuse_pread_direct(h,fd,readback,4096,0)==-EIO && !fault.remaining);
    CHECK(nvfuse_tx_status(h)==NVFUSE_TX_IDLE);
    CHECK(nvfuse_pread_direct(h,fd,readback,4096,0)==4096);
    /* Use an existing untouched data extent. Freed metadata blocks retain
     * conservative classification until close, so a recycled new file may be
     * protected as metadata and fail later during checkpoint instead. */
    int victim=nvfuse_openfile_path(h,"/victim",O_RDWR,0);CHECK(victim>=0);
    ctx=nvfuse_read_inode(&h->nvh_sb,NULL,h->nvh_sb.sb_file_table[victim].ino);
    CHECK(!nvfuse_get_block(&h->nvh_sb,ctx,512,1,NULL,&physical,0) && physical);
    nvfuse_release_inode(&h->nvh_sb,ctx,NVF_CLEAN);
    fault.offset=(uint64_t)physical*CLUSTER_SIZE;fault.write=1;fault.remaining=1;
    CHECK(nvfuse_pwrite_direct(h,victim,data,4096,512*CLUSTER_SIZE)==-EIO && !fault.remaining);
    CHECK(nvfuse_tx_status(h)==NVFUSE_TX_IDLE);
    CHECK(nvfuse_pread_direct(h,victim,readback,4096,512*CLUSTER_SIZE)==4096);
    for(unsigned i=0;i<4096;i++) CHECK(((char *)readback)[i]=='V');
    CHECK(!nvfuse_closefile(h,victim));
    nvj_set_io_fault_hook(h->nvh_target->journal,NULL,NULL);
    CHECK(nvfuse_pread_direct(h,fd,readback,4096,1)==-EINVAL);
    CHECK(!nvfuse_closefile(h,fd));
    CHECK(!nvfuse_tx_begin(h,NVFUSE_TX_DATA,64));
    fd=nvfuse_openfile_path(h,"/direct",O_RDWR,0);CHECK(fd>=0);memset(data,'Z',8192);
    CHECK(nvfuse_pwrite_direct(h,fd,data,8192,0)==8192);
    CHECK(nvfuse_readfile(h,fd,b,sizeof(b),0)==sizeof(b) && !memcmp(data,b,sizeof(b)));
    CHECK(!nvfuse_tx_abort(h));
    fd=nvfuse_openfile_path(h,"/direct",O_RDWR,0);CHECK(fd>=0);
    CHECK(nvfuse_pread_direct(h,fd,readback,8192,0)==8192);
    for(unsigned i=0;i<8192;i++) CHECK(((char *)readback)[i]==(i<4096?'N':'D'));
    CHECK(nvfuse_pwrite_direct(h,fd,data,4096,1024*1024)==4096);
    CHECK(nvfuse_pread_direct(h,fd,readback,4096,65536)==4096);
    for(unsigned i=0;i<4096;i++) CHECK(!((char *)readback)[i]);
    CHECK(!nvfuse_closefile(h,fd));CHECK(!nvfuse_unlink(h,"/direct"));free(readback);free(data);
}
static void receipts(struct nvfuse_handle *h) {
    enum nvfuse_tx_outcome state;unsigned char key[16]={0};
    CHECK(!nvfuse_tx_query_key(h,key,&state) && state==NVFUSE_TX_UNKNOWN);
    CHECK(!nvfuse_tx_begin_key(h,NVFUSE_TX_DATA,64,key));
    CHECK(!nvfuse_tx_query_key(h,key,&state) && state==NVFUSE_TX_IN_PROGRESS);
    CHECK(!nvfuse_tx_abort(h)); CHECK(!nvfuse_tx_query_key(h,key,&state) && state==NVFUSE_TX_UNKNOWN);
    for(unsigned i=0;i<127;i++) {
        key[0]=(unsigned char)i;
        CHECK(!nvfuse_tx_begin_key(h,NVFUSE_TX_METADATA,64,key));CHECK(!nvfuse_tx_commit(h));
        CHECK(!nvfuse_tx_query_key(h,key,&state) && state==NVFUSE_TX_COMMITTED);
        CHECK(nvfuse_tx_begin_key(h,NVFUSE_TX_METADATA,64,key)==-EALREADY);
    }
    key[0]=255;CHECK(nvfuse_tx_begin_key(h,NVFUSE_TX_METADATA,64,key)==-ENOSPC);
    key[0]=42;CHECK(!nvfuse_tx_forget_key(h,key));
    CHECK(!nvfuse_tx_query_key(h,key,&state) && state==NVFUSE_TX_UNKNOWN);
    key[0]=255;CHECK(!nvfuse_tx_begin_key(h,NVFUSE_TX_DATA,64,key));CHECK(!nvfuse_tx_commit(h));
    for(unsigned i=0;i<127;i++) { key[0]=(unsigned char)i;CHECK(!nvfuse_tx_forget_key(h,key)); }
    key[0]=255;CHECK(!nvfuse_tx_forget_key(h,key));
}
int main(int argc,char **argv) {
    CHECK(argc>=3); const char *mode=argv[1];
    struct nvfuse_handle *h=!strcmp(mode,"setup-32")?
        nvfuse_file_open_config(argv[2],1,128ULL*1024*1024,0,8192):!strcmp(mode,"large-setup")?
        nvfuse_file_open_config(argv[2],1,128ULL*1024*1024,0,16384):
        nvfuse_file_open(argv[2],!strcmp(mode,"setup"),128ULL*1024*1024);
    if(!strcmp(mode,"bad-open")) {
        CHECK(!h && errno==EIO);CHECK(argc==4);
        h=nvfuse_file_open(argv[3],0,0);CHECK(h);verify(h,"/victim",LARGE,'V');
        CHECK(!nvfuse_file_close(h));puts("PASS roadmap bad-open");return 0;
    }
    CHECK(h);
    if(!strcmp(mode,"setup-32")) { put(h,"/victim",LARGE,'V');put(h,"/keep",8192,'K'); }
    else if(!strcmp(mode,"legacy-check")) {
        CHECK(nvj_version(h->nvh_target->journal)==1);
        int fd=nvfuse_openfile_path(h,"/victim",O_RDWR,0);CHECK(fd>=0);
        CHECK(nvfuse_unlink(h,"/victim")==-EBUSY);
        CHECK(nvfuse_writefile(h,fd,"Z",1,LARGE+4096)==-EOPNOTSUPP);
        CHECK(nvfuse_ftruncate(h,fd,LARGE+4096)==-EOPNOTSUPP);CHECK(!nvfuse_closefile(h,fd));
        CHECK(nvfuse_tx_begin_key(h,NVFUSE_TX_DATA,64,request_key)==-EOPNOTSUPP);
        verify(h,"/victim",LARGE,'V');verify(h,"/keep",8192,'K');
    } else if(!strcmp(mode,"large-setup")) { put(h,"/victim",DATA_SIZE,'V');put(h,"/keep",8192,'K'); }
    else if(!strcmp(mode,"large-functional")) {
        CHECK(nvj_capacity(h->nvh_target->journal)>DATA_SIZE/CLUSTER_SIZE);
        CHECK(!nvfuse_tx_begin(h,NVFUSE_TX_DATA,0));put(h,"/victim",DATA_SIZE,'T');
        CHECK(!nvfuse_tx_abort(h));verify(h,"/victim",DATA_SIZE,'V');
        CHECK(!nvfuse_tx_begin(h,NVFUSE_TX_DATA,0));put(h,"/victim",DATA_SIZE,'T');
        CHECK(!nvfuse_tx_commit(h));verify(h,"/victim",DATA_SIZE,'T');
    } else if(!strcmp(mode,"large-crash")) {
        CHECK(argc==5);cut=argv[3];nth=(unsigned)atoi(argv[4]);
        CHECK(!nvfuse_tx_begin(h,NVFUSE_TX_DATA,0));put(h,"/victim",DATA_SIZE,'T');
        CHECK(!nvfuse_tx_set_hook(h,crash,NULL));CHECK(!nvfuse_tx_commit(h));CHECK(!"crash hook not reached");
    } else if(!strcmp(mode,"large-before") || !strcmp(mode,"large-after")) {
        verify(h,"/victim",DATA_SIZE,!strcmp(mode,"large-before")?'V':'T');verify(h,"/keep",8192,'K');
    } else if(!strcmp(mode,"setup")) { put(h,"/victim",LARGE,'V'); put(h,"/keep",8192,'K'); }
    else if(!strcmp(mode,"truncate-crash")) {
        CHECK(argc==5);cut=argv[3];nth=(unsigned)atoi(argv[4]);
        CHECK(!nvfuse_tx_set_hook(h,crash,NULL));
        CHECK(!nvfuse_truncate_path(h,"/victim",CUT_SIZE)); CHECK(!"crash hook not reached");
    } else if(!strcmp(mode,"truncate-before") || !strcmp(mode,"truncate-after")) {
        verify(h,"/victim",!strcmp(mode,"truncate-before")?LARGE:CUT_SIZE,'V');
        verify(h,"/keep",8192,'K');
        struct nvfuse_inode_ctx *root=nvfuse_read_inode(&h->nvh_sb,NULL,ROOT_INO);
        CHECK(!root->ictx_inode->resv2[0]);nvfuse_release_inode(&h->nvh_sb,root,NVF_CLEAN);
    } else if(!strcmp(mode,"orphan-crash")) {
        CHECK(argc==5); cut=argv[3];nth=(unsigned)atoi(argv[4]);
        int fd=nvfuse_openfile_path(h,"/victim",O_RDWR,0);CHECK(fd>=0);
        CHECK(!nvfuse_tx_set_hook(h,crash,NULL)); CHECK(!nvfuse_unlink(h,"/victim"));
        char b[4096]; CHECK(nvfuse_readfile(h,fd,b,sizeof(b),0)==sizeof(b));
        for(unsigned i=0;i<sizeof(b);i++) CHECK(b[i]=='V');
        CHECK(nvfuse_writefile(h,fd,"X",1,0)==1); CHECK(!nvfuse_closefile(h,fd));
        CHECK(!"crash hook not reached");
    } else if(!strcmp(mode,"before") || !strcmp(mode,"after")) {
        struct stat st; verify(h,"/keep",8192,'K');
        if(!strcmp(mode,"before")) verify(h,"/victim",LARGE,'V');
        else CHECK(nvfuse_getattr(h,"/victim",&st)==-ENOENT);
        struct nvfuse_inode_ctx *root=nvfuse_read_inode(&h->nvh_sb,NULL,ROOT_INO);
        CHECK(!root->ictx_inode->resv2[0]); nvfuse_release_inode(&h->nvh_sb,root,NVF_CLEAN);
    } else if(!strcmp(mode,"functional")) {
        sparse(h); direct_io(h); truncate_group(h); xattrs(h); receipts(h);
        /* Abort must preserve descriptors for previously committed orphans. */
        int fd=nvfuse_openfile_path(h,"/victim",O_RDWR,0);CHECK(fd>=0);
        CHECK(!nvfuse_unlink(h,"/victim"));
        CHECK(nvfuse_tx_begin(h,NVFUSE_TX_METADATA,0)==-EBUSY);
        CHECK(!nvfuse_closefile(h,fd));
    } else if(!strcmp(mode,"key-crash") || !strcmp(mode,"async-crash")) {
        if(!strcmp(mode,"async-crash")) CHECK(!nvfuse_tx_set_async_checkpoint(h,1));
        CHECK(argc==5);cut=argv[3];nth=(unsigned)atoi(argv[4]);
        CHECK(!nvfuse_tx_begin_key(h,NVFUSE_TX_DATA,64,request_key));
        put(h,"/keyed",8192,'T');
        CHECK(!nvfuse_tx_set_hook(h,crash,NULL)); CHECK(!nvfuse_tx_commit(h));
        (void)nvfuse_tx_status(h);
        CHECK(!"crash hook not reached");
    } else if(!strcmp(mode,"key-before") || !strcmp(mode,"key-after")) {
        enum nvfuse_tx_outcome state;CHECK(!nvfuse_tx_query_key(h,request_key,&state));
        int committed=!strcmp(mode,"key-after");
        CHECK(state==(committed?NVFUSE_TX_COMMITTED:NVFUSE_TX_UNKNOWN));
        if(committed) {
            verify(h,"/keyed",8192,'T');
            CHECK(nvfuse_tx_begin_key(h,NVFUSE_TX_DATA,64,request_key)==-EALREADY);
        } else { struct stat st;CHECK(nvfuse_getattr(h,"/keyed",&st)==-ENOENT); }
        verify(h,"/victim",LARGE,'V');verify(h,"/keep",8192,'K');
    } else if(!strcmp(mode,"xattr-check")) {
        char b[20]; CHECK(nvfuse_get_xattr(h,"/keep","user.test",b,sizeof(b))==5 && !strcmp(b,"hello"));
        CHECK(nvfuse_get_xattr(h,"/","user.root",b,sizeof(b))==4 && !strcmp(b,"root"));
        verify(h,"/keep",8192,'K');
    } else CHECK(!"invalid mode");
    CHECK(!nvfuse_file_close(h)); printf("PASS roadmap %s\n",mode);return 0;
}
