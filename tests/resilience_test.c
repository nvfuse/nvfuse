/* Stored payload durability and supported concurrency, using ordinary APIs. */
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_tx.h"
#include "test_backend.h"
#include "tree_verify.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef NVFUSE_SPDK_TRANSPORT
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
#define BASE_SIZE (20u*1024*1024+123)
#define CUT_SIZE ((11u+1024)*4096+13)
static unsigned char pattern(uint64_t n,int seed) { return (unsigned char)(1+(n*37+n/4096*11+(unsigned)seed)%251); }
static void write_pattern(struct nvfuse_handle *h,const char *path,uint64_t offset,unsigned count,int seed) {
    int fd=nvfuse_openfile_path(h,path,O_CREAT|O_RDWR,0644);CHECK(fd>=0);
    unsigned char *bytes=malloc(1024*1024);CHECK(bytes);
    for(unsigned done=0;done<count;) {
        unsigned n=count-done;if(n>1024*1024)n=1024*1024;
        for(unsigned k=0;k<n;k++)bytes[k]=pattern(offset+done+k,seed);
        CHECK(nvfuse_writefile(h,fd,(char *)bytes,n,offset+done)==(int)n);done+=n;
    }
    free(bytes);CHECK(!nvfuse_closefile(h,fd));
}
static void contents(struct nvfuse_handle *h,const char *path,uint64_t size,int appended,int seed) {
    struct stat st;CHECK(!nvfuse_getattr(h,path,&st) && (uint64_t)st.st_size==size);
    int fd=nvfuse_openfile_path(h,path,O_RDONLY,0);CHECK(fd>=0);
    unsigned char bytes[65536];
    for(uint64_t offset=0;offset<size;) {
        unsigned n=size-offset>sizeof(bytes)?sizeof(bytes):(unsigned)(size-offset);
        CHECK(nvfuse_readfile(h,fd,(char *)bytes,n,offset)==(int)n);
        for(unsigned k=0;k<n;k++) {
            uint64_t p=offset+k;int expected_seed=appended && p>=BASE_SIZE?29:seed;
            CHECK(bytes[k]==pattern(p,expected_seed));
        }
        offset+=n;
    }
    CHECK(nvfuse_readfile(h,fd,(char *)bytes,1,size)==0);CHECK(!nvfuse_closefile(h,fd));
}
static const char *phase;
static void crash(void *arg,const char *name,uint64_t block) {
    (void)arg;(void)block;if(!strcmp(name,phase)) {
        if(getenv("NVFUSE_VM_CUT")) {
            printf("READY VM cut phase=%s\n",phase);fflush(stdout);
            for(;;)pause();
        }
        _exit(86);
    }
}
static void check_data(struct nvfuse_handle *h,const char *op,int after) {
    struct stat st;
    if(!strcmp(op,"move") || !strcmp(op,"move-empty")) {
        const char *source=!strcmp(op,"move")?"/arena/moving":"/arena/vacant";
        const char *dest=!strcmp(op,"move")?"/other/moved":"/other/vacant";
        CHECK(!nvfuse_getattr(h,after?dest:source,&st) && S_ISDIR(st.st_mode));
        CHECK(nvfuse_getattr(h,after?source:dest,&st)<0);
        if(!strcmp(op,"move"))contents(h,after?"/other/moved/leaf":"/arena/moving/leaf",4096,0,97);
        contents(h,"/arena/old",BASE_SIZE,0,7);contents(h,"/other/new",4096,0,61);
    } else if(!strcmp(op,"mkdir")) {
        int rc=nvfuse_getattr(h,"/arena/brandnew",&st);CHECK(after?rc==0 && S_ISDIR(st.st_mode):rc<0);
        contents(h,"/arena/old",BASE_SIZE,0,7);
    } else if(!strcmp(op,"rename")) {
        if(after) {CHECK(nvfuse_getattr(h,"/arena/old",&st)<0);contents(h,"/other/new",BASE_SIZE,0,7);}
        else {contents(h,"/arena/old",BASE_SIZE,0,7);contents(h,"/other/new",4096,0,61);}
    } else {
        uint64_t size=after && !strcmp(op,"append")?BASE_SIZE+8192:after && !strcmp(op,"truncate")?CUT_SIZE:BASE_SIZE;
        contents(h,"/arena/old",size,after && !strcmp(op,"append"),7);contents(h,"/other/new",4096,0,61);
    }
}
struct worker {struct nvfuse_handle *h;atomic_int *ready,*go;unsigned rounds;};
static void *foreign(void *arg) {
    struct worker *w=arg;atomic_fetch_add(w->ready,1);while(!atomic_load(w->go))sched_yield();
    for(unsigned k=0;k<w->rounds;k++) {
        struct stat st;struct statvfs sv;
        CHECK(nvfuse_getattr(w->h,"/arena/old",&st)==-EPERM);
        CHECK(nvfuse_mkdir_path(w->h,"/forbidden",0755)==-EPERM);
        CHECK(nvfuse_statvfs(w->h,"/",&sv)==-EPERM);
        CHECK(nvfuse_tx_begin(w->h,NVFUSE_TX_METADATA,0)==-EPERM);
        CHECK(nvfuse_tx_abort(w->h)==-EPERM);
        CHECK(nvfuse_file_close(w->h)==-EPERM);
    }
    return NULL;
}
int main(int argc,char **argv) {
    CHECK(argc>=3);const char *mode=argv[1];int fresh=!strcmp(mode,"setup");
#ifndef NVFUSE_SPDK_TRANSPORT
    if(!strcmp(mode,"lock-probe")) {
        errno=0;struct nvfuse_handle *probe=nvfuse_file_open(argv[2],0,0);
        CHECK(!probe && (errno==EWOULDBLOCK || errno==EAGAIN));
        puts("PASS competing process refused by image lock");return 0;
    }
#endif
    struct nvfuse_handle *h=nvfuse_file_open(argv[2],fresh,128ULL*1024*1024);CHECK(h);
    if(fresh) {
        CHECK(!nvfuse_mkdir_path(h,"/arena",0755));CHECK(!nvfuse_mkdir_path(h,"/other",0755));
        write_pattern(h,"/arena/old",0,BASE_SIZE,7);write_pattern(h,"/other/new",0,4096,61);
        CHECK(!nvfuse_mkdir_path(h,"/arena/moving",0755));CHECK(!nvfuse_mkdir_path(h,"/arena/moving/empty",0755));
        write_pattern(h,"/arena/moving/leaf",0,4096,97);CHECK(!nvfuse_mkdir_path(h,"/arena/vacant",0755));
        CHECK(!test_verify_tree(h));
    } else if(!strcmp(mode,"pressure")) {
        struct statvfs sv;CHECK(!nvfuse_statvfs(h,"/",&sv));
        uint64_t budget=sv.f_bfree*sv.f_bsize;
        CHECK(budget>12u*1024*1024);
        uint64_t count=(budget-8u*1024*1024)/4096*4096;
        CHECK(count<UINT32_MAX);write_pattern(h,"/pressure",0,(unsigned)count,83);
        CHECK(!nvfuse_statvfs(h,"/",&sv));
        CHECK(sv.f_bfree*sv.f_bsize<=8u*1024*1024 && sv.f_bfree>0);
        printf("PASS near-full pressure free_bytes=%llu\n",(unsigned long long)sv.f_bfree*sv.f_bsize);
    } else if(!strcmp(mode,"data-crash")) {
        CHECK(argc==5);const char *op=argv[3];phase=argv[4];
        int fd=nvfuse_openfile_path(h,"/arena/old",O_RDWR,0);CHECK(fd>=0);
        int barrier=!strcmp(phase,"before-fsync") || !strcmp(phase,"after-fsync");
        if(!barrier)CHECK(!nvfuse_tx_set_hook(h,crash,NULL));
        if(!strcmp(op,"append")) {
            char bytes[8192];for(unsigned k=0;k<sizeof(bytes);k++)bytes[k]=(char)pattern(BASE_SIZE+k,29);
            CHECK(nvfuse_writefile(h,fd,bytes,sizeof(bytes),BASE_SIZE)==sizeof(bytes));
        } else {
            CHECK(!nvfuse_closefile(h,fd));fd=-1;
            if(!strcmp(op,"truncate"))CHECK(!nvfuse_truncate_path(h,"/arena/old",CUT_SIZE));
            else if(!strcmp(op,"move"))CHECK(!nvfuse_rename_path(h,"/arena/moving","/other/moved"));
            else if(!strcmp(op,"move-empty"))CHECK(!nvfuse_rename_path(h,"/arena/vacant","/other/vacant"));
            else if(!strcmp(op,"mkdir"))CHECK(!nvfuse_mkdir_path(h,"/arena/brandnew",0755));
            else CHECK(!nvfuse_rename_path(h,"/arena/old","/other/new"));
        }
        if(!strcmp(phase,"before-fsync"))crash(NULL,phase,0);
        if(!strcmp(phase,"after-fsync")) {
            if(fd<0)fd=nvfuse_openfile_path(h,!strcmp(op,"rename")?"/other/new":"/arena/old",O_RDONLY,0);
            CHECK(fd>=0 && !nvfuse_fsync(h,fd));crash(NULL,phase,0);
        }
        CHECK(!"crash hook did not fire");
    } else if(!strcmp(mode,"data-check")) {
        CHECK(argc==5);CHECK(!test_cold_parents(h,ROOT_INO,ROOT_INO,0));check_data(h,argv[3],!strcmp(argv[4],"new"));CHECK(!test_verify_tree(h));
    } else if(!strcmp(mode,"concurrency")) {
        #ifndef NVFUSE_SPDK_TRANSPORT
        for(unsigned k=0;k<16;k++) {
            pid_t pid;int status;char *args[]={argv[0],"lock-probe",argv[2],NULL};
            CHECK(!posix_spawn(&pid,argv[0],NULL,NULL,args,environ));
            CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
        }
#endif
        atomic_int ready=0,go=0;pthread_t threads[8];struct worker w={h,&ready,&go,10000};
        for(unsigned k=0;k<8;k++)CHECK(!pthread_create(threads+k,NULL,foreign,&w));
        while(atomic_load(&ready)!=8)sched_yield();atomic_store(&go,1);
        for(unsigned k=0;k<1000;k++) {
            write_pattern(h,"/owner",0,4096,(int)k);contents(h,"/owner",4096,0,(int)k);
        }
        for(unsigned k=0;k<8;k++)CHECK(!pthread_join(threads[k],NULL));
        struct stat st;CHECK(nvfuse_getattr(h,"/forbidden",&st)<0);check_data(h,"append",0);
        CHECK(!nvfuse_unlink(h,"/owner"));
        puts("PASS 480000 foreign-thread rejections concurrent with 1000 owner writes");
    } else CHECK(!"unknown mode");
    CHECK(!nvfuse_file_close(h));printf("PASS resilience %s\n",mode);return 0;
}
