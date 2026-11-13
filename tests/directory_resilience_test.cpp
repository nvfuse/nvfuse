#include "fs_model_adapter.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL line=%d: %s\n",__LINE__,#x);std::exit(1);} }while(0)
static void exists(void *h,const std::string &p) {uint64_t n;unsigned l;int rc=model_stat(h,p.c_str(),&n,&l);if(rc)std::fprintf(stderr,"lookup path=%s rc=%d\n",p.c_str(),rc);CHECK(rc==0);}
static std::vector<std::string> collisions() {
    std::unordered_map<uint32_t,std::string> seen;uint32_t rng=0xabcdef12;std::string a,b;
    for(unsigned k=0;k<1000000 && a.empty();k++) {
        std::string s(12,' ');for(auto &c:s){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;c="abcdefghijklmnopqrstuvwxyz0123456789"[rng%36];}
        uint32_t hash[2];model_hash((s+"abcdefghijkl").c_str(),hash);
        auto [it,inserted]=seen.emplace(hash[0],s);if(!inserted && it->second!=s){a=it->second;b=s;}
    }
    CHECK(!a.empty());std::vector<std::string> result;
    std::array<uint32_t,2> expected{};
    for(unsigned mask=0;mask<256;mask++) {
        std::string s;for(unsigned bit=0;bit<8;bit++)s+=(mask&(1u<<bit))?a:b;
        std::array<uint32_t,2> hash{};model_hash(s.c_str(),hash.data());if(!mask)expected=hash;else CHECK(hash==expected);result.push_back(s);
    }
    std::printf("COLLISION hash=%08x%08x names=%zu\n",expected[1],expected[0],result.size());std::fflush(stdout);return result;
}
int main(int argc,char **argv) {
    CHECK(argc==3);std::string mode=argv[2];void *h=model_open(argv[1],1,1);CHECK(h);
    uint64_t base_b,base_i,b,i;CHECK(!model_counts(h,&base_b,&base_i));CHECK(!model_mkdir(h,"/arena"));
    if(mode=="collision") {
        auto names=collisions();std::vector<bool> live(names.size(),true);
        for(const auto &n:names){CHECK(!model_create(h,("/arena/"+n).c_str()));}
        /* Rename to an absent name with the SAME saturated hash, persist,
         * then rename back. The index count and every survivor must agree. */
        CHECK(!model_unlink(h,("/arena/"+names.back()).c_str()));
        CHECK(!model_rename(h,("/arena/"+names[0]).c_str(),("/arena/"+names.back()).c_str()));
        CHECK(!model_close(h));h=model_open(argv[1],0,1);CHECK(h);
        for(unsigned k=1;k<names.size();k++)exists(h,"/arena/"+names[k]);
        {uint64_t n;unsigned l;CHECK(model_stat(h,("/arena/"+names[0]).c_str(),&n,&l)<0);}
        CHECK(!model_rename(h,("/arena/"+names.back()).c_str(),("/arena/"+names[0]).c_str()));
        CHECK(!model_create(h,("/arena/"+names.back()).c_str()));
        CHECK(!model_rename(h,("/arena/"+names[0]).c_str(),"/arena/temporary"));
        CHECK(!model_rename(h,"/arena/temporary",("/arena/"+names[0]).c_str()));
        CHECK(!model_rename(h,("/arena/"+names[0]).c_str(),("/arena/"+names[1]).c_str()));
        CHECK(!model_create(h,("/arena/"+names[0]).c_str()));
        for(unsigned round=0;round<3;round++) {
            for(unsigned k=0;k<names.size();k++) {
                unsigned victim=(k*7)%names.size();CHECK(!model_unlink(h,("/arena/"+names[victim]).c_str()));live[victim]=false;
                for(unsigned j=0;j<names.size();j++) {uint64_t n;unsigned l;int rc=model_stat(h,("/arena/"+names[j]).c_str(),&n,&l);CHECK(live[j]?rc==0:rc<0);}
                CHECK(!model_close(h));h=model_open(argv[1],0,1);CHECK(h);
            }
            if(round<2)for(unsigned k=0;k<names.size();k++){CHECK(!model_create(h,("/arena/"+names[k]).c_str()));live[k]=true;}
        }
        for(const auto &n:names)CHECK(!model_mkdir(h,("/arena/"+n).c_str()));
        for(unsigned k=0;k<names.size();k++) {
            CHECK(!model_rmdir(h,("/arena/"+names[(k*7)%names.size()]).c_str()));
            for(unsigned j=k+1;j<names.size();j++)exists(h,"/arena/"+names[(j*7)%names.size()]);
        }
    } else if(mode=="names") {
        std::string max="/arena/"+std::string(115,'x'),over="/arena/"+std::string(116,'x');
        CHECK(!model_create(h,max.c_str()));exists(h,max);CHECK(model_create(h,over.c_str())<0);exists(h,max);
        CHECK(model_rename(h,max.c_str(),over.c_str())<0);exists(h,max);CHECK(!model_unlink(h,max.c_str()));
        CHECK(model_create(h,("/arena/"+std::string(4097,'z')).c_str())<0);
        CHECK(model_create(h,("/"+std::string(116,'z')+"/leaf").c_str())<0);
        CHECK(!model_create(h,"arena/./relative"));exists(h,"/arena/./relative");
        CHECK(!model_rename(h,"arena/relative","/arena/renamed"));exists(h,"/arena//renamed");
        CHECK(!model_unlink(h,"arena/renamed"));
    } else if(mode=="parent") {
        CHECK(!model_parent(h,"/","/"));CHECK(!model_parent(h,"/arena","/"));
        CHECK(!model_rename(h,"/arena","/arena"));CHECK(model_rename(h,"/missing","/missing")<0);
        CHECK(!model_mkdir(h,"/arena/empty"));CHECK(!model_parent(h,"/arena/empty","/arena"));
        CHECK(!model_create(h,"/arena/child"));exists(h,"/arena/../arena/child");
        CHECK(!model_mkdir(h,"/arena/nested"));CHECK(!model_create(h,"/arena/nested/file"));
        CHECK(!model_mkdir(h,"/other"));
        CHECK(!model_rename(h,"/arena/empty","/other/empty"));CHECK(!model_parent(h,"/other/empty","/other"));
        CHECK(!model_rename(h,"/arena/nested","/other/nested"));CHECK(!model_parent(h,"/other/nested","/other"));
        exists(h,"/other/nested/../nested/file");
        CHECK(model_rename(h,"/other","/other/nested/cycle")<0);CHECK(!model_parent(h,"/other/nested","/other"));
        CHECK(!model_legacy_parent(h,"/other/empty"));CHECK(!model_legacy_parent(h,"/other/nested"));
        CHECK(!model_close(h));h=model_open(argv[1],0,1);CHECK(h);
        CHECK(!model_parent(h,"/other/empty","/other"));CHECK(!model_parent(h,"/other/nested","/other"));
        CHECK(!model_close(h));h=model_open(argv[1],0,1);CHECK(h);
        CHECK(!model_parent(h,"/other/nested","/other"));
        CHECK(!model_legacy_dot(h,"/other/nested"));
        CHECK(!model_close(h));h=model_open(argv[1],0,1);CHECK(h);
        CHECK(!model_parent(h,"/other/nested","/other"));
        CHECK(model_rmdir(h,"/other/nested/.")<0);CHECK(model_rmdir(h,"/other/nested/..")<0);
        CHECK(!model_unlink(h,"/other/nested/file"));CHECK(!model_rmdir(h,"/other/nested"));
        CHECK(!model_rmdir(h,"/other/empty"));CHECK(!model_rmdir(h,"/other"));
        CHECK(!model_unlink(h,"/arena/child"));
    } else if(mode=="depth") {
        std::vector<std::string> dirs;std::string path="/arena";
        for(unsigned k=0;k<128;k++){path+="/d";CHECK(!model_mkdir(h,path.c_str()));dirs.push_back(path);exists(h,path);}
        CHECK(!model_create(h,(path+"/leaf").c_str()));CHECK(!model_close(h));h=model_open(argv[1],0,1);CHECK(h);exists(h,path+"/leaf");
        CHECK(!model_unlink(h,(path+"/leaf").c_str()));for(auto it=dirs.rbegin();it!=dirs.rend();it++)CHECK(!model_rmdir(h,it->c_str()));
    } else if(mode=="splits") {
        for(unsigned round=0;round<5;round++) {
            std::vector<std::string> names;for(unsigned k=0;k<2048;k++){names.push_back("/arena/item"+std::to_string(k));CHECK(!model_create(h,names.back().c_str()));}
            for(unsigned k=0;k<names.size();k++)exists(h,names[k]);
            for(unsigned k=0;k<names.size();k++){unsigned victim=(k*1031)%names.size();CHECK(!model_unlink(h,names[victim].c_str()));if(k%128==0)for(unsigned j=k+1;j<names.size();j++)exists(h,names[(j*1031)%names.size()]);}
            CHECK(!model_close(h));h=model_open(argv[1],0,1);CHECK(h);
        }
    } else CHECK(!"unknown mode");
    CHECK(!model_rmdir(h,"/arena"));CHECK(!model_counts(h,&b,&i) && b==base_b && i==base_i);CHECK(!model_close(h));
    std::printf("PASS directory resilience %s\n",mode.c_str());
}
