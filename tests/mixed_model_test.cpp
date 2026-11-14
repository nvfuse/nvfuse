/* SPDX-License-Identifier: GPL-2.0-only */

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
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
static unsigned step;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL step=%u line=%d: %s\n",step,__LINE__,#x); std::exit(1); } } while(0)
static uint32_t state;
static uint32_t random_value() { state^=state<<13;state^=state>>17;state^=state<<5;return state; }
static std::string directory(unsigned d) { return "/arena/d"+std::to_string(d); }
static std::string file(unsigned d,unsigned n) { return directory(d)+"/f"+std::to_string(n); }
using Files=std::map<std::string,std::vector<unsigned char>>;
static void verify_listing(void *h,const std::string &path,const std::set<std::string> &expected) {
    std::vector<char> names((expected.size()+1)*116);unsigned count=0;
    CHECK(!model_list(h,path.c_str(),names.data(),static_cast<unsigned>(expected.size()+1),&count));
    std::set<std::string> actual;
    for(unsigned k=0;k<count;k++)CHECK(actual.insert(names.data()+k*116).second);
    CHECK(actual==expected);
}
static void verify(void *h,const Files &files,const std::set<unsigned> &dirs) {
    uint64_t size;unsigned links;
    verify_listing(h,"/",{"arena"});std::set<std::string> parents;
    for(auto d:dirs)parents.insert("d"+std::to_string(d));
    verify_listing(h,"/arena",parents);
    for(unsigned d=0;d<12;d++) {
        auto parent=directory(d);
        int rc=model_stat(h,parent.c_str(),&size,&links);
        if(!dirs.count(d)) { CHECK(rc<0); continue; }
        unsigned children=0;std::set<std::string> expected_names;
        for(unsigned n=0;n<32;n++) {
            auto path=file(d,n);auto it=files.find(path);
            rc=model_stat(h,path.c_str(),&size,&links);
            if(it==files.end()) { CHECK(rc<0); continue; }
            CHECK(rc==0 && size==it->second.size()); children++;expected_names.insert("f"+std::to_string(n));
            std::vector<unsigned char> data(it->second.size()+1,0xee);
            CHECK(model_read(h,path.c_str(),data.data(),static_cast<unsigned>(data.size()))==static_cast<int>(it->second.size()));
            CHECK(std::equal(it->second.begin(),it->second.end(),data.begin()));
        }
        CHECK(model_stat(h,parent.c_str(),&size,&links)==0 && links==children+2);
        verify_listing(h,parent,expected_names);
    }
}
static void verify_file(void *h,const Files &files,const std::string &path) {
    uint64_t size;unsigned links;auto it=files.find(path);
    int rc=model_stat(h,path.c_str(),&size,&links);
    if(it==files.end()) { CHECK(rc<0);return; }
    CHECK(rc==0 && size==it->second.size());
    std::vector<unsigned char> bytes(it->second.size()+1,0xee);
    CHECK(model_read(h,path.c_str(),bytes.data(),static_cast<unsigned>(bytes.size()))==static_cast<int>(it->second.size()));
    CHECK(std::equal(it->second.begin(),it->second.end(),bytes.begin()));
}
static void verify_parent(void *h,const Files &files,const std::set<unsigned> &dirs,unsigned d) {
    uint64_t size;unsigned links;auto path=directory(d);
    int rc=model_stat(h,path.c_str(),&size,&links);
    if(!dirs.count(d)) { CHECK(rc<0);return; }
    unsigned count=0;
    for(const auto &entry:files) count+=entry.first.rfind(path+"/",0)==0;
    CHECK(rc==0 && links==count+2);
}
template<class T> static void write_scalar(std::ofstream &out,const T &value) {out.write(reinterpret_cast<const char *>(&value),sizeof(value));CHECK(out.good());}
template<class T> static void read_scalar(std::ifstream &in,T &value) {in.read(reinterpret_cast<char *>(&value),sizeof(value));CHECK(in.good());}
static void checkpoint(const std::string &path,const Files &files,const std::set<unsigned> &dirs,uint64_t blocks,uint64_t inodes,const unsigned completed[7],int meta) {
    std::ofstream out(path+".next",std::ios::binary);CHECK(out.good());
    uint64_t magic=0x314b43454843564eULL;write_scalar(out,magic);write_scalar(out,state);write_scalar(out,step);write_scalar(out,meta);
    write_scalar(out,blocks);write_scalar(out,inodes);
    for(unsigned k=0;k<7;k++) write_scalar(out,completed[k]);
    uint32_t n=static_cast<uint32_t>(dirs.size());write_scalar(out,n);for(auto d:dirs) write_scalar(out,d);
    n=static_cast<uint32_t>(files.size());write_scalar(out,n);
    for(const auto &entry:files) {
        n=static_cast<uint32_t>(entry.first.size());write_scalar(out,n);out.write(entry.first.data(),n);
        n=static_cast<uint32_t>(entry.second.size());write_scalar(out,n);out.write(reinterpret_cast<const char *>(entry.second.data()),n);CHECK(out.good());
    }
    out.close();CHECK(out.good());int fd=open((path+".next").c_str(),O_RDONLY);CHECK(fd>=0 && fsync(fd)==0 && close(fd)==0);
    CHECK(std::rename((path+".next").c_str(),path.c_str())==0);
}
static void restore(const std::string &path,Files &files,std::set<unsigned> &dirs,uint64_t &blocks,uint64_t &inodes,unsigned completed[7],int meta) {
    std::ifstream in(path,std::ios::binary);CHECK(in.good());uint64_t magic;read_scalar(in,magic);CHECK(magic==0x314b43454843564eULL);
    read_scalar(in,state);read_scalar(in,step);int saved_meta;read_scalar(in,saved_meta);CHECK(saved_meta==meta);
    read_scalar(in,blocks);read_scalar(in,inodes);for(unsigned k=0;k<7;k++) read_scalar(in,completed[k]);
    uint32_t n;read_scalar(in,n);CHECK(n<=12);for(unsigned k=0;k<n;k++) {unsigned d;read_scalar(in,d);CHECK(d<12);dirs.insert(d);}
    read_scalar(in,n);CHECK(n<=384);
    for(unsigned k=0;k<n;k++) {
        uint32_t length;read_scalar(in,length);CHECK(length<256);std::string name(length,' ');in.read(name.data(),length);
        read_scalar(in,length);CHECK(length<=131072);std::vector<unsigned char> bytes(length);in.read(reinterpret_cast<char *>(bytes.data()),length);CHECK(in.good());files.emplace(std::move(name),std::move(bytes));
    }
}
int main(int argc,char **argv) {
    CHECK(argc==5 || argc==7 || argc==10); const char *path=argv[1];state=static_cast<uint32_t>(std::stoul(argv[2]));CHECK(state);
    unsigned rounds=std::stoul(argv[3]);int meta=std::stoi(argv[4]);
    unsigned verify_interval=argc>=7?std::stoul(argv[5]):1,remount_interval=argc>=7?std::stoul(argv[6]):100;
    CHECK(verify_interval && remount_interval);
    std::printf("SEED %u rounds=%u metadata_only=%d\n",state,rounds,meta);std::fflush(stdout);
    bool resume=argc==10 && std::stoi(argv[9]);
    void *h=model_open(path,!resume,meta);CHECK(h);
    uint64_t base_b=0,base_i=0,b,i;
    Files files;std::set<unsigned> dirs;unsigned completed[7]={};
    if(resume) {
        restore(argv[7],files,dirs,base_b,base_i,completed,meta);verify(h,files,dirs);
        std::printf("PASS checkpoint recovered step=%u files=%zu\n",step,files.size());std::fflush(stdout);
    } else {
        CHECK(model_counts(h,&base_b,&base_i)==0);
        CHECK(model_directory_limit(h)==0);CHECK(model_counts(h,&b,&i)==0 && b==base_b && i==base_i);
        CHECK(model_mkdir(h,"/arena")==0);
    }
    unsigned end=step+rounds;
    for(;step<end;step++) {
        unsigned d=random_value()%12,n=random_value()%32,op=random_value()%7;
        auto parent=directory(d),p=file(d,n);auto it=files.find(p);
        std::string other;unsigned other_dir=d;
        if(op==0 && !dirs.count(d)) { CHECK(model_mkdir(h,parent.c_str())==0);dirs.insert(d);completed[op]++; }
        else if(op==1 && dirs.count(d) && it==files.end()) { CHECK(model_create(h,p.c_str())==0);files[p]={};completed[op]++; }
        else if(op==2 && it!=files.end()) { CHECK(model_unlink(h,p.c_str())==0);files.erase(it);completed[op]++; }
        else if(op==3 && it!=files.end()) {
            unsigned target=random_value()%12;auto q=file(target,random_value()%32);
            if(dirs.count(target) && q!=p) {
                auto value=it->second;CHECK(model_rename(h,p.c_str(),q.c_str())==0);
                files.erase(p);files[q]=std::move(value);other=q;other_dir=target;completed[op]++;
            }
        } else if(op==4 && it!=files.end()) {
            size_t count=1+random_value()%16384,offset=it->second.empty()?0:random_value()%(it->second.size()+1);
            if(offset+count>131072) offset=0;
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
            // Sample expected failure in soak mode; each failure reloads the
            // legacy cache. The original exhaustive mode checks every attempt.
            if(empty || verify_interval==1 || step%4096==0) {
                int rc=model_rmdir(h,parent.c_str());
                if(empty) {CHECK(rc==0);dirs.erase(d);completed[op]++;} else CHECK(rc<0);
            }
        }
        verify_file(h,files,p);verify_parent(h,files,dirs,d);
        if(!other.empty()) {verify_file(h,files,other);verify_parent(h,files,dirs,other_dir);}
        if(step%verify_interval==0) verify(h,files,dirs);
        if(step%remount_interval==remount_interval-1) {
            CHECK(model_close(h)==0);h=model_open(path,0,meta);CHECK(h);verify(h,files,dirs);
            std::printf("PROGRESS step=%u files=%zu\n",step,files.size());std::fflush(stdout);
        }
    }
    verify(h,files,dirs);
    if(argc==10 && std::string(argv[8])!="finish") {
        CHECK(model_close(h)==0);
        std::string phase=argv[8];
        if(phase=="verify") {std::puts("PASS checkpoint verify");return 0;}
        CHECK(!files.empty());auto from=files.begin()->first;std::string to;
        for(auto d:dirs) for(unsigned n=0;n<32 && to.empty();n++) if(!files.count(file(d,n))) to=file(d,n);
        CHECK(!to.empty());
        bool committed=phase=="commit-flush" || phase=="checkpoint-write" || phase=="checkpoint-flush" || phase=="retire-flush";
        if(committed) {files[to]=files.at(from);files.erase(from);}
        checkpoint(argv[7],files,dirs,base_b,base_i,completed,meta);
        h=model_open(path,0,meta);CHECK(h);CHECK(model_set_crash(h,phase.c_str())==0);
        CHECK(model_rename(h,from.c_str(),to.c_str())==0);CHECK(!"crash hook did not fire");
    }
    for(const auto &entry:files) CHECK(model_unlink(h,entry.first.c_str())==0);
    for(auto d:dirs) CHECK(model_rmdir(h,directory(d).c_str())==0);
    CHECK(model_rmdir(h,"/arena")==0);CHECK(model_counts(h,&b,&i)==0 && b==base_b && i==base_i);
    CHECK(model_close(h)==0);h=model_open(path,0,meta);CHECK(h);
    CHECK(model_counts(h,&b,&i)==0 && b==base_b && i==base_i);CHECK(model_close(h)==0);
    for(unsigned op=0;op<7;op++) {std::printf("COVERAGE op=%u count=%u\n",op,completed[op]);CHECK(completed[op]>0);}
    std::puts("PASS mixed namespace/content model and complete reclamation");
}
