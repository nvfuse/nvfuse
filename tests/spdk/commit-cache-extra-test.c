/* Supplementary guarded append/fault tests using the exact validated runtime. */
#define main inherited_cache_test_main
#include "commit_cache_v2_test.c"
#undef main
static unsigned extra_size;
static int extra_case,extra_alias[3],extra_seed;
static char extra_pattern(unsigned k){return(char)(k*29u+k/97u+11u);}
static void extra_name(char *out){snprintf(out,96,"/commit-cache-extra-%d",extra_case);}
static int extra_open(struct nvfuse_handle*h,void*unused)
{
 (void)unused;char name[96];extra_name(name);
 cc_fd=nvfuse_openfile_path(h,name,extra_seed?O_CREAT|O_RDWR:O_RDONLY,0600);CHECK(cc_fd>=0);
 if(!extra_seed)return 0;
 CHECK(!nvfuse_ftruncate(h,cc_fd,0));char *data=malloc(extra_size);CHECK(data);
 for(unsigned k=0;k<extra_size;k++)data[k]=extra_pattern(k);
 CHECK(nvfuse_writefile(h,cc_fd,data,extra_size,0)==(int)extra_size);free(data);
 CHECK(!nvfuse_fsync(h,cc_fd));
 extra_alias[0]=nvfuse_openfile_path(h,name,O_RDONLY,0);
 extra_alias[1]=nvfuse_openfile_path(h,name,O_RDWR|O_SYNC,0);
 extra_alias[2]=nvfuse_openfile_path(h,name,O_RDWR|O_DIRECT,0);
 for(unsigned k=0;k<3;k++)CHECK(extra_alias[k]>=0);return 0;
}
static int extra_close(struct nvfuse_handle*h,void*unused)
{
 (void)unused;CHECK(!nvfuse_closefile(h,cc_fd));
 if(extra_seed)for(unsigned k=0;k<3;k++)CHECK(!nvfuse_closefile(h,extra_alias[k]));return 0;
}
static int extra_verify(unsigned appended)
{
 unsigned size=extra_size+appended;char *b=malloc(size+1);CHECK(b);
 CHECK(nvfio_pread(cc_fd,b,size+1,0)==(int)size);
 for(unsigned k=0;k<size;k++)CHECK(b[k]==(k<extra_size?extra_pattern(k):(char)0x5a));
 CHECK(!nvfio_pread(cc_fd,b,1,size));free(b);return 0;
}
static int extra_cold(struct nvfuse_handle*h,void*unused)
{
 (void)unused;u64 key;inode_t ino=nvfuse_get_file_table(&h->nvh_sb,cc_fd)->ino;
 nvfuse_make_pbno_key(ino,extra_size/CLUSTER_SIZE,&key,NVFUSE_BP_TYPE_DATA);
 nvfuse_move_bc_to_unused_list(&h->nvh_sb,key);return 0;
}
static int extra_one(unsigned initial,int expected,int cold)
{
 extra_size=initial;extra_seed=1;CHECK(!nvfio_execute(extra_open,NULL));
 if(cold)CHECK(!nvfio_execute(extra_cold,NULL));
 nvfio_test_commit_pause(1,0);pthread_t sync;CHECK(!pthread_create(&sync,NULL,cc_sync_worker,NULL));CHECK(!cc_wait(0));
 char data[8193];memset(data,0x5a,sizeof(data));
 /* These probes cannot fall through to the blocking normal gate. */
 for(unsigned k=0;k<3;k++)CHECK(!nvfio_test_cc_tryappend(extra_alias[k],data,8192,extra_size));
 CHECK(!nvfio_test_cc_tryappend(cc_fd,data,8193,extra_size));
 CHECK(!nvfio_test_cc_tryappend(cc_fd,data,16,1));
 int n=nvfio_test_cc_tryappend(cc_fd,data,8192,extra_size);
 if(n!=expected){nvfio_test_cc_dump(cc_fd);fprintf(stderr,"FAIL extra case=%d copied=%d expected=%d\n",extra_case,n,expected);}
 CHECK(n==expected);CHECK(!atomic_load(&cc_sync_done)||extra_case>0);
 nvfio_test_commit_release(1,0);void *rc;CHECK(!pthread_join(sync,&rc)&&!rc);
 if(n<8192)CHECK(nvfio_pwrite(cc_fd,data+n,8192-n,extra_size+n)==8192-n);
 CHECK(!extra_verify(8192));CHECK(!nvfio_fsync(cc_fd));CHECK(!nvfio_execute(extra_close,NULL));
 printf("PASS commit cache shape case=%d initial=%u cache_only=%d cold=%d aliases/limits/payload/EOF\n",extra_case,initial,n,cold);fflush(stdout);return 0;
}
static int extra_arm_fault(struct nvfuse_handle*h,void*unused)
{
 (void)unused;int marker=nvfuse_openfile_path(h,"/commit-cache-before-failed-log",O_CREAT|O_RDWR,0600);CHECK(marker>=0);
 CHECK(!nvfuse_closefile(h,marker));nvj_set_io_fault_hook(h->nvh_target->journal,wb_fault_payload,h->nvh_target->journal);return 0;
}
int main(int argc,char**argv)
{
 CHECK(argc==4);CHECK(!nvfio_start(argv[1],argv[2],0));CHECK(!nvfio_test_pause_periodic_journal());
 if(!strcmp(argv[3],"shapes")){
  extra_case=10;atomic_store(&cc_sync_done,0);CHECK(!extra_one(4097,8192,0));
  extra_case++;CHECK(!extra_one(12288,8192,0));
  extra_case++;CHECK(!extra_one((DIRECT_BLOCKS+4)*CLUSTER_SIZE+37,8192,0));
  extra_case++;CHECK(!extra_one((DIRECT_BLOCKS+PTRS_PER_BLOCK)*CLUSTER_SIZE-9,9,0));
  extra_case++;CHECK(!extra_one(4097,0,1));
  CHECK(!nvfio_stop());puts("PASS commit cache guarded direct/indirect/three-page/partial/cold shapes");return 0;
 }
 extra_case=99;extra_size=8192;
 if(!strcmp(argv[3],"fail")){
  extra_seed=1;CHECK(!nvfio_execute(extra_open,NULL));CHECK(!nvfio_execute(extra_arm_fault,NULL));
  nvfio_test_commit_pause(1,0);pthread_t sync;CHECK(!pthread_create(&sync,NULL,cc_sync_worker,NULL));CHECK(!cc_wait(0));
  char b[4096];memset(b,0x5a,sizeof(b));CHECK(nvfio_test_cc_tryappend(cc_fd,b,sizeof(b),extra_size)==sizeof(b));
  nvfio_test_commit_release(1,0);void *rc;CHECK(!pthread_join(sync,&rc));CHECK((intptr_t)rc<0);
  puts("EXPECTED_IO_FAILURE commit-cache-failed-log-with-concurrent-next-transaction-append");fflush(NULL);_exit(86);
 }
 CHECK(!strcmp(argv[3],"fail-check"));extra_seed=0;CHECK(!nvfio_execute(extra_open,NULL));
 CHECK(!extra_verify(0));CHECK(!nvfio_execute(extra_close,NULL));CHECK(!nvfio_stop());
 puts("PASS commit cache failed-log preserved acknowledged prefix and EOF reopened");return 0;
}
