/* SPDX-License-Identifier: GPL-2.0-only */
/* Deterministic, non-formatting NVFUSE API/SPDK and Linux FUSE soak worker.
 * Every destructive operation authenticates a private root by its run seed.
 * Large files use direct AIO on the primary path; POSIX sampling is explicit.
 * This exercises clean reopen durability, not power-failure recovery. */
#ifdef NVSOAK_POSIX
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#else
#include "spdk/stdinc.h"
#include <dirent.h>
#include <signal.h>
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_spdk_runtime.h"
#endif
#define CHUNK (256u * 1024)
#define DEPTH 16
static volatile sig_atomic_t stopped;
static const char *stop_file;
static void stop(int sig) { (void)sig; stopped = 1; }
static int stop_requested(void)
{
    if (stop_file && !access(stop_file, F_OK)) stopped = 1;
    return stopped;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
struct work {
    const char *mode, *base;
    uint64_t seed, count, size, done, bytes;
    unsigned batch, seconds;
    double started, tick;
    void *h;
    int barrier;
};
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line=%d mode=%s item=%"PRIu64" errno=%d\n",__LINE__,w->mode,w->done,errno); return -1; } } while(0)
#ifdef NVSOAK_POSIX
#define OPEN(p,f) open(p,f,0644)
#define CLOSE(f) close(f)
#define READ(f,b,n,o) pread(f,b,n,o)
#define WRITE(f,b,n,o) pwrite(f,b,n,o)
#define STAT(p,s) lstat(p,s)
#define MKDIR(p) mkdir(p,0755)
#define RMDIR(p) rmdir(p)
#define UNLINK(p) unlink(p)
#define RENAME(a,b) rename(a,b)
#define LINK(a,b) link(a,b)
#define TRUNC(f,n) ftruncate(f,n)
#define SPACE(s) statvfs(w->base,s)
#define SYNC() fsync(w->barrier)
#else
#define OPEN(p,f) nvfuse_openfile_path(w->h,p,f,0644)
#define CLOSE(f) nvfuse_closefile(w->h,f)
#define READ(f,b,n,o) nvfuse_readfile(w->h,f,(char*)b,n,o)
#define WRITE(f,b,n,o) nvfuse_writefile(w->h,f,(char*)b,n,o)
#define STAT(p,s) nvfuse_getattr(w->h,p,s)
#define MKDIR(p) nvfuse_mkdir_path(w->h,p,0755)
#define RMDIR(p) nvfuse_rmdir_path(w->h,p)
#define UNLINK(p) nvfuse_unlink(w->h,p)
#define RENAME(a,b) nvfuse_rename_path(w->h,a,b)
#define LINK(a,b) nvfuse_hardlink_path(w->h,a,b)
#define TRUNC(f,n) nvfuse_ftruncate(w->h,f,n)
#define SPACE(s) nvfuse_statvfs(w->h,w->base,s)
#define SYNC() nvfuse_sync(w->h)
#endif
static int report(struct work *w, const char *event)
{
    struct statvfs s; CHECK(!SPACE(&s));
    printf("NVSOAK {\"event\":\"%s\",\"mode\":\"%s\",\"seed\":%"PRIu64",\"files\":%"PRIu64",\"bytes\":%"PRIu64",\"elapsed\":%.3f,\"blocks\":%"PRIu64",\"free_blocks\":%"PRIu64",\"block_size\":%lu,\"free_inodes\":%"PRIu64",\"used_percent\":%.5f,\"stopped\":%s}\n",
           event,w->mode,w->seed,w->done,w->bytes,now()-w->started,(uint64_t)s.f_blocks,(uint64_t)s.f_bfree,(unsigned long)s.f_frsize,(uint64_t)s.f_ffree,100.0*(s.f_blocks-s.f_bfree)/s.f_blocks,stopped?"true":"false");
    fflush(stdout); w->tick=now(); return 0;
}
static uint64_t length(struct work *w,uint64_t id) { return w->size ? w->size : 4096u << (id%3); }
static uint64_t pattern(struct work *w,uint64_t id,uint64_t word)
{
    uint64_t k=w->seed ^ ((id+1)*UINT64_C(0x9e3779b97f4a7c15));
    k ^= (word+1)*UINT64_C(0xd6e8feb86659fd93); k ^= k>>29; return k;
}
static int payload(struct work *w,void *buf,size_t n,uint64_t id,uint64_t off,int verify)
{
    uint64_t *p=buf;
    for(size_t j=0;j<n/8;j++) {
        uint64_t v=pattern(w,id,off/8+j);
        if(verify) { if(p[j]!=v) { fprintf(stderr,"DATA MISMATCH file=%"PRIu64" byte=%"PRIu64" expected=%016"PRIx64" actual=%016"PRIx64"\n",id,off+j*8,v,p[j]); return -1; } }
        else p[j]=v;
    }
    return 0;
}
static void paths(struct work *w,uint64_t id,char *dir,char *file,char *extra)
{
    snprintf(dir,512,"%s/b%08"PRIu64,w->base,id/256);
    snprintf(file,576,"%s/f%08"PRIu64,dir,id);
    snprintf(extra,576,"%s/d%08"PRIu64,dir,id);
}
static int admission(struct work *w,uint64_t bytes,int bulk)
{
    struct statvfs s; CHECK(!SPACE(&s));
    uint64_t reserve=(s.f_blocks*(bulk?5:3)+99)/100;
    uint64_t need=(bytes+s.f_frsize-1)/s.f_frsize+4096;
    return s.f_bfree>reserve+need && s.f_ffree>1024 ? 1:0;
}
static int authenticate(struct work *w,int init)
{
    char path[576]; uint64_t expected[2]={UINT64_C(0x4e56534f414b7631),w->seed},got[2];
    if(init) CHECK(!MKDIR(w->base));
    snprintf(path,sizeof(path),"%s/.owner",w->base);
    int fd=OPEN(path,O_RDWR|(init?(O_CREAT|O_EXCL):0)); CHECK(fd>=0);
    if(init) CHECK(WRITE(fd,expected,sizeof(expected),0)==sizeof(expected));
    else { CHECK(READ(fd,got,sizeof(got),0)==sizeof(got)); CHECK(!memcmp(got,expected,sizeof(got))); }
    w->barrier=fd;
    if(init) CHECK(!SYNC());
    return 0;
}
static int small_io(struct work *w,const char *path,uint64_t id,int write,int sample)
{
    uint64_t size=length(w,id); unsigned char buf[CHUNK];
    struct stat st;
    if(!write) { CHECK(!STAT(path,&st)); CHECK(S_ISREG(st.st_mode) && (st.st_mode&0777)==0644 && (uint64_t)st.st_size==size); }
    int fd=OPEN(path,write?(O_RDWR|O_CREAT|O_EXCL):O_RDONLY); CHECK(fd>=0);
    for(uint64_t off=0;off<size;) {
        size_t n=size-off>CHUNK?CHUNK:size-off;
        if(sample && n>65536) n=65536;
        if(write) { CHECK(!payload(w,buf,n,id,off,0)); CHECK(WRITE(fd,buf,n,off)==(ssize_t)n); }
        else { CHECK(READ(fd,buf,n,off)==(ssize_t)n); CHECK(!payload(w,buf,n,id,off,1)); }
        w->bytes+=n;
        if(sample && size>3*65536) {
            if(off==0) off=(size/2/4096)*4096;
            else if(off<size-65536) off=size-65536;
            else break;
        } else off+=n;
    }
    CHECK(!CLOSE(fd)); return 0;
}
/* Exact names, duplicate detection, and counts; physical API readdir slots may
 * contain holes, so only the inode size bounds iteration (not a null entry). */
static int enumeration(struct work *w,const char *path,uint64_t bucket,int root)
{
    uint64_t buckets=(w->count+255)/256;
    size_t cap=root==2?0:(root?buckets+1:320);
    unsigned char *seen=calloc(cap?cap:1,1); CHECK(seen);
    uint64_t found=0; int bad=0;
#ifdef NVSOAK_POSIX
    DIR *d=opendir(path); CHECK(d); struct dirent *e;
    while((e=readdir(d))) {
#else
    int d=nvfuse_opendir(w->h,path); CHECK(d>=0); struct stat st;
    CHECK(!STAT(path,&st)); struct dirent storage,*e;
    for(off_t slot=0;slot<st.st_size/DIR_ENTRY_SIZE || slot<2;slot++) {
        e=nvfuse_readdir(w->h,d,&storage,slot); if(!e) continue;
#endif
        const char *name=e->d_name; if(!strcmp(name,".")||!strcmp(name,"..")) continue;
        uint64_t v=0; char tail; size_t index=cap;
        if(root && !strcmp(name,".owner")) index=0;
        else if(root && sscanf(name,"b%08"SCNu64"%c",&v,&tail)==1 && v<buckets) index=v+1;
        else if(!root && sscanf(name,"f%08"SCNu64"%c",&v,&tail)==1 && v/256==bucket && v<w->count) index=v%256;
        else if(!root && sscanf(name,"d%08"SCNu64"%c",&v,&tail)==1 && v/256==bucket && v<w->count && v%4==0) index=256+v%256/4;
        if(index>=cap || seen[index]) { bad=1; break; }
        seen[index]=1; found++;
    }
#ifdef NVSOAK_POSIX
    if(closedir(d)) bad=1;
#endif
    uint64_t first=bucket*256,last=first+256; if(last>w->count) last=w->count;
    uint64_t expected=root==2?0:root?buckets+1:(last-first)+(last+3)/4-(first+3)/4;
    free(seen); CHECK(!bad && found==expected); return 0;
}
static int metadata(struct work *w)
{
    char dir[512],file[576],extra[576],other[600];
    int create=!strcmp(w->mode,"create"),verify=!strcmp(w->mode,"verify")||!strcmp(w->mode,"sample"),del=!strcmp(w->mode,"delete"),churn=!strcmp(w->mode,"churn");
    for(uint64_t i=0;i<w->count;i++) {
        if(stop_requested() || (create && w->seconds && now()-w->started>=w->seconds)) break;
        paths(w,i,dir,file,extra);
        if(create) {
            int ok=admission(w,length(w,i),w->size>CHUNK); CHECK(ok>=0); if(!ok) break;
            if(i%256==0) CHECK(!MKDIR(dir));
            if(i%4==0) CHECK(!MKDIR(extra));
            CHECK(!small_io(w,file,i,1,0));
        } else if(verify) {
            CHECK(!small_io(w,file,i,0,!strcmp(w->mode,"sample")));
            if(i%4==0) { struct stat st; CHECK(!STAT(extra,&st)&&S_ISDIR(st.st_mode)); CHECK(!enumeration(w,extra,0,2)); }
        } else if(del) {
            CHECK(!UNLINK(file)); if(i%4==0) CHECK(!RMDIR(extra));
        } else if(churn) {
            CHECK(!small_io(w,file,i,0,0));
            snprintf(other,sizeof(other),"%s.rename",file); CHECK(!RENAME(file,other));
            CHECK(!LINK(other,file));
            struct stat st; CHECK(!STAT(file,&st)&&st.st_nlink==2);
            CHECK(!UNLINK(other));
            int fd=OPEN(file,O_RDWR); CHECK(fd>=0);
            int second=OPEN(file,O_RDWR); CHECK(second>=0);
            CHECK(!TRUNC(fd,length(w,i)/2));
            unsigned char buf[16384]; CHECK(length(w,i)<=sizeof(buf));
            CHECK(!payload(w,buf,length(w,i),i,0,0));
            /* The second descriptor was opened before the shrink: both its
             * cached size and the descriptor used for truncate must refresh. */
            CHECK(WRITE(i%2?second:fd,buf,length(w,i),0)==(ssize_t)length(w,i));
            CHECK(!CLOSE(second)); CHECK(!CLOSE(fd));
            CHECK(!small_io(w,file,i,0,0));
        } else CHECK(0);
        w->done=i+1;
        if(!verify && ((i+1)%w->batch==0 || i+1==w->count)) CHECK(!SYNC());
        if(now()-w->tick>5) { if(!verify) CHECK(!SYNC()); CHECK(!report(w,"progress")); }
        if(del && ((i+1)%256==0 || i+1==w->count)) CHECK(!RMDIR(dir));
    }
    CHECK(!SYNC());
    if(verify && !stopped) {
        CHECK(!enumeration(w,w->base,0,1));
        for(uint64_t b=0;b<(w->count+255)/256;b++) { snprintf(dir,sizeof(dir),"%s/b%08"PRIu64,w->base,b); CHECK(!enumeration(w,dir,b,0)); }
    }
    return 0;
}
#ifndef NVSOAK_POSIX
static int owner(struct nvfuse_handle *h,void *arg)
{
    struct work *w=arg; w->h=h;
    CHECK(h->nvh_target->journal);
    if(!strcmp(w->mode,"stat")) return report(w,"result");
    CHECK(!authenticate(w,!strcmp(w->mode,"init")));
    int rc=!strcmp(w->mode,"init")?0:metadata(w);
    if(!rc) rc=report(w,"result");
    if(CLOSE(w->barrier) && !rc) rc=-1;
    return rc;
}
struct slot { struct nvfio_request *r; void *buf; uint64_t off; int busy; };
static int aio_file(struct work *w,struct nvfio_client *client,struct slot *slots,const char *file,uint64_t id,int write)
{
    int fd=nvfio_open(file,0); CHECK(fd>=0);
    uint64_t next=0,completed=0,size=length(w,id); unsigned active=0;
    double last=now();
    while(completed<size) {
        for(unsigned j=0;j<DEPTH && next<size;j++) if(!slots[j].busy) {
            if(write) CHECK(!payload(w,slots[j].buf,CHUNK,id,next,0));
            slots[j].off=next;
            int rc=nvfio_submit(client,slots[j].r,fd,slots[j].buf,CHUNK,next,write);
            if(rc==-EAGAIN) break;
            CHECK(!rc); slots[j].busy=1; active++; next+=CHUNK;
        }
        struct nvfio_request *done[DEPTH]; int n=nvfio_poll(client,DEPTH,done); CHECK(n>=0);
        for(int j=0;j<n;j++) {
            struct slot *s=nvfio_request_user(done[j]); CHECK(!nvfio_request_error(done[j]));
            if(!write) CHECK(!payload(w,s->buf,CHUNK,id,s->off,1));
            s->busy=0; active--; completed+=CHUNK; w->bytes+=CHUNK; last=now();
        }
        CHECK(active || next<size || completed==size);
        CHECK(now()-last<120);
    }
    if(write) CHECK(!nvfio_sync(client,fd));
    CHECK(!nvfio_close(fd)); return 0;
}
static int bulk_prepare(struct nvfuse_handle *h,void *arg)
{
    struct work *w=arg; w->h=h; CHECK(h->nvh_target->journal);
    CHECK(!authenticate(w,0));
    int result=1;
    if(!strcmp(w->mode,"bulk-create")) {
        result=admission(w,w->size,1); CHECK(result>=0);
        if(result) {
            char dir[512],file[576],extra[576]; paths(w,w->done,dir,file,extra);
            if(w->done%256==0) CHECK(!MKDIR(dir));
            if(w->done%4==0) CHECK(!MKDIR(extra));
            int fd=OPEN(file,O_CREAT|O_EXCL|O_RDWR); CHECK(fd>=0); CHECK(!CLOSE(fd)); CHECK(!SYNC());
        }
    } else if(!strcmp(w->mode,"bulk-verify")) {
        char dir[512],file[576],extra[576]; struct stat st; paths(w,w->done,dir,file,extra);
        CHECK(!STAT(file,&st)&&S_ISREG(st.st_mode)&&(uint64_t)st.st_size==w->size);
    }
    CHECK(!CLOSE(w->barrier)); return result;
}
static int progress_owner(struct nvfuse_handle *h,void *arg) { struct work *w=arg; w->h=h; return report(w,"progress"); }
static int bulk_namespace(struct nvfuse_handle *h,void *arg)
{
    struct work *w=arg; w->h=h; CHECK(!enumeration(w,w->base,0,1));
    for(uint64_t i=0;i<w->count;i++) {
        char dir[512],file[576],extra[576]; paths(w,i,dir,file,extra);
        if(i%256==0) CHECK(!enumeration(w,dir,i/256,0));
        if(i%4==0) { struct stat st; CHECK(!STAT(extra,&st)&&S_ISDIR(st.st_mode)); CHECK(!enumeration(w,extra,0,2)); }
    }
    return 0;
}
static int bulk(struct work *w)
{
    struct nvfio_client *c=nvfio_client_alloc(4); CHECK(c);
    struct slot slots[DEPTH]={0};
    for(unsigned j=0;j<DEPTH;j++) { slots[j].buf=nvfio_dma_alloc(CHUNK); slots[j].r=nvfio_request_alloc(&slots[j]); CHECK(slots[j].buf&&slots[j].r); }
    int write=!strcmp(w->mode,"bulk-create");
    for(uint64_t i=0;i<w->count;i++) {
        if(stop_requested() || (write && w->seconds && now()-w->started>=w->seconds)) break;
        int ok=nvfio_execute(bulk_prepare,w); CHECK(ok>=0); if(!ok) break;
        char dir[512],file[576],extra[576]; paths(w,i,dir,file,extra);
        CHECK(!aio_file(w,c,slots,file,i,write)); w->done=i+1;
        if(now()-w->tick>5) CHECK(!nvfio_execute(progress_owner,w));
    }
    for(unsigned j=0;j<DEPTH;j++) { nvfio_dma_free(slots[j].buf); nvfio_request_free(slots[j].r); }
    CHECK(!nvfio_client_free(c));
    if(!write && !stopped) { /* Namespace and directory checks without rereading TB of data. */
        CHECK(!nvfio_execute(bulk_namespace,w));
    }
    CHECK(!nvfio_execute(progress_owner,w));
    return 0;
}
#endif
int main(int argc,char **argv)
{
#ifdef NVSOAK_POSIX
    int start=1;
#else
    int start=3;
#endif
    if(argc!=start+7) { fprintf(stderr,"usage: [json bdf] mode base seed count bytes(0=mixed4/8/16K) seconds batch\n"); return 2; }
    struct work w={.mode=argv[start],.base=argv[start+1],.seed=strtoull(argv[start+2],0,10),.count=strtoull(argv[start+3],0,10),.size=strtoull(argv[start+4],0,10),.seconds=strtoul(argv[start+5],0,10),.batch=strtoul(argv[start+6],0,10)};
    if(w.base[0]!='/' || strlen(w.base)>440 || !w.seed || w.count>10000000 || !w.batch || w.batch>128 || w.size>(1ull<<30) || (w.size && w.size%4096)) return 2;
    stop_file=getenv("NVSOAK_STOP_FILE");
    signal(SIGINT,stop); signal(SIGTERM,stop); w.started=w.tick=now(); int rc;
#ifdef NVSOAK_POSIX
    rc=!strcmp(w.mode,"stat")?report(&w,"result"):authenticate(&w,!strcmp(w.mode,"init"));
    if(!rc && strcmp(w.mode,"stat")) {
        if(strcmp(w.mode,"init")) rc=metadata(&w);
        if(!rc) rc=report(&w,"result");
        if(CLOSE(w.barrier)&&!rc) rc=-1;
    }
#else
    rc=nvfio_start(argv[1],argv[2],0);
    if(!rc) {
        if(!strcmp(w.mode,"bulk-create")||!strcmp(w.mode,"bulk-verify")) rc=bulk(&w);
        else rc=nvfio_execute(owner,&w);
        if(rc) { fflush(NULL); _exit(1); } /* Retain original dirty evidence on failure. */
        rc=nvfio_stop();
    }
#endif
    if(!rc) { printf("NVSOAK_DONE {\"files\":%"PRIu64",\"bytes\":%"PRIu64",\"stopped\":%s}\n",w.done,w.bytes,stopped?"true":"false"); fflush(stdout); }
    return rc?1:0;
}
