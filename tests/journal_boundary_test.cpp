/* Capacity-end coverage complements the sector/crash matrix in the C suite. */
extern "C" {
#include "nvfuse_journal.h"
}
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
using Block = std::array<unsigned char,NVJ_BLOCK>;
struct Disk {
    std::map<uint64_t,Block> pages;
    uint64_t fail_block=UINT64_MAX;
    bool armed=false;
    static int io(void *ctx,uint64_t offset,size_t n,void *buf,int write) {
        auto &d=*static_cast<Disk *>(ctx);
        CHECK(offset%NVJ_BLOCK==0 && n==NVJ_BLOCK);
        auto block=offset/NVJ_BLOCK;
        if (write) {
            std::memcpy(d.pages[block].data(),buf,n);
            /* Fully persistent unflushed writes are a permitted crash outcome. */
            if (d.armed && block==d.fail_block) { d.armed=false; return -EIO; }
        } else {
            auto it=d.pages.find(block);
            if (it==d.pages.end()) std::memset(buf,0,n);
            else std::memcpy(buf,it->second.data(),n);
        }
        return 0;
    }
    static int flush(void *) { return 0; }
};
static constexpr uint64_t bytes=uint64_t(NVJ_GROUP_BLOCKS)*NVJ_BLOCK;
static Block image(unsigned i) {
    Block b{};
    for (unsigned k=0;k<NVJ_BLOCK;k++) b[k]=static_cast<unsigned char>(1+(i*17+k)%251);
    return b;
}
static void stage(nvj *j,unsigned n) {
    for (unsigned i=0;i<n;i++) {
        auto b=image(i);
        CHECK(nvj_transfer(j,uint64_t(i)*NVJ_BLOCK,NVJ_BLOCK,b.data(),1)==0);
    }
}
static void verify(Disk &d,unsigned n,bool changed) {
    Block b{},zero{};
    for (unsigned i=0;i<n;i++) {
        CHECK(Disk::io(&d,uint64_t(i)*NVJ_BLOCK,NVJ_BLOCK,b.data(),0)==0);
        CHECK(b==(changed?image(i):zero));
    }
}
static void sparse_volume() {
    /* Largest format-compatible volume below the uint32 block ceiling. */
    const uint64_t volume=(uint64_t(UINT32_MAX)/NVJ_GROUP_BLOCKS)*NVJ_GROUP_BLOCKS*NVJ_BLOCK;
    const uint32_t homes[]={0,4100,UINT32_C(0x80001004),uint32_t(volume/NVJ_BLOCK-1)};
    Disk d; nvj_ops ops{Disk::io,Disk::flush,&d}; nvj *j=nullptr;
    CHECK(!nvj_format(&ops,volume,0)); CHECK(!nvj_open(&j,&ops,volume,0));
    CHECK(nvj_memory_bytes(j)<20u*1024*1024);
    for(auto home:homes) nvj_mark_metadata(j,home);
    CHECK(!nvj_begin(j,0,4));
    for(auto home:homes) { auto b=image(home); CHECK(!nvj_transfer(j,uint64_t(home)*NVJ_BLOCK,NVJ_BLOCK,b.data(),1)); }
    CHECK(d.pages.find(homes[3])==d.pages.end());
    CHECK(!nvj_commit(j)); CHECK(nvj_memory_bytes(j)<20u*1024*1024); nvj_close(j);
    CHECK(!nvj_open(&j,&ops,volume,0));
    for(auto home:homes) CHECK(d.pages.at(home)==image(home));
    nvj_mark_filesystem_metadata(j); CHECK(!nvj_begin(j,0,1));
    auto b=image(7); auto high=uint64_t(UINT32_C(0x80000000));
    CHECK(!nvj_transfer(j,high*NVJ_BLOCK,NVJ_BLOCK,b.data(),1));
    CHECK(d.pages.find(high)==d.pages.end()); CHECK(!nvj_commit(j)); CHECK(d.pages.at(high)==b);
    auto flushes=nvj_stats(j)->flushes;
    CHECK(!nvj_begin(j,0,0)); CHECK(!nvj_commit(j)); CHECK(nvj_stats(j)->flushes==flushes);
    /* Metadata-only data overwrite has no overlay, but must still flush. */
    CHECK(!nvj_begin(j,0,0)); CHECK(!nvj_transfer(j,4101ULL*NVJ_BLOCK,NVJ_BLOCK,b.data(),1));
    CHECK(!nvj_commit(j)); CHECK(nvj_stats(j)->flushes==flushes+1);
    nvj_close(j); std::puts("PASS maximum-volume sparse memory <20MiB, distant overlay, fixed classification, read-only/data-only barriers");
}
static void large_geometry() {
    for(unsigned blocks:{8192u,16384u}) {
        for(unsigned extra:{0u,1u,2u,3u}) {
            Disk d;nvj_ops ops{Disk::io,Disk::flush,&d};nvj *j=nullptr;
            CHECK(!nvj_format_sized(&ops,bytes,0,blocks));CHECK(!nvj_open(&j,&ops,bytes,0));
            unsigned capacity=nvj_capacity(j);CHECK(capacity==(blocks==8192?8155u:16315u));
            unsigned n=extra==1?capacity+1:capacity;
            CHECK(!nvj_begin(j,1,0));stage(j,n);
            if(extra>=2) { d.fail_block=extra==2?NVJ_GROUP_BLOCKS-3:capacity-1;d.armed=true; }
            CHECK(nvj_commit(j)==(extra==1?-E2BIG:extra>=2?-EIO:0));
            if(extra==1) nvj_discard(j);
            nvj_close(j);CHECK(!nvj_open(&j,&ops,bytes,0));
            verify(d,n,extra==0 || extra==3);
            if(extra==3) CHECK(nvj_stats(j)->replays==1);
            nvj_close(j);
            std::printf("PASS journal geometry=%u full-capacity=%u case=%u anchor-skip/reopen\n",blocks,capacity,extra);
        }
    }
}
int main() {
    sparse_volume(); large_geometry();
    for (unsigned n : {NVJ_CAPACITY-1,NVJ_CAPACITY,NVJ_CAPACITY+1}) {
        Disk d; nvj_ops ops{Disk::io,Disk::flush,&d}; nvj *j=nullptr;
        CHECK(nvj_format(&ops,bytes,0)==0); CHECK(nvj_open(&j,&ops,bytes,0)==0);
        CHECK(nvj_begin(j,1,0)==0); stage(j,n);
        verify(d,n,false);
        CHECK(nvj_error(j)==(n>NVJ_CAPACITY?-E2BIG:0));
        CHECK(nvj_commit(j)==(n>NVJ_CAPACITY?-E2BIG:0));
        if (n>NVJ_CAPACITY) nvj_discard(j);
        nvj_close(j); CHECK(nvj_open(&j,&ops,bytes,0)==0);
        verify(d,n,n<=NVJ_CAPACITY);
        CHECK(nvj_begin(j,1,1)==0);
        auto a=image(19),b=image(23);
        for (unsigned k=0;k<100;k++) CHECK(nvj_transfer(j,0,NVJ_BLOCK,(k==99?b:a).data(),1)==0);
        CHECK(nvj_commit(j)==0); CHECK(d.pages.at(0)==b); nvj_close(j);
        std::printf("PASS journal capacity=%u coalescing/reopen/reuse\n",n);
    }
    const uint64_t cuts[]={NVJ_START+3+NVJ_DESCRIPTORS-1,
        NVJ_START+NVJ_PAYLOAD+NVJ_CAPACITY-1,
        NVJ_START+NVJ_BLOCKS-1,NVJ_CAPACITY-1};
    for (unsigned c=0;c<4;c++) {
        Disk d; nvj_ops ops{Disk::io,Disk::flush,&d}; nvj *j=nullptr;
        CHECK(nvj_format(&ops,bytes,0)==0); CHECK(nvj_open(&j,&ops,bytes,0)==0);
        CHECK(nvj_begin(j,1,0)==0); stage(j,NVJ_CAPACITY);
        d.fail_block=cuts[c]; d.armed=true;
        CHECK(nvj_commit(j)==-EIO); CHECK(!d.armed); CHECK(nvj_recovery_required(j));
        nvj_close(j);
        for (unsigned repeat=0;repeat<2;repeat++) {
            CHECK(nvj_open(&j,&ops,bytes,0)==0); verify(d,NVJ_CAPACITY,c>=2); nvj_close(j);
        }
        CHECK(nvj_open(&j,&ops,bytes,0)==0); CHECK(nvj_begin(j,1,0)==0);
        stage(j,NVJ_CAPACITY); CHECK(nvj_commit(j)==0); nvj_close(j);
        CHECK(nvj_open(&j,&ops,bytes,0)==0); verify(d,NVJ_CAPACITY,true); nvj_close(j);
        std::printf("PASS journal final-slot crash=%u recovery/reuse\n",c);
    }
}
