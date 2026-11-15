/* Exercise the real serialized mapper against authoritative cache pages.
 * Metadata-only fixtures; physical CRC/reopen tests cover the integrated path. */
#include "spdk/stdinc.h"
#include "nvfuse_core.h"
#include "nvfuse_dep.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_indirect.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); abort(); } } while(0)
static struct nvfuse_superblock sb;
static struct nvfuse_buffer_cache page[4];
static void cache_page(unsigned n,inode_t ino,u32 block,u32 physical)
{
    struct nvfuse_buffer_cache *bc=&page[n];
    bc->bc_buf=calloc(1,CLUSTER_SIZE);CHECK(bc->bc_buf);
    bc->bc_pno=physical;bc->bc_load=1;bc->bc_list_type=BUFFER_TYPE_CLEAN;
    nvfuse_make_pbno_key(ino,block,&bc->bc_bno,NVFUSE_BP_TYPE_DATA);
    rte_atomic32_set(&bc->bc_ref,0);SPINLOCK_INIT(&bc->bc_lock);
    list_add(&bc->bc_list,&sb.sb_bm->bm_list[BUFFER_TYPE_CLEAN]);
    hlist_add_head(&bc->bc_hash,&sb.sb_bm->bm_hash[bc->bc_bno%HASH_NUM]);
}
int main(void)
{
    sb.sb_bm=calloc(1,sizeof(*sb.sb_bm));CHECK(sb.sb_bm);SPINLOCK_INIT(&sb.sb_bm->bm_lock);
    for(unsigned i=0;i<BUFFER_TYPE_NUM;i++)INIT_LIST_HEAD(&sb.sb_bm->bm_list[i]);
    cache_page(0,ITABLE_INO,101/INODE_ENTRY_NUM,42);
    for(unsigned i=1;i<4;i++)cache_page(i,BLOCK_IO_INO,500+i,500+i);
    struct nvfuse_inode *inode=(struct nvfuse_inode*)page[0].bc_buf+101%INODE_ENTRY_NUM;
    inode->i_ino=101;inode->i_mode=S_IFREG|0600;inode->i_size=MAX_FILE_SIZE;
    const u32 blocks[]={2,DIRECT_BLOCKS+7,DIRECT_BLOCKS+PTRS_PER_BLOCK+77,
        DIRECT_BLOCKS+PTRS_PER_BLOCK+PTRS_PER_BLOCK*PTRS_PER_BLOCK+123};
    unsigned paths=0;
    for(unsigned d=0;d<4;d++) {
        u32 path[4],boundary,physical=0;int depth=nvfuse_block_to_path(blocks[d],path,&boundary);
        CHECK(depth==(int)d+1&&boundary>=3);memset(inode->i_blocks,0,sizeof(inode->i_blocks));
        u32 *p=inode->i_blocks;
        for(int i=0;i<depth-1;i++){p[path[i]]=501+i;memset(page[i+1].bc_buf,0,CLUSTER_SIZE);p=(u32*)page[i+1].bc_buf;}
        for(unsigned i=0;i<4;i++)p[path[depth-1]+i]=700+i;
        CHECK(nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,4*CLUSTER_SIZE,&physical)==1&&physical==700);
        struct nvfuse_inode_ctx ictx={.ictx_inode=inode};SPINLOCK_INIT(&ictx.ictx_lock);SPINLOCK_LOCK(&ictx.ictx_lock);set_bit(&ictx.ictx_status,INODE_STATE_LOCK);
        u32 count,ordinary;CHECK(!nvfuse_get_block_cached(&sb,&ictx,blocks[d],4,&count,&ordinary));CHECK(count==4&&ordinary==physical);SPINLOCK_UNLOCK(&ictx.ictx_lock);
        p[path[depth-1]+1]=799;CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,2*CLUSTER_SIZE,&physical));p[path[depth-1]+1]=701;
        for(unsigned n=0;n<(unsigned)depth;n++) {
            page[n].bc_load=0;CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,CLUSTER_SIZE,&physical));page[n].bc_load=1;
            page[n].bc_flush=1;CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,CLUSTER_SIZE,&physical));page[n].bc_flush=0;
            page[n].bc_locked=1;CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,CLUSTER_SIZE,&physical));page[n].bc_locked=0;
            rte_atomic32_set(&page[n].bc_ref,1);CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,CLUSTER_SIZE,&physical));rte_atomic32_set(&page[n].bc_ref,0);
            if(n){page[n].bc_pno++;CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,CLUSTER_SIZE,&physical));page[n].bc_pno--;}
        }
        inode->i_size=(s64)blocks[d]*CLUSTER_SIZE;CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,CLUSTER_SIZE,&physical));inode->i_size=MAX_FILE_SIZE;
        inode->i_ino=102;CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,CLUSTER_SIZE,&physical));inode->i_ino=101;
        p[path[depth-1]]=0;CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,CLUSTER_SIZE,&physical));p[path[depth-1]]=800;
        CHECK(nvfuse_try_serialized_read_mapping(&sb,101,(s64)blocks[d]*CLUSTER_SIZE,CLUSTER_SIZE,&physical)==1&&physical==800);paths++;
    }
    u32 physical=0;CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,-1,CLUSTER_SIZE,&physical));CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,0,0,&physical));CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,0,1,&physical));CHECK(!nvfuse_try_serialized_read_mapping(&sb,101,MAX_FILE_SIZE,CLUSTER_SIZE,&physical));
    for(unsigned i=0;i<4;i++)free(page[i].bc_buf);free(sb.sb_bm);
    printf("PASS serialized mapper: %u levels, locked-mapper equivalence, mutation, cold/pinned/flushing/identity/EOF/hole/fragment fallbacks\n",paths);return 0;
}
