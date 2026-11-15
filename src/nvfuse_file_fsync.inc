/* SPDX-License-Identifier: GPL-2.0-only */

/* Conservative file-scoped v3 commit. The owner holds fs_gate and has drained
 * all AIO/writeback. Unrelated metadata remains in the ordinary dirty cache;
 * an already populated journal overlay always falls back to a full commit.
 *
 * For an existing regular file with unchanged lifetime/namespace attributes,
 * collect its inode and changed indirect blocks. Start allocation bitmaps,
 * descriptors and the superblock from their COMMITTED images, adding only the
 * target's new mappings. Never publish another file's pending allocation or
 * size. Free/reuse and inode creation require an intervening full commit.
 * Images are private copies: shared cached metadata and accounting stay live.
 * No on-disk format, journal ordering or successful-fsync barrier changes.
 */
#define FILE_SYNC_IMAGES 64u
#define FILE_SYNC_GROUPS 16u
#define FILE_SYNC_NODES 256u
#define FILE_SYNC_ALLOCS 2048u

struct file_sync_group {
    u32 group;
    unsigned bitmap, descriptor;
};
struct file_sync_plan {
    struct nvfuse_superblock *sb;
    unsigned count, groups, nodes, allocations;
    u32 home[FILE_SYNC_IMAGES];
    struct file_sync_group group[FILE_SYNC_GROUPS];
    unsigned char image[FILE_SYNC_IMAGES][CLUSTER_SIZE];
};

static int file_sync_read(struct file_sync_plan *p, u32 block, void *image)
{
    if ((u64)block >= (u64)p->sb->sb_no_of_blocks ||
        (block >= NVJ_START && block < NVJ_GROUP_BLOCKS)) return -EAGAIN;
    return nvj_transfer(p->sb->target->journal, (u64)block * CLUSTER_SIZE,
                         CLUSTER_SIZE, image, 0);
}

static int file_sync_image(struct file_sync_plan *p, u32 home, const void *image)
{
    for (unsigned i = 0; i < p->count; i++)
        if (p->home[i] == home) return -EAGAIN; /* aliases/cycles are unsupported */
    if (p->count == FILE_SYNC_IMAGES) return -EAGAIN;
    p->home[p->count] = home;
    memcpy(p->image[p->count++], image, CLUSTER_SIZE);
    return 0;
}

static int file_sync_allocate(struct file_sync_plan *p, u32 block)
{
    struct nvfuse_superblock *sb = p->sb;
    if (!block || (u64)block >= (u64)sb->sb_no_of_blocks ||
        p->allocations == FILE_SYNC_ALLOCS) return -EAGAIN;
    u32 group = block / sb->sb_no_of_blocks_per_bg;
    u32 within = block % sb->sb_no_of_blocks_per_bg;
    struct file_sync_group *g = NULL;
    for (unsigned i = 0; i < p->groups; i++)
        if (p->group[i].group == group) g = &p->group[i];
    if (!g) {
        unsigned char image[CLUSTER_SIZE];
        if (p->groups == FILE_SYNC_GROUPS || p->count + 2 > FILE_SYNC_IMAGES)
            return -EAGAIN;
        u32 descriptor = group * sb->sb_no_of_blocks_per_bg + NVFUSE_BD_OFFSET;
        int rc = file_sync_read(p, descriptor, image);
        if (rc) return rc;
        struct nvfuse_bg_descriptor *bd = (void *)image;
        if (bd->bd_magic != NVFUSE_BD_MAGIC || bd->bd_id != group ||
            bd->bd_bg_start != group * sb->sb_no_of_blocks_per_bg ||
            bd->bd_dbitmap_size != 1 ||
            bd->bd_dbitmap_start / sb->sb_no_of_blocks_per_bg != group)
            return -EAGAIN;
        g = &p->group[p->groups++];
        g->group = group; g->descriptor = p->count;
        u32 bitmap = bd->bd_dbitmap_start;
        rc = file_sync_image(p, descriptor, image);
        if (rc) return rc;
        rc = file_sync_read(p, bitmap, image);
        if (rc) return rc;
        g->bitmap = p->count;
        rc = file_sync_image(p, bitmap, image);
        if (rc) return rc;
    }
    struct nvfuse_bg_descriptor *bd = (void *)p->image[g->descriptor];
    unsigned char *bitmap = p->image[g->bitmap];
    /* A new pointer to an already durable allocation can be free/reuse ABA,
     * corruption or an alias. Resolve it with the existing full commit. */
    if (block < bd->bd_dtable_start || !bd->bd_free_blocks ||
        (bitmap[within / 8] & (1u << (within % 8)))) return -EAGAIN;
    bitmap[within / 8] |= 1u << (within % 8);
    bd->bd_free_blocks--;
    p->allocations++;
    return 0;
}

static int file_sync_current_node(struct file_sync_plan *p, u32 block,
                                  int existed, void *image)
{
    struct nvfuse_buffer_manager *bm = p->sb->sb_bm;
    u64 key;
    int loaded = 0;
    nvfuse_make_pbno_key(BLOCK_IO_INO, block, &key, NVFUSE_BP_TYPE_DATA);
    SPINLOCK_LOCK(&bm->bm_lock);
    struct nvfuse_buffer_cache *bc = nvfuse_hash_lookup(bm, key);
    if (bc) {
        SPINLOCK_LOCK(&bc->bc_lock);
        if (bc->bc_load && bc->bc_pno == block && !bc->bc_flush) {
            memcpy(image, bc->bc_buf, CLUSTER_SIZE);
            loaded = 1;
        }
        SPINLOCK_UNLOCK(&bc->bc_lock);
    }
    SPINLOCK_UNLOCK(&bm->bm_lock);
    if (loaded) return 0;
    /* With an empty active overlay, an evicted OLD node can only equal the
     * committed image. A missing NEW node has no such proof. */
    return existed ? file_sync_read(p, block, image) : -EAGAIN;
}

static int file_sync_node(struct file_sync_plan *p, u32 old, u32 now, unsigned level)
{
    if (!now) return old ? -EAGAIN : 0;
    if (old && old != now) return -EAGAIN;
    if (++p->nodes > FILE_SYNC_NODES) return -EAGAIN;
    if (!old) {
        int rc = file_sync_allocate(p, now);
        if (rc) return rc;
    }
    u32 current[PTRS_PER_BLOCK], previous[PTRS_PER_BLOCK];
    int rc = file_sync_current_node(p, now, !!old, current);
    if (rc) return rc;
    if (old) rc = file_sync_read(p, old, previous);
    else memset(previous, 0, sizeof(previous));
    if (rc) return rc;
    if (!old || memcmp(current, previous, CLUSTER_SIZE)) {
        rc = file_sync_image(p, now, current);
        if (rc) return rc;
    }
    for (unsigned i = 0; i < PTRS_PER_BLOCK; i++) {
        if (previous[i] && previous[i] != current[i]) return -EAGAIN;
        if (level > 1) rc = file_sync_node(p, previous[i], current[i], level - 1);
        else if (!previous[i] && current[i]) rc = file_sync_allocate(p, current[i]);
        if (rc) return rc;
    }
    return 0;
}

static int file_sync_collect(struct nvfuse_handle *h, int fd, struct file_sync_plan *p)
{
    struct nvfuse_superblock *sb = &h->nvh_sb;
    struct nvfuse_inode_ctx *ctx = nvfuse_read_inode(sb, NULL, sb->sb_file_table[fd].ino);
    if (!ctx) return -EAGAIN;
    u32 physical = ctx->ictx_bh->bh_bc->bc_pno;
    struct nvfuse_inode current = *ctx->ictx_inode;
    nvfuse_release_inode(sb, ctx, NVF_CLEAN);
    struct nvfuse_inode previous, allowed;
    int rc = file_sync_read(p, physical, &previous);
    if (rc) return rc;
    if (current.i_ino < NUM_RESV_INO || current.i_type != NVFUSE_TYPE_FILE ||
        !S_ISREG(current.i_mode) || previous.i_ino != current.i_ino ||
        previous.i_size < 0 || current.i_size < previous.i_size ||
        current.i_deleted || current.i_version != previous.i_version)
        return -EAGAIN;
    allowed = previous;
    allowed.i_size = current.i_size;
    allowed.i_atime = current.i_atime;
    allowed.i_mtime = current.i_mtime;
    allowed.i_ctime = current.i_ctime;
    memcpy(allowed.i_blocks, current.i_blocks, sizeof(allowed.i_blocks));
    if (memcmp(&allowed, &current, sizeof(current))) return -EAGAIN;
    rc = file_sync_image(p, physical, &current);
    if (rc) return rc;
    for (unsigned i = 0; i < DIRECT_BLOCKS; i++) {
        if (previous.i_blocks[i] && previous.i_blocks[i] != current.i_blocks[i])
            return -EAGAIN;
        if (!previous.i_blocks[i] && current.i_blocks[i])
            rc = file_sync_allocate(p, current.i_blocks[i]);
        if (rc) return rc;
    }
    for (unsigned i = INDIRECT_BLOCKS; i <= TINDIRECT_BLOCKS; i++) {
        rc = file_sync_node(p, previous.i_blocks[i], current.i_blocks[i],
                             i - INDIRECT_BLOCKS + 1);
        if (rc) return rc;
    }
    if (p->allocations) {
        unsigned char image[CLUSTER_SIZE];
        rc = file_sync_read(p, 0, image);
        if (rc) return rc;
        struct nvfuse_superblock_common *super = (void *)image;
        if (super->sb_signature != NVJ_SIGNATURE ||
            super->sb_free_blocks < (s64)p->allocations ||
            super->sb_no_of_used_blocks > super->sb_no_of_blocks - p->allocations)
            return -EAGAIN;
        super->sb_free_blocks -= p->allocations;
        super->sb_no_of_used_blocks += p->allocations;
        rc = file_sync_image(p, 0, image);
    }
    return rc;
}

/* Return 1 when handled, 0 for conservative full-commit fallback, or errno. */
static int file_sync_try(struct nvfuse_handle *h, int fd)
{
    struct io_target *t = h->nvh_target;
    struct nvfuse_superblock *sb = &h->nvh_sb;
    if (!t->journal_file_sync) return 0;
    t->journal_file_sync_calls++;
    if (fd < START_OPEN_FILE || fd >= MAX_OPEN_FILE || !sb->sb_file_table[fd].used)
        return -EBADF;
    if (INODE_ENTRY_NUM != 1 || !nvfuse_process_model_is_standalone() ||
        nvj_version(t->journal) != 3 || t->journal_aio ||
        t->journal_file_sync_unsafe || nvj_pending_blocks(t->journal) ||
        !nvj_active(t->journal) || nvj_full_data(t->journal)) return 0;
    if (nvj_error(t->journal) || nvj_recovery_required(t->journal)) return -EIO;
    struct file_sync_plan *p = t->journal_file_sync_workspace;
    if (!p) {
        p = malloc(sizeof(*p));
        if (!p) return 0;
        t->journal_file_sync_workspace = p;
    }
    p->sb = sb; p->count = p->groups = p->nodes = p->allocations = 0;
    int rc = file_sync_collect(h, fd, p);
    if (rc == -EAGAIN) return 0;
    if (rc) return rc;
    /* Inode acquisition may evict dirty metadata. Recheck after planning. */
    if (nvj_pending_blocks(t->journal) || t->journal_file_sync_unsafe) return 0;
    unsigned data = 0, deferred = 0;
    struct list_head *ptr, *next;
    inode_t ino = sb->sb_file_table[fd].ino;
    SPINLOCK_LOCK(&sb->sb_bm->bm_lock);
    list_for_each_safe(ptr, next, &sb->sb_bm->bm_list[BUFFER_TYPE_DIRTY]) {
        struct nvfuse_buffer_cache *bc = list_entry(ptr, struct nvfuse_buffer_cache, bc_list);
        if (bc->bc_ino == ino) data++;
        else deferred++;
    }
    SPINLOCK_UNLOCK(&sb->sb_bm->bm_lock);
    /* Protected/reused data can also enter the overlay; budget conservatively. */
    if ((u64)p->count + data > nvj_budget(t->journal)) return 0;
    nvfuse_flush_dirty_inode_data(sb, ino);
    for (unsigned i = 0; i < p->count && !rc; i++)
        rc = nvj_transfer(t->journal, (u64)p->home[i] * CLUSTER_SIZE,
                          CLUSTER_SIZE, p->image[i], 1);
    if (!rc) {
        int overlap = cache_window_open && cache_window_open(h,0);
        rc = nvj_commit(t->journal);
        if (overlap) cache_window_close();
    }
    if (rc) { nvj_require_recovery(t->journal); return rc; }
    rc = nvj_begin(t->journal, 0, 0);
    if (rc) { nvj_require_recovery(t->journal); return rc; }
    t->journal_dirty = nvfuse_get_dirty_count(sb) != 0;
    t->journal_file_sync_hits++;
    t->journal_file_sync_deferred += deferred;
    t->journal_file_sync_allocations += p->allocations;
    return 1;
}
