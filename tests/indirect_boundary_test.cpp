/* SPDX-License-Identifier: GPL-2.0-only */

// Full production geometry, compressed metadata only, no regular-file payload.
#include "indirect_adapter.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while (0)
constexpr uint64_t B=4096,P=1024,D=11,S=D+P,T=S+P*P,CAP=T+P*P*P;
constexpr uint64_t META=1+(1+P)+(1+P+P*P), TOTAL=CAP+META, END=uint64_t{1}<<32;
struct Run { uint32_t offset,count,first,step; };
struct Store {
    uint64_t begin,end,live=0,allocated=0,released=0,peak=0;
    std::map<uint64_t,uint64_t> free;
    std::map<uint32_t,std::vector<Run>> pages;
    explicit Store(uint64_t lo,uint64_t hi):begin(lo),end(hi) { CHECK(lo && lo<hi && hi<=END);free[lo]=hi; }
    bool owned(uint64_t p) const {
        if(p<begin || p>=end) return false;
        auto it=free.upper_bound(p); return it==free.begin() || std::prev(it)->second<=p;
    }
    void *get(uint32_t block,int write) {
        CHECK(owned(block)); auto it=pages.find(block);
        CHECK(write || it!=pages.end());
        auto *data=new uint32_t[P]{};
        if(it!=pages.end()) for(const auto &r:it->second)
            for(uint32_t k=0;k<r.count;k++) data[r.offset+k]=r.first+k*r.step;
        return data;
    }
    void put(uint32_t block,void *buffer,int dirty) {
        auto *data=static_cast<uint32_t *>(buffer);
        if(dirty) {
            if(!owned(block)) std::fprintf(stderr,"dirty release of unowned block=%u live=%llu\n",block,(unsigned long long)live);
            CHECK(owned(block)); std::vector<Run> runs;
            for(uint32_t i=0;i<P;) {
                if(!data[i]) { i++;continue; }
                uint32_t start=i++,step=i<P && data[i]?data[i]-data[start]:0;
                while(i<P && data[i] && data[i]==data[start]+(i-start)*step) i++;
                runs.push_back({start,i-start,data[start],step});
            }
            pages[block]=std::move(runs); peak=std::max<uint64_t>(peak,pages.size());
        }
        delete[] data;
    }
    uint32_t allocate(uint32_t *out,uint32_t requested) {
        uint32_t done=0;
        while(done<requested && !free.empty()) {
            auto it=free.begin(); uint64_t lo=it->first,hi=it->second;
            uint64_t n=std::min<uint64_t>(requested-done,hi-lo);free.erase(it);
            if(lo+n<hi) free[lo+n]=hi;
            for(uint64_t k=0;k<n;k++) out[done++]=static_cast<uint32_t>(lo+k);
            live+=n;allocated+=n;
        }
        return done;
    }
    void release(uint32_t block,uint32_t count) {
        CHECK(count && uint64_t(block)>=begin && uint64_t(block)+count<=end);
        uint64_t lo=block,hi=lo+count;
        auto next=free.lower_bound(lo);
        CHECK(next==free.end() || next->first>=hi);
        CHECK(next==free.begin() || std::prev(next)->second<=lo);
        auto page=pages.lower_bound(block);
        while(page!=pages.end() && uint64_t(page->first)<hi) page=pages.erase(page);
        CHECK(live>=count);live-=count;released+=count;
        if(next!=free.begin()) {
            auto prev=std::prev(next); if(prev->second==lo) { lo=prev->first; free.erase(prev); }
        }
        if(next!=free.end() && next->first==hi) { hi=next->second;free.erase(next); }
        free[lo]=hi;
    }
    indirect_ops ops() {
        return {this,
            [](void *p,uint32_t b,int w){return static_cast<Store *>(p)->get(b,w);},
            [](void *p,uint32_t b,void *d,int w){static_cast<Store *>(p)->put(b,d,w);},
            [](void *p,uint32_t *b,uint32_t n){return static_cast<Store *>(p)->allocate(b,n);},
            [](void *p,uint32_t b,uint32_t n){static_cast<Store *>(p)->release(b,n);},
            [](void *p){auto *s=static_cast<Store *>(p);return s->end-s->begin-s->live;}};
    }
    void empty() const { CHECK(!live && pages.empty() && allocated==released && free.size()==1 && free.begin()->first==begin && free.begin()->second==end); }
};
struct Test {
    Store store;
    indirect_test *fs;
    Test(uint64_t lo,uint64_t hi):store(lo,hi),fs(indirect_test_open(store.ops())) { CHECK(fs); }
    ~Test() { indirect_test_close(fs); }
    uint32_t map(uint64_t logical,bool create,uint32_t max=1,uint32_t *count=nullptr) {
        CHECK(logical<=INT32_MAX);uint32_t n=0,p=0;
        CHECK(!indirect_test_map(fs,static_cast<int32_t>(logical),max,&n,&p,create));
        if(count) *count=n;return p;
    }
    void empty() const {store.empty();for(unsigned k=0;k<14;k++) CHECK(!indirect_test_root(fs,k));}
};
static uint64_t span(uint64_t n) { return n<D?D-n:P-((n-(n<S?D:n<T?S:T))%P); }
static uint64_t physical(uint64_t base,uint64_t n) {
    uint64_t meta=n<D?0:n<S?1:n<T?3+(n-S)/P:1029+(n-T)/P+(n-T)/(P*P);
    return base+n+meta;
}
static void paths() {
    const int64_t points[]={INT32_MIN,-1,0,10,11,1034,1035,int64_t(T-1),int64_t(T),int64_t(CAP-1),int64_t(CAP),INT32_MAX};
    for(auto n:points) {
        uint32_t offsets[4]={},boundary=0;
        int depth=indirect_test_path(static_cast<int32_t>(n),offsets,&boundary);
        if(n<0 || uint64_t(n)>=CAP) { CHECK(!depth); continue; }
        unsigned want=n<11?1:n<int64_t(S)?2:n<int64_t(T)?3:4; CHECK(depth==int(want));
        CHECK(boundary+1==span(n));
        uint64_t decoded=offsets[0];
        if(depth==2) decoded=D+offsets[1];
        if(depth==3) decoded=S+uint64_t(offsets[1])*P+offsets[2];
        if(depth==4) decoded=T+uint64_t(offsets[1])*P*P+uint64_t(offsets[2])*P+offsets[3];
        CHECK(decoded==uint64_t(n));
    }
    std::puts("PASS logical path transitions and final address");
}
static uint64_t ancestors(const std::map<uint64_t,uint32_t> &data) {
    std::set<std::array<uint64_t,3>> keys;
    for(auto [n,p]:data) {
        (void)p;
        if(n>=T) {uint64_t r=n-T;keys.insert({3,0,0});keys.insert({3,1,r/(P*P)});keys.insert({3,2,r/P});}
        else if(n>=S) {keys.insert({2,0,0});keys.insert({2,1,(n-S)/P});}
        else if(n>=D) keys.insert({1,0,0});
    }
    return keys.size();
}
static void sparse() {
    const std::vector<uint64_t> points={0,10,11,S-1,S,T-1,T,CAP-P,CAP-1};
    const std::vector<uint64_t> cuts={CAP*B,(CAP-1)*B,(CAP-P)*B+1,T*B+1,T*B,(T-1)*B,S*B+1,S*B,(S-1)*B,D*B+1,D*B,(D-1)*B,1,0};
    for(uint64_t cut:cuts) {
        std::printf("PROGRESS sparse cut=%llu\n",(unsigned long long)cut);std::fflush(stdout);
        Test t(0x7ffffff8ULL,0x80000100ULL);std::map<uint64_t,uint32_t> model;
        for(uint64_t n:points) { uint32_t p=t.map(n,true);CHECK(p && t.store.owned(p));model[n]=p; }
        for(auto [n,p]:model) CHECK(t.map(n,false)==p);
        indirect_test_truncate(t.fs,cut);
        uint64_t keep=(cut+B-1)/B;
        for(auto it=model.begin();it!=model.end();) { if(it->first>=keep) it=model.erase(it); else ++it; }
        for(auto n:points) CHECK(t.map(n,false)==(model.count(n)?model.at(n):0));
        CHECK(t.store.live==model.size()+ancestors(model));
        indirect_test_truncate(t.fs,0);t.empty();
        CHECK(t.map(CAP-1,true));indirect_test_truncate(t.fs,0);t.empty();
    }
    Test final(END-4,END);CHECK(final.map(CAP-1,true)==UINT32_MAX);
    CHECK(final.store.live==4);uint32_t n=0,p=99;
    CHECK(indirect_test_map(final.fs,static_cast<int32_t>(CAP),1,&n,&p,1)<0 && !p && !n);
    CHECK(final.store.live==4);indirect_test_truncate(final.fs,0);final.empty();
    std::puts("PASS sparse edge allocation, partial truncate, signed physical transition and UINT32_MAX reuse");
}
static void full(unsigned cycles) {
    Test t(END-TOTAL,END);
    for(unsigned cycle=0;cycle<cycles;cycle++) {
        auto started=std::chrono::steady_clock::now();
        for(uint64_t logical=0;logical<CAP;) {
            uint32_t count=0;uint32_t p=t.map(logical,true,P,&count);
            CHECK(count==span(logical) && uint64_t(p)==physical(t.store.begin,logical));logical+=count;
        }
        CHECK(t.store.live==TOTAL && t.store.pages.size()==META && t.store.free.empty());
        std::printf("PROGRESS full cycle=%u allocated_data=%llu metadata=%llu\n",cycle,(unsigned long long)CAP,(unsigned long long)META);std::fflush(stdout);
        for(uint64_t logical=0;logical<CAP;) {
            uint32_t count=0;uint32_t p=t.map(logical,false,P,&count);
            CHECK(count==span(logical) && uint64_t(p)==physical(t.store.begin,logical));logical+=count;
        }
        CHECK(t.map(CAP-1,false)==UINT32_MAX);
        uint32_t count=0,p=42;
        CHECK(indirect_test_map(t.fs,static_cast<int32_t>(CAP),1,&count,&p,1)<0 && !count && !p);
        CHECK(t.store.live==TOTAL);
        indirect_test_truncate(t.fs,0);t.empty();
        double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        std::printf("PASS full cycle=%u data=%llu metadata=%llu last_physical=0xffffffff outstanding=0 seconds=%.3f\n",cycle,(unsigned long long)CAP,(unsigned long long)META,seconds);std::fflush(stdout);
    }
}
int main(int argc,char **argv) {
    bool entire=argc>1 && !std::strcmp(argv[1],"--full");
    unsigned cycles=argc>2?std::stoul(argv[2]):1;CHECK(cycles>0);
    paths();sparse();if(entire) full(cycles);
    std::puts("PASS indirect boundary suite");return 0;
}
