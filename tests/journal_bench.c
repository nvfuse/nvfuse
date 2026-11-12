/* File-image benchmark. Every measured group ends at a durable journal commit. */
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_tx.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"benchmark failed: %s\n",#x); exit(1); } } while(0)
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }
static int compare(const void *a,const void *b) { double x=*(const double *)a,y=*(const double *)b; return (x>y)-(x<y); }
int main(int argc,char **argv) {
    CHECK(argc==5);
    unsigned mode=atoi(argv[2]),batch=atoi(argv[3]),files=atoi(argv[4]);
    CHECK(mode<=2 && batch && files && !(files%batch));
    struct nvfuse_handle *h=nvfuse_file_open(argv[1],1,128ULL*1024*1024); CHECK(h);
    struct nvj_stats before,after; CHECK(!nvfuse_tx_get_stats(h,&before));
    double *latency=calloc(files/batch,sizeof(*latency)); CHECK(latency);
    char data[8192]; memset(data,'B',sizeof(data));
    double start=now();
    for(unsigned i=0;i<files;i+=batch) {
        if(mode) CHECK(!nvfuse_tx_begin(h,mode,0));
        for(unsigned n=i;n<i+batch;n++) {
            char path[40]; snprintf(path,sizeof(path),"/bench-%u",n);
            int fd=nvfuse_openfile_path(h,path,O_CREAT|O_RDWR,0644); CHECK(fd>=0);
            CHECK(nvfuse_writefile(h,fd,data,sizeof(data),0)==sizeof(data));
            CHECK(!nvfuse_closefile(h,fd));
        }
        double commit=now();
        if(mode) CHECK(!nvfuse_tx_commit(h));
        latency[i/batch]=(now()-commit)*1e3;
    }
    double elapsed=now()-start; CHECK(!nvfuse_tx_get_stats(h,&after));
    unsigned groups=files/batch; qsort(latency,groups,sizeof(*latency),compare);
    double abort_ms=0;
    if(mode) {
        CHECK(!nvfuse_tx_begin(h,mode,0));
        CHECK(nvfuse_mkdir_path(h,"/aborted",0755)==0);
        double start_abort=now(); CHECK(!nvfuse_tx_abort(h)); abort_ms=(now()-start_abort)*1e3;
    }
    printf("BENCH {\"mode\":%u,\"batch\":%u,\"files\":%u,\"seconds\":%.6f,\"files_per_second\":%.3f,\"commit_p50_ms\":%.3f,\"commit_p95_ms\":%.3f,\"commit_p99_ms\":%.3f,\"commits\":%llu,\"flushes\":%llu,\"journal_bytes\":%llu,\"home_bytes\":%llu,\"peak_blocks\":%llu,\"abort_ms\":%.3f}\n",
        mode,batch,files,elapsed,files/elapsed,latency[groups/2],latency[(groups-1)*95/100],latency[(groups-1)*99/100],
        (unsigned long long)(after.commits-before.commits),(unsigned long long)(after.flushes-before.flushes),
        (unsigned long long)(after.journal_bytes-before.journal_bytes),(unsigned long long)(after.home_bytes-before.home_bytes),
        (unsigned long long)after.peak_blocks,abort_ms);
    free(latency); CHECK(!nvfuse_file_close(h)); return 0;
}
