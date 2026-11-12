// Independent namespace and byte-content oracle over the real filesystem API.
#include "fs_model_adapter.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
static unsigned step;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL step=%u line=%d: %s\n",step,__LINE__,#x); std::exit(1); } } while(0)
static uint32_t state;
static uint32_t random_value() { state^=state<<13;state^=state>>17;state^=state<<5;return state; }
static std::string directory(unsigned d) { return "/arena/d"+std::to_string(d); }
static std::string file(unsigned d,unsigned n) { return directory(d)+"/f"+std::to_string(n); }
using Files=std::map<std::string,std::vector<unsigned char>>;
static void verify(void *h,const Files &files,const std::set<unsigned> &dirs) {
    uint64_t size;unsigned links;
    for(unsigned d=0;d<12;d++) {
        auto parent=directory(d);
        int rc=model_stat(h,parent.c_str(),&size,&links);
        if(!dirs.count(d)) { CHECK(rc<0); continue; }
        unsigned children=0;
        for(unsigned n=0;n<32;n++) {
            auto path=file(d,n);auto it=files.find(path);
            rc=model_stat(h,path.c_str(),&size,&links);
            if(it==files.end()) { CHECK(rc<0); continue; }
            CHECK(rc==0 && size==it->second.size()); children++;
            std::vector<unsigned char> data(it->second.size()+1,0xee);
            CHECK(model_read(h,path.c_str(),data.data(),static_cast<unsigned>(data.size()))==static_cast<int>(it->second.size()));
            CHECK(std::equal(it->second.begin(),it->second.end(),data.begin()));
        }
        CHECK(model_stat(h,parent.c_str(),&size,&links)==0 && links==children+2);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==5); const char *path=argv[1];state=static_cast<uint32_t>(std::stoul(argv[2]));CHECK(state);
    unsigned rounds=std::stoul(argv[3]);int meta=std::stoi(argv[4]);
    std::printf("SEED %u rounds=%u metadata_only=%d\n",state,rounds,meta);std::fflush(stdout);
    void *h=model_open(path,1,meta);CHECK(h);
    uint64_t base_b,base_i,b,i;CHECK(model_counts(h,&base_b,&base_i)==0);
    CHECK(model_directory_limit(h)==0);CHECK(model_counts(h,&b,&i)==0 && b==base_b && i==base_i);
    CHECK(model_mkdir(h,"/arena")==0);
    Files files;std::set<unsigned> dirs;
    unsigned completed[7]={};
    for(step=0;step<rounds;step++) {
        unsigned d=random_value()%12,n=random_value()%32,op=random_value()%7;
        auto parent=directory(d),p=file(d,n);auto it=files.find(p);
        if(op==0 && !dirs.count(d)) { CHECK(model_mkdir(h,parent.c_str())==0);dirs.insert(d);completed[op]++; }
        else if(op==1 && dirs.count(d) && it==files.end()) { CHECK(model_create(h,p.c_str())==0);files[p]={};completed[op]++; }
        else if(op==2 && it!=files.end()) { CHECK(model_unlink(h,p.c_str())==0);files.erase(it);completed[op]++; }
        else if(op==3 && it!=files.end()) {
            unsigned target=random_value()%12;auto q=file(target,random_value()%32);
            if(dirs.count(target) && q!=p) {
                auto value=it->second;CHECK(model_rename(h,p.c_str(),q.c_str())==0);
                files.erase(p);files[q]=std::move(value);completed[op]++;
            }
        } else if(op==4 && it!=files.end()) {
            size_t count=1+random_value()%16384,offset=it->second.empty()?0:random_value()%(it->second.size()+1);
            if(offset+count>131072) continue;
            std::vector<unsigned char> data(count);
            for(auto &byte:data) byte=static_cast<unsigned char>(1+random_value()%251);
            CHECK(model_write(h,p.c_str(),offset,data.data(),static_cast<unsigned>(count))==static_cast<int>(count));
            it->second.resize(std::max(it->second.size(),offset+count));
            if(meta) std::fill(data.begin(),data.end(),0);
            std::copy(data.begin(),data.end(),it->second.begin()+offset);completed[op]++;
        } else if(op==5 && it!=files.end()) {
            auto cut=it->second.empty()?0:random_value()%(it->second.size()+1);
            CHECK(model_truncate(h,p.c_str(),cut)==0);it->second.resize(cut);completed[op]++;
        } else if(op==6 && dirs.count(d)) {
            bool empty=std::none_of(files.begin(),files.end(),[&](const auto &v){return v.first.rfind(parent+"/",0)==0;});
            int rc=model_rmdir(h,parent.c_str());
            if(empty) {CHECK(rc==0);dirs.erase(d);completed[op]++;} else CHECK(rc<0);
        }
        verify(h,files,dirs);
        if(step%100==99) {
            CHECK(model_close(h)==0);h=model_open(path,0,meta);CHECK(h);verify(h,files,dirs);
            std::printf("PROGRESS step=%u files=%zu\n",step,files.size());std::fflush(stdout);
        }
    }
    for(const auto &entry:files) CHECK(model_unlink(h,entry.first.c_str())==0);
    for(auto d:dirs) CHECK(model_rmdir(h,directory(d).c_str())==0);
    CHECK(model_rmdir(h,"/arena")==0);CHECK(model_counts(h,&b,&i)==0 && b==base_b && i==base_i);
    CHECK(model_close(h)==0);h=model_open(path,0,meta);CHECK(h);
    CHECK(model_counts(h,&b,&i)==0 && b==base_b && i==base_i);CHECK(model_close(h)==0);
    for(unsigned op=0;op<7;op++) {std::printf("COVERAGE op=%u count=%u\n",op,completed[op]);CHECK(completed[op]>0);}
    std::puts("PASS mixed namespace/content model and complete reclamation");
}
