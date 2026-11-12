/* Storage-only adapter for the real nvfuse_indirect.c, linked without the rest
 * of the filesystem. C++ supplies blocks/accounting; C supplies core ABI types. */
#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_indirect.h"
#include "indirect_adapter.h"
struct indirect_test {
    struct nvfuse_superblock sb;
    struct nvfuse_inode_ctx ictx;
    struct nvfuse_inode inode;
    struct io_target target;
    struct indirect_ops ops;
};
struct test_bh { struct nvfuse_buffer_head bh; uint32_t block; int dirty; };
static struct indirect_test *current;
struct indirect_test *indirect_test_open(struct indirect_ops ops) {
    assert(!current);
    struct indirect_test *t=calloc(1,sizeof(*t)); assert(t);
    t->ops=ops; t->inode.i_ino=8; t->ictx.ictx_inode=&t->inode;
    t->sb.sb_no_of_inodes_per_bg=4096; t->sb.sb_bg_num=1;
    /* An opaque nonnull marker enables production standalone error returns.
     * This adapter never invokes the journal engine. */
    t->target.journal=(struct nvj *)t;
    t->sb.target=&t->target; current=t; return t;
}
void indirect_test_close(struct indirect_test *t) { assert(t==current); current=NULL; free(t); }
int indirect_test_map(struct indirect_test *t,int32_t block,uint32_t max,uint32_t *n,uint32_t *p,int create) {
    return nvfuse_get_block(&t->sb,&t->ictx,block,max,n,p,create);
}
void indirect_test_truncate(struct indirect_test *t,uint64_t bytes) { nvfuse_truncate_blocks(&t->sb,&t->ictx,bytes); }
uint32_t indirect_test_root(struct indirect_test *t,unsigned n) { assert(n<14); return t->inode.i_blocks[n]; }
int indirect_test_path(int32_t b,uint32_t p[4],uint32_t *n) { return nvfuse_block_to_path(b,p,n); }
struct nvfuse_buffer_head *nvfuse_get_bh(struct nvfuse_superblock *sb,struct nvfuse_inode_ctx *ictx,
                                      inode_t ino,lbno_t block,s32 rw,s32 type) {
    (void)sb;(void)ictx;(void)ino;(void)type;
    struct test_bh *b=calloc(1,sizeof(*b)); assert(b);
    b->block=block; b->bh.bh_buf=current->ops.get(current->ops.context,block,rw==WRITE);
    assert(b->bh.bh_buf); return &b->bh;
}
void nvfuse_release_bh(struct nvfuse_superblock *sb,struct nvfuse_buffer_head *bh,s32 tail,s32 dirty) {
    (void)sb;(void)tail;
    struct test_bh *b=(void *)bh;
    current->ops.put(current->ops.context,b->block,bh->bh_buf,dirty||b->dirty); free(b);
}
void nvfuse_mark_dirty_bh(struct nvfuse_superblock *sb,struct nvfuse_buffer_head *bh) { (void)sb; ((struct test_bh *)bh)->dirty=1; }
void nvfuse_mark_inode_dirty(struct nvfuse_inode_ctx *ictx) { (void)ictx; }
u32 nvfuse_get_free_blocks(struct nvfuse_superblock *sb,u32 bg) {
    (void)sb;assert(!bg); uint64_t n=current->ops.available(current->ops.context); return n>UINT32_MAX?UINT32_MAX:(u32)n;
}
u32 nvfuse_alloc_dbitmap(struct nvfuse_superblock *sb,u32 bg,u32 *out,u32 count) {
    (void)sb;assert(!bg);return current->ops.allocate(current->ops.context,out,count);
}
void nvfuse_free_blocks(struct nvfuse_superblock *sb,u32 block,u32 count) { (void)sb;current->ops.release(current->ops.context,block,count); }
void nvfuse_print_bg_list(struct nvfuse_superblock *sb) { (void)sb; }
void *nvfuse_local_dma_zmalloc(size_t bytes,size_t align,uint64_t *physical) { (void)align;(void)physical;return calloc(1,bytes); }
