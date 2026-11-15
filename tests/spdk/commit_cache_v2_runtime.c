#define NVFIO_TEST
#include "../../src/nvfuse_spdk_runtime.c"
#include "nvfuse_inode_cache.h"
int nvfio_test_cc_tryappend(int fd,const void*data,unsigned count,uint64_t offset)
{int result=0;return commit_cache_write(fd,data,count,offset,&result)?result:0;}
void nvfio_test_cc_dump(int fd)
{
 if(!commit_cache_enter()){fprintf(stderr,"DUMP no admission\n");return;}
 struct nvfuse_superblock*sb=&rt.handle->nvh_sb;struct nvfuse_file_table*of=nvfuse_get_file_table(sb,fd);
 struct nvfuse_inode_ctx*c=nvfuse_ictx_hash_lookup(sb->sb_ictxc,of->ino);
 fprintf(stderr,"DUMP fd=%d ino=%u flags=%u size=%llu ctx=%p ref=%d bh=%p lock=%d\n",fd,of->ino,of->flags,(unsigned long long)of->size,(void*)c,c?c->ictx_ref:-1,c?(void*)c->ictx_bh:NULL,c?rte_spinlock_is_locked(&c->ictx_lock):-1);
 struct nvfuse_buffer_manager*bm=sb->sb_bm;u64 key;nvfuse_make_pbno_key(ITABLE_INO,of->ino/INODE_ENTRY_NUM,&key,NVFUSE_BP_TYPE_DATA);
 struct nvfuse_buffer_cache*bc=nvfuse_hash_lookup(bm,key);
 if(bc){struct nvfuse_inode*i=(struct nvfuse_inode*)bc->bc_buf+of->ino%INODE_ENTRY_NUM;
 fprintf(stderr,"DUMP inode ino=%u size=%llu ref=%d bh=%d load=%u flush=%u locked=%u type=%u pno=%u meta=%d ptr2=%u\n",i->i_ino,(unsigned long long)i->i_size,rte_atomic32_read(&bc->bc_ref),rte_atomic32_read(&bc->bc_bh_count),bc->bc_load,bc->bc_flush,bc->bc_locked,bc->bc_list_type,bc->bc_pno,nvj_metadata_known_frozen(sb->target->journal,bc->bc_pno),i->i_blocks[2]);}
 nvfuse_make_pbno_key(of->ino,2,&key,NVFUSE_BP_TYPE_DATA);bc=nvfuse_hash_lookup(bm,key);
 fprintf(stderr,"DUMP data2=%p pno=%u ref=%d type=%u dirty=%u load=%u\n",(void*)bc,bc?bc->bc_pno:0,bc?rte_atomic32_read(&bc->bc_ref):-1,bc?bc->bc_list_type:99,bc?bc->bc_dirty:99,bc?bc->bc_load:99);
 nvcc_leave(&commit_cache.state);
}
