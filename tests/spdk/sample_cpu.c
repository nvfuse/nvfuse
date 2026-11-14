/* SPDX-License-Identifier: GPL-2.0-only */

/* Linux x86_64 user-space IP sampler. No changes to the target executable. */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define MAX_EVENTS 128
struct event { int fd, tid; void *map; size_t length; uint64_t tail, samples, lost; };
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void copy_ring(void *dst, const char *ring, size_t size, uint64_t offset, size_t count)
{
    size_t start=offset%size, first=count < size-start ? count : size-start;
    memcpy(dst,ring+start,first); if(first<count)memcpy((char *)dst+first,ring,count-first);
}
static void drain(struct event *e, FILE *out)
{
    struct perf_event_mmap_page *meta=e->map;
    uint64_t head=__atomic_load_n(&meta->data_head,__ATOMIC_ACQUIRE);
    size_t size=meta->data_size; const char *ring=(char *)e->map+meta->data_offset;
    if(head-e->tail>size){fprintf(stderr,"ring overrun tid=%d\n",e->tid);exit(2);}
    while(e->tail<head){
        struct perf_event_header h;
        copy_ring(&h,ring,size,e->tail,sizeof(h));
        if(h.size<sizeof(h)||h.size>size||e->tail+h.size>head){fprintf(stderr,"bad perf record\n");exit(2);}
        if(h.type==PERF_RECORD_SAMPLE){
            struct { uint64_t ip; uint32_t pid,tid; uint64_t time; } sample;
            if(h.size<sizeof(h)+sizeof(sample)){fprintf(stderr,"short sample\n");exit(2);}
            copy_ring(&sample,ring,size,e->tail+sizeof(h),sizeof(sample));
            fprintf(out,"%u,0x%" PRIx64 ",%" PRIu64 "\n",sample.tid,sample.ip,sample.time);e->samples++;
        }else if(h.type==PERF_RECORD_LOST){
            uint64_t lost[2];copy_ring(lost,ring,size,e->tail+sizeof(h),sizeof(lost));e->lost+=lost[1];
        }
        e->tail+=h.size;
    }
    __atomic_store_n(&meta->data_tail,e->tail,__ATOMIC_RELEASE);
}
int main(int argc,char **argv)
{
    if(argc!=4){fprintf(stderr,"usage: sample_cpu PID SECONDS OUTPUT.csv\n");return 2;}
    int pid=atoi(argv[1]); double seconds=atof(argv[2]);
    if(pid<=0||seconds<=0||seconds>60)return 2;
    char path[256];snprintf(path,sizeof(path),"/proc/%d/task",pid);
    DIR *tasks=opendir(path);if(!tasks){perror(path);return 2;}
    struct event events[MAX_EVENTS];int count=0;struct dirent *entry;
    while((entry=readdir(tasks))){
        int tid=atoi(entry->d_name);if(tid<=0)continue;
        if(count==MAX_EVENTS){fprintf(stderr,"too many tasks\n");return 2;}
        struct perf_event_attr attr={0};attr.type=PERF_TYPE_SOFTWARE;attr.size=sizeof(attr);
        attr.config=PERF_COUNT_SW_CPU_CLOCK;attr.freq=1;attr.sample_freq=997;
        attr.sample_type=PERF_SAMPLE_IP|PERF_SAMPLE_TID|PERF_SAMPLE_TIME;
        attr.exclude_kernel=1;attr.exclude_hv=1;attr.wakeup_events=1;
        int fd=syscall(__NR_perf_event_open,&attr,tid,-1,-1,PERF_FLAG_FD_CLOEXEC);
        if(fd<0){if(errno==ESRCH)continue;perror("perf_event_open");return 2;}
        size_t length=(size_t)sysconf(_SC_PAGESIZE)*9;
        void *map=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(map==MAP_FAILED){perror("mmap");close(fd);return 2;}
        events[count++]=(struct event){.fd=fd,.tid=tid,.map=map,.length=length};
    }
    closedir(tasks);if(!count)return 2;
    FILE *out=fopen(argv[3],"wx");if(!out){perror(argv[3]);return 2;}
    fputs("tid,ip,time_ns\n",out);
    uint64_t end=now_ns()+(uint64_t)(seconds*1e9);
    do{for(int i=0;i<count;i++)drain(&events[i],out);usleep(10000);}while(now_ns()<end);
    uint64_t lost=0;
    for(int i=0;i<count;i++){
        drain(&events[i],out);fprintf(stderr,"tid=%d samples=%" PRIu64 " lost=%" PRIu64 "\n",events[i].tid,events[i].samples,events[i].lost);
        lost+=events[i].lost;munmap(events[i].map,events[i].length);close(events[i].fd);
    }
    if(fclose(out))return 2;
    return lost?1:0;
}
