/* SPDX-License-Identifier: GPL-2.0-only */
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <time.h>
#include <sys/stat.h>
#include "nvbridge.h"
#ifdef KV_ROCKS
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/cache.h"
#include "rocksdb/table.h"
#include "rocksdb/write_batch.h"
namespace kv=rocksdb;
#define APP_NAME "rocksdb-9.10.0"
#else
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/cache.h"
#include "leveldb/write_batch.h"
#include "util/env_posix_test_helper.h"
namespace leveldb { class EnvPosixTest {public:static void DisableMmap(){EnvPosixTestHelper::SetReadOnlyMMapLimit(0);}}; }
namespace kv=leveldb;
#define APP_NAME "leveldb-1.23"
#endif
using kv::Status;using kv::Slice;
static Status status(int r,const std::string&op){if(r>=0)return Status::OK();if(r==-ENOENT)return Status::NotFound(op);return Status::IOError(op,std::to_string(r));}
#ifdef KV_NV
static Status read_at(int fd,uint64_t off,size_t n,Slice*out,char*buf){size_t done=0;while(done<n){size_t take=std::min(n-done,size_t(1<<20));int r=nvdb_read(fd,buf+done,take,off+done);if(r<0){*out=Slice();return status(r,"read");}done+=r;if(size_t(r)<take)break;}*out=Slice(buf,done);return Status::OK();}
class Seq final:public kv::SequentialFile{int fd_;uint64_t off_=0;public:explicit Seq(int f):fd_(f){}~Seq()override{nvdb_close(fd_);}Status Read(size_t n,Slice*r,char*b)override{auto s=read_at(fd_,off_,n,r,b);off_+=r->size();return s;}Status Skip(uint64_t n)override{off_+=n;return Status::OK();}};
class Random final:public kv::RandomAccessFile{int fd_;public:explicit Random(int f):fd_(f){}~Random()override{nvdb_close(fd_);}Status Read(uint64_t o,size_t n,Slice*r,char*b)const override{return read_at(fd_,o,n,r,b);}};
class Writable final:public kv::WritableFile{
 int fd_;uint64_t off_;std::string buffer_;
 Status Write(const Slice&d){size_t done=0;while(done<d.size()){size_t take=std::min(d.size()-done,size_t(1<<20));int r=nvdb_write(fd_,d.data()+done,take,off_);if(r<0)return status(r,"append");if(!r)return Status::IOError("short append");done+=r;off_+=r;}return Status::OK();}
public:Writable(int f,uint64_t o=0):fd_(f),off_(o){buffer_.reserve(65536);}~Writable()override{if(fd_>=0)Close();}
 Status Append(const Slice&d)override{if(buffer_.size()+d.size()>65536){auto s=Flush();if(!s.ok())return s;}if(d.size()>=65536)return Write(d);buffer_.append(d.data(),d.size());return Status::OK();}
 Status Close()override{if(fd_<0)return Status::OK();auto s=Flush();int r=nvdb_close(fd_);fd_=-1;return s.ok()?status(r,"close"):s;}
 Status Flush()override{if(buffer_.empty())return Status::OK();auto s=Write(Slice(buffer_));if(s.ok())buffer_.clear();return s;}
 Status Sync()override{auto s=Flush();return s.ok()?status(nvdb_sync(fd_),"sync"):s;}
#ifdef KV_ROCKS
 Status Fsync()override{return Sync();}uint64_t GetFileSize()override{return off_+buffer_.size();}Status Truncate(uint64_t n)override{auto s=Flush();if(!s.ok())return s;s=status(nvdb_truncate(fd_,n),"truncate");if(s.ok()&&off_>n)off_=n;return s;}
#endif
};
class Lock final:public kv::FileLock{public:std::string name;explicit Lock(std::string n):name(std::move(n)){} };
#ifdef KV_ROCKS
class Dir final:public kv::Directory{public:Status Fsync()override{return status(nvdb_sync(-1),"directory sync");}Status Close()override{return Status::OK();}};
#endif
class NvEnv final:public kv::EnvWrapper{std::mutex mu_;std::set<std::string>locks_;
public:NvEnv():kv::EnvWrapper(kv::Env::Default()){}
#ifdef KV_ROCKS
const char*Name()const override{return "nvfuse-experiment";}
Status NewSequentialFile(const std::string&p,std::unique_ptr<kv::SequentialFile>*r,const kv::EnvOptions&)override{r->reset();int fd=nvdb_open(p.c_str(),O_RDONLY);if(fd<0)return status(fd,p);r->reset(new Seq(fd));return Status::OK();}
Status NewRandomAccessFile(const std::string&p,std::unique_ptr<kv::RandomAccessFile>*r,const kv::EnvOptions&)override{r->reset();int fd=nvdb_open(p.c_str(),O_RDONLY);if(fd<0)return status(fd,p);r->reset(new Random(fd));return Status::OK();}
Status NewWritableFile(const std::string&p,std::unique_ptr<kv::WritableFile>*r,const kv::EnvOptions&)override{r->reset();int fd=nvdb_open(p.c_str(),O_RDWR|O_CREAT|O_TRUNC);if(fd<0)return status(fd,p);r->reset(new Writable(fd));return Status::OK();}
Status ReopenWritableFile(const std::string&p,std::unique_ptr<kv::WritableFile>*r,const kv::EnvOptions&)override{r->reset();int fd=nvdb_open(p.c_str(),O_RDWR|O_CREAT);if(fd<0)return status(fd,p);uint64_t size;int rc=nvdb_stat(p.c_str(),&size,nullptr,nullptr);if(rc){nvdb_close(fd);return status(rc,p);}r->reset(new Writable(fd,size));return Status::OK();}
Status ReuseWritableFile(const std::string&p,const std::string&o,std::unique_ptr<kv::WritableFile>*r,const kv::EnvOptions&opts)override{auto s=RenameFile(o,p);if(!s.ok())return s;return NewWritableFile(p,r,opts);}
Status NewDirectory(const std::string&p,std::unique_ptr<kv::Directory>*r)override{r->reset();unsigned mode;int rc=nvdb_stat(p.c_str(),nullptr,nullptr,&mode);if(rc)return status(rc,p);if(!S_ISDIR(mode))return Status::IOError("not directory",p);r->reset(new Dir);return Status::OK();}
Status FileExists(const std::string&p)override{return status(nvdb_stat(p.c_str(),nullptr,nullptr,nullptr),p);}
Status CreateDirIfMissing(const std::string&p)override{int rc=nvdb_mkdir(p.c_str());if(rc==-EEXIST){unsigned mode;rc=nvdb_stat(p.c_str(),nullptr,nullptr,&mode);if(!rc&&!S_ISDIR(mode))rc=-ENOTDIR;}return status(rc,p);}
Status GetFileModificationTime(const std::string&p,uint64_t*t)override{return status(nvdb_stat(p.c_str(),nullptr,t,nullptr),p);}
Status GetChildrenFileAttributes(const std::string&p,std::vector<kv::Env::FileAttributes>*r)override{r->clear();std::vector<std::string>v;auto s=GetChildren(p,&v);if(!s.ok())return s;for(auto&n:v){uint64_t size;auto t=GetFileSize(p+"/"+n,&size);if(t.IsNotFound())continue;if(!t.ok())return t;kv::Env::FileAttributes a;a.name=n;a.size_bytes=size;r->push_back(a);}return Status::OK();}
Status GetAbsolutePath(const std::string&p,std::string*r)override{if(p.empty()||p[0]!='/')return Status::InvalidArgument("absolute path required");*r=p;return Status::OK();}
Status IsDirectory(const std::string&p,bool*r)override{unsigned mode;auto s=status(nvdb_stat(p.c_str(),nullptr,nullptr,&mode),p);if(s.ok())*r=S_ISDIR(mode);return s;}
Status GetFreeSpace(const std::string&,uint64_t*)override{return Status::NotSupported("free-space query");}
Status Truncate(const std::string&p,size_t n)override{int f=nvdb_open(p.c_str(),O_RDWR);if(f<0)return status(f,p);int rc=nvdb_truncate(f,n);int c=nvdb_close(f);return status(rc?rc:c,p);}
Status LinkFile(const std::string&a,const std::string&b)override{return status(nvdb_link(a.c_str(),b.c_str()),a);}
Status NumFileLinks(const std::string&,uint64_t*)override{return Status::NotSupported("link count");}
Status AreFilesSame(const std::string&,const std::string&,bool*)override{return Status::NotSupported("inode identity");}
Status NewRandomRWFile(const std::string&,std::unique_ptr<kv::RandomRWFile>*,const kv::EnvOptions&)override{return Status::NotSupported("random read-write file");}
Status NewMemoryMappedFileBuffer(const std::string&,std::unique_ptr<kv::MemoryMappedFileBuffer>*)override{return Status::NotSupported("mmap");}
#else
Status NewSequentialFile(const std::string&p,kv::SequentialFile**r)override{*r=nullptr;int fd=nvdb_open(p.c_str(),O_RDONLY);if(fd<0)return status(fd,p);*r=new Seq(fd);return Status::OK();}
Status NewRandomAccessFile(const std::string&p,kv::RandomAccessFile**r)override{*r=nullptr;int fd=nvdb_open(p.c_str(),O_RDONLY);if(fd<0)return status(fd,p);*r=new Random(fd);return Status::OK();}
Status NewWritableFile(const std::string&p,kv::WritableFile**r)override{*r=nullptr;int fd=nvdb_open(p.c_str(),O_RDWR|O_CREAT|O_TRUNC);if(fd<0)return status(fd,p);*r=new Writable(fd);return Status::OK();}
Status NewAppendableFile(const std::string&p,kv::WritableFile**r)override{*r=nullptr;int fd=nvdb_open(p.c_str(),O_RDWR|O_CREAT);if(fd<0)return status(fd,p);uint64_t size;int rc=nvdb_stat(p.c_str(),&size,nullptr,nullptr);if(rc){nvdb_close(fd);return status(rc,p);}*r=new Writable(fd,size);return Status::OK();}
bool FileExists(const std::string&p)override{return !nvdb_stat(p.c_str(),nullptr,nullptr,nullptr);}
Status RemoveFile(const std::string&p)override{return status(nvdb_unlink(p.c_str()),p);}
Status RemoveDir(const std::string&p)override{return status(nvdb_rmdir(p.c_str()),p);}
#endif
Status DeleteFile(const std::string&p)override{return status(nvdb_unlink(p.c_str()),p);}
Status CreateDir(const std::string&p)override{return status(nvdb_mkdir(p.c_str()),p);}
Status DeleteDir(const std::string&p)override{return status(nvdb_rmdir(p.c_str()),p);}
Status GetFileSize(const std::string&p,uint64_t*n)override{return status(nvdb_stat(p.c_str(),n,nullptr,nullptr),p);}
Status RenameFile(const std::string&a,const std::string&b)override{return status(nvdb_rename(a.c_str(),b.c_str()),a);}
Status GetChildren(const std::string&p,std::vector<std::string>*r)override{r->clear();char**names=nullptr;size_t n=0;int rc=nvdb_list(p.c_str(),&names,&n);for(size_t i=0;i<n;i++){if(!rc)r->emplace_back(names[i]);free(names[i]);}free(names);return status(rc,p);}
Status LockFile(const std::string&p,kv::FileLock**r)override{*r=nullptr;std::lock_guard<std::mutex>g(mu_);if(locks_.count(p))return Status::IOError("already locked",p);int fd=nvdb_open(p.c_str(),O_RDWR|O_CREAT);if(fd<0)return status(fd,p);int rc=nvdb_close(fd);if(rc)return status(rc,p);locks_.insert(p);*r=new Lock(p);return Status::OK();}
Status UnlockFile(kv::FileLock*p)override{std::lock_guard<std::mutex>g(mu_);auto*l=static_cast<Lock*>(p);locks_.erase(l->name);delete l;return Status::OK();}
};
#endif
class QuietLogger final:public kv::Logger{public:void Logv(const char*,va_list)override{}};
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static double cpu(){timespec t;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void check(const Status&s){if(!s.ok()){fprintf(stderr,"FAIL %s\n",s.ToString().c_str());exit(2);}}
static uint64_t next(uint64_t&r){r=r*6364136223846793005ULL+1;return r>>32;}
static std::string key(int k){char b[32];snprintf(b,sizeof(b),"%016d",k);return b;}
static std::string value(int k,int rev){std::string v(1024,'\0');uint64_t x=uint64_t(k+1)*0x9e3779b97f4a7c15ULL^uint64_t(rev)*0xd6e8feb86659fd93ULL;for(size_t j=0;j<v.size();j++){x^=x>>12;x^=x<<25;x^=x>>27;v[j]=char((x*2685821657736338717ULL)>>56);}return v;}
static void profile(const char*p){
#ifdef KV_NV
nvdb_profile(p);
#else
(void)p;
#endif
}
static void report(const char*phase,std::vector<double>&lat,int ops,double dt,double c){std::sort(lat.begin(),lat.end());size_t n=lat.size();printf("KVBENCH {\"application\":\"%s\",\"phase\":\"%s\",\"transactions\":%zu,\"operations\":%d,\"seconds\":%.6f,\"ops_s\":%.2f,\"tx_s\":%.2f,\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,\"process_cpu_seconds\":%.6f}\n",APP_NAME,phase,n,ops,dt,ops/dt,n/dt,lat[n/2]*1e6,lat[(n-1)*95/100]*1e6,lat[(n-1)*99/100]*1e6,c);fflush(stdout);profile(phase);}
static void drain(kv::DB*d,const char*phase){double t=now();
#ifdef KV_ROCKS
kv::FlushOptions f;f.wait=true;check(d->Flush(f));kv::CompactRangeOptions c;check(d->CompactRange(c,nullptr,nullptr));
#else
d->CompactRange(nullptr,nullptr);
#endif
printf("KVDRAIN {\"phase\":\"%s\",\"seconds\":%.6f}\n",phase,now()-t);fflush(stdout);profile(phase);}
int main(int argc,char**argv){if(argc!=5&&argc!=8)return 1;std::string path=argv[1];int rows=atoi(argv[2]),iterations=atoi(argv[3]);bool verify=!strcmp(argv[4],"verify");const char*batch_env=getenv("KV_BATCH_ITERATIONS");int batch_iterations=batch_env?atoi(batch_env):iterations;if(batch_iterations<100||batch_iterations%100||batch_iterations>10000000)return 1;if(rows<100||rows%100||iterations<100||iterations%100)return 1;
#ifndef KV_ROCKS
leveldb::EnvPosixTest::DisableMmap();
#endif
#ifdef KV_NV
if(argc!=8)return 1;int start=nvdb_start(argv[5],argv[6],atoi(argv[7]));if(start){fprintf(stderr,"START %d\n",start);return 3;}NvEnv env;
#endif
kv::Options opts;opts.create_if_missing=!verify;opts.error_if_exists=!verify;opts.paranoid_checks=true;opts.write_buffer_size=4<<20;opts.compression=kv::kNoCompression;
#ifdef KV_NV
opts.env=&env;
#endif
#ifdef KV_ROCKS
opts.allow_mmap_reads=false;opts.allow_mmap_writes=false;opts.max_background_jobs=2;opts.max_open_files=256;opts.target_file_size_base=4<<20;opts.max_bytes_for_level_base=16<<20;opts.use_fsync=true;opts.bytes_per_sync=0;opts.wal_bytes_per_sync=0;opts.info_log=std::make_shared<QuietLogger>();kv::BlockBasedTableOptions table;table.block_cache=kv::NewLRUCache(16<<20);table.block_size=4096;opts.table_factory.reset(kv::NewBlockBasedTableFactory(table));
#else
std::unique_ptr<kv::Cache>cache(kv::NewLRUCache(16<<20));opts.block_cache=cache.get();opts.max_file_size=4<<20;opts.max_open_files=256;QuietLogger logger;opts.info_log=&logger;
#endif
kv::DB*raw=nullptr;check(kv::DB::Open(opts,path,&raw));std::unique_ptr<kv::DB>d(raw);profile("open");printf("KVCONFIG app=%s rows=%d value_bytes=1024 key_bytes=16 sync=1 WAL=on compression=none write_buffer_mib=4 cache_mib=16 mmap=off\n",APP_NAME,rows);fflush(stdout);
kv::WriteOptions w;w.sync=true;kv::ReadOptions ro;ro.verify_checksums=true;ro.fill_cache=true;std::vector<int>expected(rows,0);uint64_t rng=42;for(int i=0;i<iterations*5;i++)next(rng);for(int i=0;i<iterations+batch_iterations;i++)expected[next(rng)%rows]=i+1;
if(!verify){std::vector<double>lat;double t=now(),c=cpu();for(int i=0;i<rows;i+=100){double a=now();kv::WriteBatch b;for(int j=i;j<i+100;j++)b.Put(key(j),value(j,0));check(d->Write(w,&b));lat.push_back(now()-a);}report("load-100",lat,rows,now()-t,cpu()-c);drain(d.get(),"load-flush-compact");
rng=42;lat.clear();t=now();c=cpu();for(int i=0;i<iterations*5;i++){int k=next(rng)%rows;double a=now();std::string got;check(d->Get(ro,key(k),&got));if(got!=value(k,0)){fprintf(stderr,"READ MISMATCH %d\n",k);return 4;}lat.push_back(now()-a);}report("random-read-warm",lat,iterations*5,now()-t,cpu()-c);
int serial=0;for(int batch:{1,100}){int limit=batch==1?iterations:batch_iterations;lat.clear();if(batch==100)profile("batch-begin");t=now();c=cpu();for(int i=0;i<limit;i+=batch){double a=now();kv::WriteBatch b;for(int j=0;j<batch;j++){int k=next(rng)%rows;b.Put(key(k),value(k,++serial));}check(d->Write(w,&b));lat.push_back(now()-a);}report(batch==1?"update-1":"update-100",lat,limit,now()-t,cpu()-c);} }
// Full ordered scan validates every key and every value byte, including per-key revisions.
auto it=std::unique_ptr<kv::Iterator>(d->NewIterator(ro));int seen=0;for(it->SeekToFirst();it->Valid();it->Next()){if(seen>=rows||it->key()!=key(seen)||it->value()!=value(seen,expected[seen])){fprintf(stderr,"VERIFY MISMATCH row=%d\n",seen);return 4;}seen++;}check(it->status());it.reset();if(seen!=rows)return 4;printf("KVVERIFY app=%s rows=%d full_payload=ok per_key_revision=ok checksums=on\n",APP_NAME,seen);fflush(stdout);
// Preserve durable WAL-backed updates for a fresh-process reopen; no explicit final flush masks WAL recovery.
d.reset();profile("verify-close");
std::vector<std::string> files;check(opts.env->GetChildren(path,&files));unsigned sst=0;uint64_t bytes=0;for(const auto& f:files){uint64_t n;auto st=opts.env->GetFileSize(path+"/"+f,&n);if(st.IsNotFound())continue;check(st);bytes+=n;if(f.size()>4&&(f.substr(f.size()-4)==".sst"||f.substr(f.size()-4)==".ldb"))sst++;}printf("KVFILES entries=%zu sst=%u bytes=%llu\n",files.size(),sst,(unsigned long long)bytes);fflush(stdout);if(!sst)return 6;

#ifdef KV_NV
int stop=nvdb_stop();if(stop)return 5;
#endif
return 0;}
