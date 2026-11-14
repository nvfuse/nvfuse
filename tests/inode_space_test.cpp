/* SPDX-License-Identifier: GPL-2.0-only */

// Enumerate every inode bitmap slot of the maximum journal-format volume.
#include "inode_space_adapter.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
int main(int argc,char **argv) {
    bool full=argc==2 && !std::strcmp(argv[1],"--full");
    constexpr uint32_t groups=UINT32_MAX/32768u;
    uint64_t count=0;auto start=std::chrono::steady_clock::now();
    if(inode_space_edges()) return 1;
    if(full) count=inode_space_full(groups);
    else for(uint32_t g : {0u,1u,65535u,65536u,groups-1}) count+=inode_group_cycle(g,groups,g==0?9:0);
    std::printf("PASS inode space groups=%u exhaustive=%d highest_inode=%u allocated_and_freed=%llu cycles=2 seconds=%.3f\n",
        groups,full,groups*4096u-1,(unsigned long long)count,
        std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
}
