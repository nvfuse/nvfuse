/* SPDX-License-Identifier: GPL-2.0-only */
#include "spdk/stdinc.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_spdk_runtime.h"
#include "nvbridge.h"
enum {BR_OPEN,BR_CLOSE,BR_READ,BR_WRITE,BR_TRUNCATE,BR_SYNC,BR_STAT,BR_MKDIR,BR_UNLINK,BR_RMDIR,BR_RENAME,BR_LINK,BR_LIST,BR_OPS};
static const char*names[]={"open","close","read","write","truncate","sync","stat","mkdir","unlink","rmdir","rename","link","list"};
struct call {int op,fd,flags;const char *p,*q;void*b;size_t n;uint64_t off,size,mtime;unsigned mode;char**list;size_t count;};
static uint64_t count[BR_OPS],total[BR_OPS],api[BR_OPS],old_count[BR_OPS],old_total[BR_OPS],old_api[BR_OPS];
static uint64_t nanos(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*UINT64_C(1000000000)+t.tv_nsec;}
static int work(struct nvfuse_handle*h,void*a){struct call*c=a;int r=-EINVAL;struct stat s;
switch(c->op){
case BR_OPEN:r=nvfuse_openfile_path(h,c->p,c->flags,0600);break;
case BR_CLOSE:r=nvfuse_closefile(h,c->fd);break;
case BR_READ:r=nvfuse_readfile(h,c->fd,c->b,c->n,c->off);break;
case BR_WRITE:r=nvfuse_writefile(h,c->fd,c->b,c->n,c->off);break;
case BR_TRUNCATE:r=nvfuse_ftruncate(h,c->fd,c->off);break;
case BR_SYNC:r=c->fd<0?nvfuse_sync(h):nvfuse_fsync(h,c->fd);break;
case BR_STAT:r=nvfuse_getattr(h,c->p,&s);if(!r){c->size=s.st_size;c->mtime=s.st_mtime;c->mode=s.st_mode;}break;
case BR_MKDIR:r=nvfuse_mkdir_path(h,c->p,0700);break;
case BR_UNLINK:r=nvfuse_unlink(h,c->p);break;
case BR_RMDIR:r=nvfuse_rmdir_path(h,c->p);break;
case BR_RENAME:r=nvfuse_rename_path(h,c->p,c->q);break;
case BR_LINK:r=nvfuse_hardlink_path(h,c->p,c->q);break;
case BR_LIST:{r=nvfuse_getattr(h,c->p,&s);if(r)break;if(!S_ISDIR(s.st_mode)){r=-ENOTDIR;break;}int ino=nvfuse_opendir(h,c->p);if(ino<0){r=ino;break;}size_t slots=s.st_size/DIR_ENTRY_SIZE;if(slots<2)slots=2;c->list=calloc(slots,sizeof(char*));if(!c->list){r=-ENOMEM;break;}for(size_t j=0;j<slots;j++){struct dirent ent;if(!nvfuse_readdir(h,ino,&ent,j))continue;if(!strcmp(ent.d_name,".")||!strcmp(ent.d_name,".."))continue;char*z=strdup(ent.d_name);if(!z){r=-ENOMEM;break;}c->list[c->count++]=z;}break;}
}
return r;}
static int caller_api;
static int call(struct call*c){return caller_api?nvfio_execute_serialized(work,c):nvfio_execute(work,c);}
int nvdb_start(const char*j,const char*b,int f){const char*s=getenv("NVFUSE_API_CALLER_EXEC");caller_api=s&&!strcmp(s,"1");return nvfio_start(j,b,f);}
int nvdb_stop(void){return nvfio_stop();}
int nvdb_open(const char*p,int f){struct call c={.op=BR_OPEN,.p=p,.flags=f};return call(&c);}
int nvdb_close(int f){struct call c={.op=BR_CLOSE,.fd=f};return call(&c);}
int nvdb_read(int f,void*b,size_t n,uint64_t o){return nvfio_pread(f,b,n,o);}
int nvdb_write(int f,const void*b,size_t n,uint64_t o){return nvfio_pwrite(f,b,n,o);}
int nvdb_truncate(int f,uint64_t n){struct call c={.op=BR_TRUNCATE,.fd=f,.off=n};return call(&c);}
int nvdb_sync(int f){return nvfio_fsync(f);}
int nvdb_stat(const char*p,uint64_t*n,uint64_t*m,unsigned*mode){struct call c={.op=BR_STAT,.p=p};int r=call(&c);if(!r){if(n)*n=c.size;if(m)*m=c.mtime;if(mode)*mode=c.mode;}return r;}
int nvdb_mkdir(const char*p){struct call c={.op=BR_MKDIR,.p=p};return call(&c);}
int nvdb_unlink(const char*p){struct call c={.op=BR_UNLINK,.p=p};return call(&c);}
int nvdb_rmdir(const char*p){struct call c={.op=BR_RMDIR,.p=p};return call(&c);}
int nvdb_rename(const char*p,const char*q){struct call c={.op=BR_RENAME,.p=p,.q=q};return call(&c);}
int nvdb_link(const char*p,const char*q){struct call c={.op=BR_LINK,.p=p,.q=q};return call(&c);}
int nvdb_list(const char*p,char***z,size_t*n){struct call c={.op=BR_LIST,.p=p};int r=call(&c);*z=c.list;*n=c.count;return r;}
extern int nvfio_file_sync_stats(uint64_t *);
extern int nvfio_commit_cache_stats(uint64_t *);
void nvdb_profile(const char *phase) {
 uint64_t c[8], cc[8];
 if(nvfio_file_sync_stats(c) || nvfio_commit_cache_stats(cc)){fputs("stats failure\n",stderr);abort();}
 printf("KVCC {\"phase\":\"%s\",\"reads\":%llu,\"writes\":%llu,\"bytes\":%llu}\n",phase,(unsigned long long)cc[0],(unsigned long long)cc[1],(unsigned long long)cc[2]);
 printf("KVDCC {\"phase\":\"%s\",\"windows\":%llu,\"reads\":%llu,\"writes\":%llu,\"bytes\":%llu}\n",phase,(unsigned long long)cc[4],(unsigned long long)cc[5],(unsigned long long)cc[6],(unsigned long long)cc[7]);
 printf("KVFSYNC {\"phase\":\"%s\",\"calls\":%llu,\"hits\":%llu,\"deferred\":%llu,\"allocations\":%llu,\"commits\":%llu,\"journal_bytes\":%llu,\"dirty_blocks\":%llu,\"dirty_requests\":%llu}\n",phase,(unsigned long long)c[0],(unsigned long long)c[1],(unsigned long long)c[2],(unsigned long long)c[3],(unsigned long long)c[4],(unsigned long long)c[5],(unsigned long long)c[6],(unsigned long long)c[7]);fflush(stdout);
}
