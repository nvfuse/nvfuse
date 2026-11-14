/* SPDX-License-Identifier: GPL-2.0-only */

/* Only storage callbacks are replaced. Link dead-strips unused core routines. */
#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_dep.h"
#include "inode_space_adapter.h"
static struct nvfuse_superblock sb;
static struct nvfuse_handle handle;
static struct nvfuse_bg_descriptor bd;
static unsigned char bitmap[CLUSTER_SIZE];
static struct nvfuse_buffer_head bd_head,bitmap_head;
static struct nvfuse_bg_descriptor *all_descriptors;
static unsigned char *all_bitmaps;
struct nvfuse_buffer_head *nvfuse_get_bh(struct nvfuse_superblock *s,struct nvfuse_inode_ctx *ctx,
                                      inode_t ino,lbno_t group,s32 rw,s32 type) {
    (void)ctx;(void)rw;(void)type;
    assert(s==&sb && group<(u32)sb.sb_bg_num);
    struct nvfuse_bg_descriptor *descriptor=all_descriptors?all_descriptors+group:&bd;
    unsigned char *bits=all_bitmaps?all_bitmaps+(size_t)group*CLUSTER_SIZE:bitmap;
    assert(group==descriptor->bd_id);
    if(ino==BD_INO) { bd_head.bh_buf=(void *)descriptor;return &bd_head; }
    assert(ino==IBITMAP_INO);bitmap_head.bh_buf=(void *)bits;return &bitmap_head;
}
void nvfuse_release_bh(struct nvfuse_superblock *s,struct nvfuse_buffer_head *h,s32 tail,s32 dirty) {
    (void)s;(void)h;(void)tail;(void)dirty;
}
static void initialize(uint32_t group,uint32_t groups,unsigned reserved) {
    memset(&sb,0,sizeof(sb));memset(&bd,0,sizeof(bd));memset(bitmap,0,sizeof(bitmap));
    sb.sb_nvh=&handle;handle.nvh_params.preallocation=1;
    sb.sb_no_of_inodes_per_bg=NVFUSE_INODE_PER_BG;sb.sb_bg_num=groups;
    bd.bd_id=group;bd.bd_max_inodes=NVFUSE_INODE_PER_BG;
    bd.bd_free_inodes=NVFUSE_INODE_PER_BG-reserved;sb.sb_free_inodes=bd.bd_free_inodes;
    for(unsigned k=0;k<reserved;k++) ext2fs_set_bit(k,bitmap);
}
uint64_t inode_group_cycle(uint32_t group,uint32_t groups,unsigned reserved) {
    initialize(group,groups,reserved);
    unsigned available=NVFUSE_INODE_PER_BG-reserved;
    for(unsigned round=0;round<2;round++) {
        for(unsigned k=reserved;k<NVFUSE_INODE_PER_BG;k++) {
            uint32_t expected=group*NVFUSE_INODE_PER_BG+k;
            assert(nvfuse_scan_free_ibitmap(&sb,NULL,group,k)==expected);
            nvfuse_dec_free_inodes(&sb,expected);
            assert(bd.bd_free_inodes==NVFUSE_INODE_PER_BG-k-1 && sb.sb_free_inodes==bd.bd_free_inodes);
        }
        assert(nvfuse_scan_free_ibitmap(&sb,NULL,group,0)==0);
        for(unsigned k=0;k<NVFUSE_INODE_PER_BG;k++) assert(ext2fs_test_bit(k,bitmap));
        for(unsigned k=NVFUSE_INODE_PER_BG;k-->reserved;) {
            uint32_t ino=group*NVFUSE_INODE_PER_BG+k;
            nvfuse_release_ibitmap(&sb,group,ino);nvfuse_inc_free_inodes(&sb,ino);
            assert(!ext2fs_test_bit(k,bitmap));
        }
        assert(bd.bd_free_inodes==available && sb.sb_free_inodes==available);
        for(unsigned k=0;k<NVFUSE_INODE_PER_BG;k++) assert(!!ext2fs_test_bit(k,bitmap)==(k<reserved));
    }
    return (uint64_t)available*2;
}
int inode_space_edges(void) {
    /* Probe a bitmap hint at the final slot wrapping to slot zero. */
    initialize(131070,131071,0);
    for(unsigned k=1;k<NVFUSE_INODE_PER_BG;k++) ext2fs_set_bit(k,bitmap);
    bd.bd_free_inodes=sb.sb_free_inodes=1;
    assert(nvfuse_scan_free_ibitmap(&sb,NULL,bd.bd_id,NVFUSE_INODE_PER_BG-1)==131070u*NVFUSE_INODE_PER_BG);
    initialize(0,2,NUM_RESV_INO);
    all_descriptors=calloc(2,sizeof(*all_descriptors));all_bitmaps=malloc(2*CLUSTER_SIZE);
    assert(all_descriptors && all_bitmaps);memset(all_bitmaps,0xff,2*CLUSTER_SIZE);
    for(unsigned g=0;g<2;g++) {all_descriptors[g].bd_id=g;all_descriptors[g].bd_max_inodes=NVFUSE_INODE_PER_BG;}
    all_descriptors[1].bd_free_inodes=sb.sb_free_inodes=1;
    ext2fs_clear_bit(NVFUSE_INODE_PER_BG-1,all_bitmaps+CLUSTER_SIZE);
    assert(nvfuse_find_free_inode(&sb,NULL,2*NVFUSE_INODE_PER_BG)==2*NVFUSE_INODE_PER_BG-1);
    free(all_descriptors);free(all_bitmaps);all_descriptors=NULL;all_bitmaps=NULL;
    return 0;
}

uint64_t inode_space_full(uint32_t groups) {
    initialize(0,groups,NUM_RESV_INO);
    uint32_t capacity=groups*NVFUSE_INODE_PER_BG-NUM_RESV_INO;
    sb.sb_free_inodes=capacity;
    all_descriptors=calloc(groups,sizeof(*all_descriptors));
    all_bitmaps=calloc(groups,CLUSTER_SIZE);
    assert(all_descriptors && all_bitmaps);
    for(uint32_t g=0;g<groups;g++) {
        all_descriptors[g].bd_id=g;all_descriptors[g].bd_max_inodes=NVFUSE_INODE_PER_BG;
        all_descriptors[g].bd_free_inodes=NVFUSE_INODE_PER_BG-(g==0?NUM_RESV_INO:0);
    }
    for(unsigned k=0;k<NUM_RESV_INO;k++) ext2fs_set_bit(k,all_bitmaps);
    for(unsigned round=0;round<2;round++) {
        uint32_t remaining=capacity;
        for(uint32_t g=0;g<groups;g++) {
            for(unsigned k=g==0?NUM_RESV_INO:0;k<NVFUSE_INODE_PER_BG;k++) {
                uint32_t ino=g*NVFUSE_INODE_PER_BG+k;
                assert(nvfuse_scan_free_ibitmap(&sb,NULL,g,k)==ino);
                nvfuse_dec_free_inodes(&sb,ino);
                assert(sb.sb_free_inodes==--remaining);
            }
            assert(!all_descriptors[g].bd_free_inodes);
        }
        assert(!sb.sb_free_inodes);
        /* A successful allocation of the final inode leaves the next hint one
         * past the volume. Releasing/reallocating it must wrap group selection. */
        uint32_t final_inode=groups*NVFUSE_INODE_PER_BG-1;
        nvfuse_release_ibitmap(&sb,groups-1,final_inode);nvfuse_inc_free_inodes(&sb,final_inode);
        assert(nvfuse_find_free_inode(&sb,NULL,final_inode+1)==final_inode);
        nvfuse_dec_free_inodes(&sb,final_inode);assert(!sb.sb_free_inodes);
        /* All groups remain allocated simultaneously until this full sweep
         * verifies exhaustion and every live bitmap bit. */
        for(uint32_t g=0;g<groups;g++) {
            assert(nvfuse_scan_free_ibitmap(&sb,NULL,g,0)==0);
            const unsigned char *bits=all_bitmaps+(size_t)g*CLUSTER_SIZE;
            for(unsigned k=0;k<NVFUSE_INODE_PER_BG/8;k++) assert(bits[k]==0xff);
            for(unsigned k=NVFUSE_INODE_PER_BG/8;k<CLUSTER_SIZE;k++) assert(!bits[k]);
        }
        printf("PROGRESS inode full cycle=%u simultaneously_allocated=%u free=0\n",round,capacity);fflush(stdout);
        for(uint32_t g=groups;g-->0;) {
            unsigned reserved=g==0?NUM_RESV_INO:0;
            for(unsigned k=NVFUSE_INODE_PER_BG;k-->reserved;) {
                uint32_t ino=g*NVFUSE_INODE_PER_BG+k;
                nvfuse_release_ibitmap(&sb,g,ino);nvfuse_inc_free_inodes(&sb,ino);
                assert(sb.sb_free_inodes==++remaining);
            }
            assert(all_descriptors[g].bd_free_inodes==NVFUSE_INODE_PER_BG-reserved);
        }
        assert(sb.sb_free_inodes==capacity);
        for(uint32_t g=0;g<groups;g++) {
            const unsigned char *bits=all_bitmaps+(size_t)g*CLUSTER_SIZE;
            for(unsigned k=0;k<CLUSTER_SIZE;k++) {
                unsigned char expected=g==0 && k<NUM_RESV_INO/8?0xff:
                    g==0 && k==NUM_RESV_INO/8?(unsigned char)((1u<<(NUM_RESV_INO%8))-1):0;
                assert(bits[k]==expected);
            }
        }
    }
    free(all_bitmaps);free(all_descriptors);all_bitmaps=NULL;all_descriptors=NULL;
    return (uint64_t)capacity*2;
}
