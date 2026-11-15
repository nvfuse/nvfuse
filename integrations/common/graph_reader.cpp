/* SPDX-License-Identifier: GPL-2.0-only */
/* Version-pinned ELF method replacement for diskannpy 0.7.0 and
 * Knowhere v2.6.4's embedded DiskANN. This is NOT an upstream plugin API.
 * No class layout/vtable is recreated or patched. The exported member entry
 * points receive the original `this` and the pinned 24-byte AlignedRead vector.
 * Graph data reads use only NVFUSE+SPDK. Auxiliary file lifecycle stays POSIX.
 */
#include <atomic>
#include <cerrno>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
extern "C" {
int nvai_start(int);
int nvai_stop(void);
int nvai_open_read(const char *);
int nvai_close(int);
void *nvai_context_create(void);
int nvai_context_free(void *);
int nvai_transfer_batch_context(void *,int,void **,const size_t *,const uint64_t *,const int *,unsigned);
}
/* Field offsets and size checked against both pinned upstream headers. */
struct AlignedRead {uint64_t offset;uint64_t len;void *buf;};
static_assert(sizeof(AlignedRead)==24 && offsetof(AlignedRead,buf)==16,"reader ABI");
struct File {int fd;std::string path;~File(){if(fd>=0)nvai_close(fd);}};
struct State {
    std::mutex mutex;
    bool started=false,finished=false;
    std::atomic<unsigned> active{0};
    std::atomic<unsigned> contexts_created{0};
    std::vector<std::pair<std::string,std::string>> routes;
    std::unordered_map<void *,std::shared_ptr<File>> files;
    std::vector<void *> contexts;
    std::atomic<uint64_t> opens{0},batches{0},reads{0},bytes{0},maximum{0};
};
static State &state(){static State *s=new State;return *s;}
static void check(int rc){if(rc<0)throw std::runtime_error(std::string("NVAI native reader: ")+strerror(-rc));}
static void evidence(const char *event,const std::string &path="") {
    State &s=state();const char *name=getenv("NVAI_READER_EVIDENCE");if(!name)return;
    int fd=::open(name,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);if(fd<0)return;
    // Routes accept no quotes/newlines; this remains valid JSONL.
    char out[4096];int n=snprintf(out,sizeof(out),
      "{\"event\":\"%s\",\"pid\":%d,\"path\":\"%s\",\"opens\":%llu,\"batches\":%llu,\"reads\":%llu,\"bytes\":%llu,\"max_batch\":%llu,\"contexts_created\":%u}\n",
      event,getpid(),path.c_str(),(unsigned long long)s.opens.load(),(unsigned long long)s.batches.load(),(unsigned long long)s.reads.load(),(unsigned long long)s.bytes.load(),(unsigned long long)s.maximum.load(),s.contexts_created.load());
    if(n>0 && (size_t)n<sizeof(out)) {ssize_t ignored=::write(fd,out,n);(void)ignored;}
    ::close(fd);
}
extern "C" int nvai_reader_shutdown();
static void control_server() {
    const char *path=getenv("NVAI_READER_CONTROL");if(!path)return;
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return;
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    struct sockaddr_un address={};address.sun_family=AF_UNIX;
    if(strlen(path)>=sizeof(address.sun_path)){::close(fd);return;}
    strcpy(address.sun_path,path);
    // Never unlink an existing endpoint; only our isolated run may own it.
    if(bind(fd,(struct sockaddr *)&address,sizeof(address)) || listen(fd,1)){::close(fd);return;}
    for(;;) {
        int peer=accept(fd,nullptr,nullptr);if(peer<0)continue;
        fcntl(peer,F_SETFD,FD_CLOEXEC);
        char command[32]={};ssize_t n=recv(peer,command,sizeof(command)-1,0);
        int rc=-EINVAL;
        if(n==8 && !memcmp(command,"shutdown",8))rc=nvai_reader_shutdown();
        char response[32];int bytes=snprintf(response,sizeof(response),"%d\n",rc);
        send(peer,response,bytes,MSG_NOSIGNAL);::close(peer);
        if(!rc)break;
    }
    ::close(fd);unlink(path);
}
static void initialize_locked() {
    State &s=state();if(s.finished)throw std::runtime_error("native reader already shut down");if(s.started)return;
    const char *map=getenv("NVAI_READER_MAP");if(!map)throw std::runtime_error("NVAI_READER_MAP required; no POSIX fallback");
    std::ifstream input(map);if(!input)throw std::runtime_error("cannot open native reader route map");
    std::string line;
    while(std::getline(input,line)) {
        size_t tab=line.find('\t');
        if(tab==std::string::npos || !tab || tab+1==line.size() || line.find_first_of("\"\r")!=std::string::npos)
            throw std::runtime_error("invalid native reader route");
        s.routes.emplace_back(line.substr(0,tab),line.substr(tab+1));
    }
    if(s.routes.empty())throw std::runtime_error("empty native reader route map");
    check(nvai_start(0));s.started=true;
    if(getenv("NVAI_READER_CONTROL"))std::thread(control_server).detach();
    evidence("started");
}
extern "C" void native_open(void *,const std::string &)
    asm("_ZN22LinuxAlignedFileReader4openERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
extern "C" void native_open(void *self,const std::string &path) {
    State &s=state();std::lock_guard<std::mutex> lock(s.mutex);initialize_locked();
    std::string destination;
    for(const auto &route:s.routes) {
        const std::string &suffix=route.first;
        if(path.size()>=suffix.size() && path.compare(path.size()-suffix.size(),suffix.size(),suffix)==0) {
            if(!destination.empty())throw std::runtime_error("ambiguous native reader route");
            destination=route.second;
        }
    }
    if(destination.empty())throw std::runtime_error("unmapped DiskANN graph; POSIX fallback prohibited: "+path);
    if(s.files.count(self))throw std::runtime_error("reader reopened without close");
    int fd=nvai_open_read(destination.c_str());check(fd);
    auto file=std::make_shared<File>();file->fd=fd;file->path=destination;
    s.files.emplace(self,file);s.opens++;evidence("open",destination);
}
extern "C" void native_close(void *) asm("_ZN22LinuxAlignedFileReader5closeEv");
extern "C" void native_close(void *self) {
    State &s=state();std::lock_guard<std::mutex> lock(s.mutex);
    s.files.erase(self);evidence("close");
}
static void *context() {
    static thread_local void *channel=nullptr;
    if(!channel) {
        State &s=state();std::lock_guard<std::mutex> lock(s.mutex);
        unsigned long limit=32;const char *setting=getenv("NVAI_READER_MAX_CONTEXTS");
        if(setting){char *end=nullptr;errno=0;limit=strtoul(setting,&end,10);if(errno || !*setting || *end || limit<1 || limit>256)throw std::runtime_error("invalid NVAI_READER_MAX_CONTEXTS");}
        if(s.contexts.size()>=limit)throw std::runtime_error("native reader thread-context limit");
        channel=nvai_context_create();if(!channel)throw std::runtime_error("native reader DMA/client allocation failed");
        s.contexts.push_back(channel);s.contexts_created++;
    }
    return channel;
}
extern "C" void native_read(void *,std::vector<AlignedRead> &,void *&,bool)
    asm("_ZN22LinuxAlignedFileReader4readERSt6vectorI11AlignedReadSaIS1_EERP10io_contextb");
extern "C" void native_read(void *self,std::vector<AlignedRead> &reads,void *&ignored_ctx,bool async) {
    (void)ignored_ctx;(void)async; // Both pinned Linux read() implementations block.
    State &s=state();std::shared_ptr<File> file;
    {std::lock_guard<std::mutex> lock(s.mutex);file=s.files.at(self);s.active++;}
    // Close the final file reference before exposing active==0 to shutdown.
    struct Active {State &s;std::shared_ptr<File> &file;
        ~Active(){file.reset();s.active--;}} active{s,file};
    if(reads.empty())return;
    void *channel=context();uint64_t total=0;
    for(size_t start=0;start<reads.size();start+=1024) {
        unsigned n=std::min<size_t>(1024,reads.size()-start);
        std::vector<void *> buffers(n);std::vector<size_t> lengths(n);
        std::vector<uint64_t> offsets(n);std::vector<int> writes(n,0);
        for(unsigned i=0;i<n;i++) {auto &r=reads[start+i];buffers[i]=r.buf;lengths[i]=r.len;offsets[i]=r.offset;total+=r.len;}
        check(nvai_transfer_batch_context(channel,file->fd,buffers.data(),lengths.data(),offsets.data(),writes.data(),n));
    }
    s.reads+=reads.size();s.bytes+=total;
    uint64_t maximum=s.maximum.load();while(reads.size()>maximum && !s.maximum.compare_exchange_weak(maximum,reads.size())){}
    uint64_t batches=++s.batches;if(batches==1 || batches%1024==0)evidence("progress");
}
/* Reject untested async-only/AiSAQ entry points rather than silently retaining
 * a kernel I/O path. cached_beam_search uses the blocking read() method above. */
extern "C" void native_submit(void *,void *&,std::vector<AlignedRead> &)
    asm("_ZN22LinuxAlignedFileReader10submit_reqERP10io_contextRSt6vectorI11AlignedReadSaIS4_EE");
extern "C" void native_submit(void *,void *&,std::vector<AlignedRead> &) {
    throw std::runtime_error("NVAI reader: async submit_req/AiSAQ unsupported");
}
extern "C" void native_get(void *,void *&,size_t)
    asm("_ZN22LinuxAlignedFileReader17get_submitted_reqERP10io_contextm");
extern "C" void native_get(void *,void *&,size_t) {
    throw std::runtime_error("NVAI reader: async get_submitted_req/AiSAQ unsupported");
}
extern "C" void nvai_reader_snapshot(){evidence("snapshot");}
extern "C" int nvai_reader_shutdown() {
    State &s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(!s.started)return 0;
    if(s.active.load() || !s.files.empty())return -EBUSY;
    evidence("shutdown");
    for(void *channel:s.contexts)check(nvai_context_free(channel));
    s.contexts.clear();int rc=nvai_stop();if(!rc){s.started=false;s.finished=true;}return rc;
}
__attribute__((destructor)) static void finish() {
    try{nvai_reader_shutdown();}catch(const std::exception &e){fprintf(stderr,"%s\n",e.what());}
}
