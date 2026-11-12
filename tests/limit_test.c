/* Exhaust real inode/block resources without storing regular-file payload. */
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_journal.h"
#include "nvfuse_tx.h"
#include "test_backend.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define SHARDS 32u
static void name(char *out,size_t len,const char *kind,unsigned n) {
    if (!strcmp(kind,"sharded")) snprintf(out,len,"/arena/p%02u/item-%05u",n%SHARDS,n);
    else snprintf(out,len,"/arena/item-%05u",n);
}
static int create(struct nvfuse_handle *h,const char *kind,const char *path) {
    if (!strcmp(kind,"dirs")) return nvfuse_mkdir_path(h,path,0755);
    int fd=nvfuse_openfile_path(h,path,O_CREAT|O_RDWR,0644);
    if (fd<0) return fd;
    REQUIRE(!nvfuse_closefile(h,fd)); return 0;
}
static void verify_names(struct nvfuse_handle *h,const char *kind,unsigned count,int reused) {
    struct stat st; char path[80];
    for (unsigned i=0;i<count;i++) {
        if (reused && i==count/2) continue;
        name(path,sizeof(path),kind,i);
        REQUIRE(!nvfuse_getattr(h,path,&st));
        if (!strcmp(kind,"dirs")) REQUIRE(S_ISDIR(st.st_mode));
        else REQUIRE(st.st_size==0 && (st.st_mode & 0777)==0644);
    }
    name(path,sizeof(path),kind,count); REQUIRE(nvfuse_getattr(h,path,&st)<0);
    if (reused) {
        name(path,sizeof(path),kind,count/2); REQUIRE(nvfuse_getattr(h,path,&st)<0);
        REQUIRE(!nvfuse_getattr(h,"/arena/replacement",&st));
    }
}
static void counts(struct nvfuse_handle *h,const char *kind,unsigned *count) {
    char path[80]; struct statvfs before,after;
    REQUIRE(!nvfuse_statvfs(h,"/",&before));
    if (!strcmp(kind,"sharded"))
        for (unsigned i=0;i<SHARDS;i++) { snprintf(path,sizeof(path),"/arena/p%02u",i); REQUIRE(!nvfuse_mkdir_path(h,path,0755)); }
    unsigned n=0;
    for (;;) {
        name(path,sizeof(path),kind,n);
        int rc=create(h,kind,path);
        if (rc<0) { REQUIRE(rc==-ENOSPC); break; }
        n++;
        REQUIRE(n<=before.f_ffree);
        if (!h->nvh_sb.sb_free_inodes) {
            /* Reuse the final inode before an ENOSPC rollback/remount can reset
             * the one-past-volume allocator hint and hide group wrap bugs. */
            REQUIRE(!(!strcmp(kind,"dirs")?nvfuse_rmdir_path(h,path):nvfuse_rmfile_path(h,path)));
            REQUIRE(!create(h,kind,path));
            REQUIRE(!h->nvh_sb.sb_free_inodes);
            printf("PASS final inode reuse before exhaustion rollback\n");
        }
    }
    REQUIRE(n>0); REQUIRE(!nvfuse_statvfs(h,"/",&after));
    REQUIRE(after.f_ffree==0 && after.f_bfree>0);
    printf("CAPACITY kind=%s count=%u free_blocks=%llu\n",kind,n,(unsigned long long)after.f_bfree);
    verify_names(h,kind,n,0);
    name(path,sizeof(path),kind,n/2);
    int rc=!strcmp(kind,"dirs")?nvfuse_rmdir_path(h,path):nvfuse_rmfile_path(h,path);
    REQUIRE(!rc);
    if (!strcmp(kind,"dirs")) {
        /* An empty directory needs a child inode AND a delayed index inode.
         * One remaining inode must fail before either resource is consumed. */
        struct statvfs saved,restored; struct stat parent;
        REQUIRE(!nvfuse_statvfs(h,"/",&saved) && saved.f_ffree==1);
        REQUIRE(!nvfuse_tx_begin(h,NVFUSE_TX_METADATA,0));
        REQUIRE(nvfuse_mkdir_path(h,"/arena/item-00000/child",0755)==-ENOSPC);
        REQUIRE(nvfuse_tx_status(h)==NVFUSE_TX_ABORT_ONLY);
        REQUIRE(nvfuse_tx_commit(h)==-ENOSPC); REQUIRE(!nvfuse_tx_abort(h));
        REQUIRE(!nvfuse_statvfs(h,"/",&restored));
        REQUIRE(saved.f_ffree==restored.f_ffree && saved.f_bfree==restored.f_bfree);
        REQUIRE(!nvfuse_getattr(h,"/arena/item-00000",&parent) && parent.st_size==0);
    }
    REQUIRE(!create(h,kind,"/arena/replacement"));
    REQUIRE(!nvfuse_statvfs(h,"/",&after) && after.f_ffree==0);
    verify_names(h,kind,n,1); *count=n;
}
static uint64_t size_limit(struct nvfuse_handle *h) {
    int fd=nvfuse_openfile_path(h,"/arena/large",O_CREAT|O_RDWR,0644); REQUIRE(fd>=0);
    char data[1024*1024]; memset(data,0x5a,sizeof(data));
    uint64_t size=0;
    unsigned chunk=sizeof(data);
    for (;;) {
        int rc=nvfuse_writefile(h,fd,data,chunk,(nvfuse_off_t)size);
        if (rc<0) {
            REQUIRE(rc==-ENOSPC);
            struct stat st; REQUIRE(!nvfuse_getattr(h,"/arena/large",&st) && (uint64_t)st.st_size==size);
            if (chunk==NVJ_BLOCK) break;
            chunk=NVJ_BLOCK; continue;
        }
        REQUIRE(rc==(int)chunk); size+=chunk;
    }
    REQUIRE(size>4*1024*1024); /* Exercise direct, single and double indirection. */
    char readback[19];
    const uint64_t offsets[]={0,11ULL*4096-7,(11ULL+1024)*4096-7,size-19};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        memset(readback,0xa5,sizeof(readback));
        REQUIRE(nvfuse_readfile(h,fd,readback,sizeof(readback),offsets[i])==sizeof(readback));
        for(unsigned k=0;k<sizeof(readback);k++) REQUIRE(!readback[k]);
    }
    /* Reject representational range errors before sparse-write handling. */
    REQUIRE(nvfuse_writefile(h,fd,data,1,MAX_FILE_SIZE)==-EINVAL);
    REQUIRE(nvfuse_writefile(h,fd,data,1,MAX_FILE_SIZE-1)==-EINVAL);
    REQUIRE(nvfuse_writefile(h,fd,data,0,MAX_FILE_SIZE)==-EINVAL);
    REQUIRE(!nvfuse_closefile(h,fd));
    struct statvfs full,reclaimed; REQUIRE(!nvfuse_statvfs(h,"/",&full));
    REQUIRE(full.f_bfree<=2);
    /* Explicit ENOSPC must be abort-only and preserve the full committed file. */
    REQUIRE(!nvfuse_tx_begin(h,NVFUSE_TX_METADATA,0));
    fd=nvfuse_openfile_path(h,"/arena/large",O_RDWR,0); REQUIRE(fd>=0);
    REQUIRE(nvfuse_writefile(h,fd,data,NVJ_BLOCK,size)==-ENOSPC);
    REQUIRE(nvfuse_tx_status(h)==NVFUSE_TX_ABORT_ONLY);
    REQUIRE(nvfuse_tx_commit(h)==-ENOSPC); REQUIRE(!nvfuse_tx_abort(h));
    struct stat committed;
    REQUIRE(!nvfuse_getattr(h,"/arena/large",&committed) && (uint64_t)committed.st_size==size);
    REQUIRE(nvfuse_ftruncate(h,fd,0)==-EBADF);
    REQUIRE(!nvfuse_truncate_path(h,"/arena/large",size-NVJ_BLOCK));
    REQUIRE(!nvfuse_statvfs(h,"/",&reclaimed) && reclaimed.f_bfree>full.f_bfree);
    fd=nvfuse_openfile_path(h,"/arena/large",O_RDWR,0); REQUIRE(fd>=0);
    REQUIRE(nvfuse_writefile(h,fd,data,NVJ_BLOCK,size-NVJ_BLOCK)==NVJ_BLOCK);
    REQUIRE(!nvfuse_closefile(h,fd));
    return size;
}
static const char *crash_phase; static unsigned crash_nth;
static void crash_hook(void *arg,const char *phase,uint64_t block) {
    (void)arg;(void)block;
    if (!strcmp(phase,crash_phase) && !--crash_nth) {
        if (getenv("NVFUSE_VM_CUT")) {
            printf("READY VM cut phase=%s\n",phase);fflush(stdout);
            for (;;) pause();
        }
        _exit(86);
    }
}
static void mutate(struct nvfuse_handle *h,const char *kind,uint64_t reached,const char *op) {
    if (!strcmp(kind,"size")) {
        if (!strcmp(op,"delete")) REQUIRE(!nvfuse_rmfile_path(h,"/arena/large"));
        else if (!strcmp(op,"truncate")) REQUIRE(!nvfuse_truncate_path(h,"/arena/large",reached/2+13));
        else {
            char bytes[NVJ_BLOCK]={0};int fd=nvfuse_openfile_path(h,"/arena/large",O_RDWR,0);REQUIRE(fd>=0);
            REQUIRE(nvfuse_writefile(h,fd,bytes,sizeof(bytes),reached-NVJ_BLOCK)==sizeof(bytes));
            REQUIRE(!nvfuse_closefile(h,fd));
        }
    } else if (!strcmp(op,"delete")) {
        char path[80];name(path,sizeof(path),kind,0);
        REQUIRE(!(!strcmp(kind,"dirs")?nvfuse_rmdir_path(h,path):nvfuse_rmfile_path(h,path)));
    } else REQUIRE(!create(h,kind,"/arena/added"));
}
static void check_crash(struct nvfuse_handle *h,const char *kind,uint64_t reached,const char *op,int after) {
    struct stat st;
    if (!strcmp(kind,"size")) {
        int rc=nvfuse_getattr(h,"/arena/large",&st);
        if (!strcmp(op,"delete") && after) REQUIRE(rc<0);
        else {
            uint64_t expected=!strcmp(op,"truncate") && after?reached/2+13:
                !strcmp(op,"create") && !after?reached-NVJ_BLOCK:reached;
            REQUIRE(!rc && (uint64_t)st.st_size==expected);
        }
    } else {
        char path[80];
        for (unsigned n=0;n<reached;n++) {
            name(path,sizeof(path),kind,n);int rc=nvfuse_getattr(h,path,&st);
            if(n==reached/2 || (!strcmp(op,"delete") && after && !n)) REQUIRE(rc<0);
            else REQUIRE(!rc && st.st_size==0);
        }
        int rc=nvfuse_getattr(h,"/arena/replacement",&st);
        if(!strcmp(op,"create")) REQUIRE(rc<0);else REQUIRE(!rc);
        rc=nvfuse_getattr(h,"/arena/added",&st);
        if(!strcmp(op,"create") && after) REQUIRE(!rc);else REQUIRE(rc<0);
    }
}
int main(int argc,char **argv) {
    REQUIRE(argc>=5);
    const char *mode=argv[1],*path=argv[2],*kind=argv[3];
    uint64_t bytes=strtoull(argv[4],NULL,10)*1024*1024;
    int fresh=!strcmp(mode,"create");
    struct nvfuse_handle *h=nvfuse_file_open_mode(path,fresh,bytes,1); REQUIRE(h);
    if (!strcmp(mode,"reserve") || !strcmp(mode,"mutate") || !strcmp(mode,"crash") || !strcmp(mode,"check-crash") || !strcmp(mode,"reuse")) {
        REQUIRE(argc>=7);uint64_t reached=strtoull(argv[5],NULL,10);const char *op=argv[6];
        if(!strcmp(mode,"reserve")) {
            REQUIRE(!strcmp(op,"create"));
            if(!strcmp(kind,"size")) REQUIRE(!nvfuse_truncate_path(h,"/arena/large",reached-NVJ_BLOCK));
            else REQUIRE(!(!strcmp(kind,"dirs")?nvfuse_rmdir_path(h,"/arena/replacement"):nvfuse_rmfile_path(h,"/arena/replacement")));
        } else if(!strcmp(mode,"reuse")) {
            if(strcmp(kind,"size")) {
                char entry[80];name(entry,sizeof(entry),kind,1);
                REQUIRE(!(!strcmp(kind,"dirs")?nvfuse_rmdir_path(h,entry):nvfuse_rmfile_path(h,entry)));
            }
            REQUIRE(!create(h,kind,"/arena/probe"));
            REQUIRE(!(!strcmp(kind,"dirs")?nvfuse_rmdir_path(h,"/arena/probe"):nvfuse_rmfile_path(h,"/arena/probe")));
        } else if(!strcmp(mode,"check-crash")) { REQUIRE(argc==8);check_crash(h,kind,reached,op,!strcmp(argv[7],"new")); }
        else {
            if(!strcmp(mode,"crash")) {
                REQUIRE(argc==9);crash_phase=argv[7];crash_nth=(unsigned)strtoul(argv[8],NULL,10);
                REQUIRE(!nvfuse_tx_set_hook(h,crash_hook,NULL));
            }
            mutate(h,kind,reached,op);REQUIRE(strcmp(mode,"crash"));
        }
        REQUIRE(!nvfuse_file_close(h));printf("PASS limit %s %s\n",mode,kind);return 0;
    }
    uint64_t reached=0;
    struct statvfs baseline; REQUIRE(!nvfuse_statvfs(h,"/",&baseline));
    if (fresh || !strcmp(mode,"refill")) {
        REQUIRE(!nvfuse_mkdir_path(h,"/arena",0755));
        if (!strcmp(kind,"size")) reached=size_limit(h);
        else { unsigned n=0; counts(h,kind,&n); reached=n; }
        printf("LIMIT {\"kind\":\"%s\",\"reached\":%" PRIu64 ",\"image_bytes\":%" PRIu64 ",\"format_size_exclusive\":%" PRId64 ",\"payload_discarded\":%" PRIu64 ",\"baseline_blocks\":%llu,\"baseline_inodes\":%llu}\n",
               kind,reached,bytes,(int64_t)MAX_FILE_SIZE,h->nvh_target->payload_bytes_discarded,(unsigned long long)baseline.f_bfree,(unsigned long long)baseline.f_ffree);
    } else {
        REQUIRE(argc==6); reached=strtoull(argv[5],NULL,10);
        if (!strcmp(kind,"size")) {
            struct stat st; REQUIRE(!nvfuse_getattr(h,"/arena/large",&st) && (uint64_t)st.st_size==reached);
            int fd=nvfuse_openfile_path(h,"/arena/large",O_RDONLY,0); REQUIRE(fd>=0);
            char zero[32]; memset(zero,0xa5,sizeof(zero));
            REQUIRE(nvfuse_readfile(h,fd,zero,sizeof(zero),reached-sizeof(zero))==sizeof(zero));
            for(unsigned i=0;i<sizeof(zero);i++) REQUIRE(!zero[i]);
            REQUIRE(!nvfuse_closefile(h,fd));
        } else verify_names(h,kind,(unsigned)reached,1);
    }
    if (!strcmp(mode,"cleanup")) {
        if (!strcmp(kind,"size")) REQUIRE(!nvfuse_rmfile_path(h,"/arena/large"));
        else {
            char entry[80];
            for (unsigned i=0;i<reached;i++) {
                if (i==reached/2) continue;
                name(entry,sizeof(entry),kind,i);
                REQUIRE(!(!strcmp(kind,"dirs")?nvfuse_rmdir_path(h,entry):nvfuse_rmfile_path(h,entry)));
            }
            REQUIRE(!(!strcmp(kind,"dirs")?nvfuse_rmdir_path(h,"/arena/replacement"):nvfuse_rmfile_path(h,"/arena/replacement")));
            if (!strcmp(kind,"sharded")) for (unsigned i=0;i<SHARDS;i++) {
                snprintf(entry,sizeof(entry),"/arena/p%02u",i); REQUIRE(!nvfuse_rmdir_path(h,entry));
            }
        }
        REQUIRE(!nvfuse_rmdir_path(h,"/arena"));
        struct statvfs empty; REQUIRE(!nvfuse_statvfs(h,"/",&empty));
        printf("CLEANUP {\"free_blocks\":%llu,\"free_inodes\":%llu}\n",(unsigned long long)empty.f_bfree,(unsigned long long)empty.f_ffree);
    }
    REQUIRE(!nvfuse_file_close(h)); printf("PASS limit %s %s\n",mode,kind); return 0;
}
