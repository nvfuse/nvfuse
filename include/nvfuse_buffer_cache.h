/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 30/10/2016
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*/

#include "rte_spinlock.h"
#include "rte_atomic.h"
#include "nvfuse_config.h"
#include "nvfuse_core.h"
#include "list.h"

#ifndef __NVFUSE_BUFFER_CACHE_H__
#define __NVFUSE_BUFFER_CACHE_H__

#define INSERT_HEAD 0
#define INSERT_TAIL 1

/* Buffer List Types */
#define BUFFER_TYPE_UNUSED		0
#define BUFFER_TYPE_REF			1
#define BUFFER_TYPE_CLEAN		2
#define BUFFER_TYPE_DIRTY		3
#define BUFFER_TYPE_FLUSHING	4
#define BUFFER_TYPE_NUM			5

static inline s8 *buffer_type_to_str(s32 type)
{
	switch (type) {
	case BUFFER_TYPE_UNUSED:
		return "UNUSED";
		break;
	case BUFFER_TYPE_REF:
		return "REF";
		break;
	case BUFFER_TYPE_CLEAN:
		return "CLEAN";
		break;
	case BUFFER_TYPE_DIRTY:
		return "DIRTY";
		break;
	default:
		break;
	}

	return "FLUSHING";
}

/* Buffer Status */
#define BUFFER_STATUS_UNUSED	0
#define BUFFER_STATUS_CLEAN		1
#define BUFFER_STATUS_DIRTY		2
#define BUFFER_STATUS_LOAD		3
#define BUFFER_STATUS_META		4
#define BUFFER_STATUS_MAX		5

#define DIRTY_FLUSH_DELAY		0
#define DIRTY_FLUSH_FORCE		1

/* buffer head to track dirty buffer for each inode */
struct nvfuse_buffer_head {
	rte_spinlock_t bh_lock;		/* spin lock */
	struct list_head bh_dirty_list; /* metadata buffer list for specific inode */
#ifdef USE_RBNODE
	struct rb_node bh_dirty_rbnode;
#endif

	struct list_head bh_bc_list;	/* bc_list for linked buffer list*/

	struct list_head bh_aio_list;  /* aio_list */

	struct nvfuse_inode_ctx *bh_ictx; /* inode context pointer */

	struct nvfuse_buffer_cache *bh_bc; /* pointer to actual buffer head */
	s32 bh_status; /* status (e.g., clean, dirty, meta) */
	s8 *bh_buf;
};

/* buffer cache allocated to each physical block */
struct nvfuse_buffer_cache {
	struct hlist_node bc_hash;	/* hash list */
	struct list_head bc_list;	/* main buffer list */
	u32 bc_list_type;		/* buffer status (e.g., clean, dirty, unused) */

	union {
		u64 bc_bno;				/* buffer number (type | inode | block number)*/
		struct {
			u64 bc_lbno: 32;	/* logical block number */
			u64 bc_ino: 32;		/* inode number */
		};
	};
	/* these above variables can be protected by bm->bm_lock */

	rte_spinlock_t bc_lock;		/* spin lock */
	u32 bc_dirty: 1;			/* dirty status */
	u32 bc_load	: 1;			/* data loaded from storage */
	u32 bc_locked: 1;
	u32	bc_flush: 1;
	u32	bc_temp: 28;			/* FIXED: to be removed */

	rte_atomic32_t bc_ref;		/* reference count*/

	struct list_head bc_bh_head; /* buffer list to retrieve */
	rte_atomic32_t bc_bh_count;
	pbno_t bc_pno;				/* physical block no*/

	s8 *bc_buf;					/* actual buffered data */

	struct nvfuse_superblock *bc_sb; /* FIXME: it must be eliminated. */
};

/* Buffer Manager State Definition */
#define BM_STATE_UNINITIALIZED	0
#define BM_STATE_RUNNING		1
#define BM_STATE_LOCKED			2
#define BM_STATE_FINALIZED		3

struct nvfuse_buffer_manager {
	/* block buffer manager */
	rte_spinlock_t bm_lock; /* spin lock */

	struct list_head bm_list[BUFFER_TYPE_NUM];
	struct hlist_head bm_hash[HASH_NUM + 1]; /* regular hash list and unused hash list (1) */

	rte_atomic32_t bm_list_count[BUFFER_TYPE_NUM];
	rte_atomic32_t bm_hash_count[HASH_NUM + 1];
	s32 bm_cache_size;

	u64 bm_cache_ref;
	u64 bm_cache_hit;
	s32 bm_state;
};

/*
 * Buffer Cache (bc) and Buffer Head (bh) Prototype Declration
 */

/* init buffer cache structure with buffer size */
int nvfuse_init_buffer_cache(struct nvfuse_superblock *sb, s32 buffer_size);
/* destroy buffer cache structure */
void nvfuse_deinit_buffer_cache(struct nvfuse_superblock *sb);
/* init buffer cache (bc) */
void nvfuse_init_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc);
/* alloc buffer head (bh) using memppol */
struct nvfuse_buffer_head *nvfuse_alloc_buffer_head(struct nvfuse_superblock *sb);
/* free buffer head (bh) using memppol */
void nvfuse_free_buffer_head(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh);
/* alloc buffer cache (bc) using mempool */
struct nvfuse_buffer_cache *nvfuse_alloc_bc(struct nvfuse_superblock *sb);
/* free buffer cache (bc) using mempool */
void nvfuse_free_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc);
/* get buffer head (bh) with inode, inode number and lba number */
struct nvfuse_buffer_head *nvfuse_get_bh(struct nvfuse_superblock *sb,
										struct nvfuse_inode_ctx *ictx, 
										inode_t ino, lbno_t lblock, 
										s32 read, s32 is_meta);

struct nvfuse_buffer_head *_nvfuse_get_bh(struct nvfuse_superblock *sb,
											struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lblock, 
											s32 sync_read, s32 is_meta);

struct nvfuse_buffer_cache *nvfuse_get_bc(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ctx, inode_t ino, lbno_t lblock, s32 sync_read);
/* alloc and return buffer_head (bh) with inode, inoe number and lba number */
struct nvfuse_buffer_head *nvfuse_get_new_bh(struct nvfuse_superblock *sb,
											struct nvfuse_inode_ctx *ictx, 
											inode_t ino, lbno_t lblock, s32 is_meta);
/* find out buffer cache (bc) associated with key and lblock */
struct nvfuse_buffer_cache *nvfuse_find_bc(struct nvfuse_superblock *sb, u64 key, lbno_t lblock);
/* replace the buffer cahce located at the end of the LRU list*/
struct nvfuse_buffer_cache *nvfuse_replace_buffer_cache(struct nvfuse_superblock *sb, u64 key);
/* move buffer cache (bc) to another list with spinlock */
void nvfuse_move_buffer_list(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc,
							 s32 buffer_type, s32 tail);
/* move buffer cache (bc) to another list with no spinlock */
void nvfuse_move_buffer_list_nolock(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc,
							 s32 buffer_type, s32 tail);
void nvfuse_move_bc_to_unused_list(struct nvfuse_superblock *sb, u64 key);
/* return the number of dirty buffer caches (e.g., 4K dirty buffers) */
s32 nvfuse_get_dirty_count(struct nvfuse_superblock *sb);
/* mark the buffer head as dirty */
void nvfuse_mark_dirty_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh);
/* lookup the buffer cache (bc) related to a given key */
struct nvfuse_buffer_cache *nvfuse_hash_lookup(struct nvfuse_buffer_manager *bm, u64 key);
/* set bh status */
void nvfuse_set_bh_status(struct nvfuse_buffer_head *bh, s32 status);
/* clear bh status */
void nvfuse_clear_bh_status(struct nvfuse_buffer_head *bh, s32 status);

/* find buffer head that is keeping by inode context */
struct nvfuse_buffer_head *nvfuse_find_bh_in_ictx(struct nvfuse_superblock *sb,
												struct nvfuse_inode_ctx *ictx,
												inode_t ino, lbno_t lbno);
/* insert dirty buffer head (bh) into ictx to track dirty buffers */
s32 nvfuse_insert_dirty_bh_to_ictx(struct nvfuse_buffer_head *bh, struct nvfuse_inode_ctx *ictx);

/* forget buffer head, but this is not referenced by any other functions */
//s32 nvfuse_forget_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh);

/* release buffer head, but this is not referenced by any other functions */
void nvfuse_release_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh, s32 tail, s32 dirty);
void nvfuse_release_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc, s32 tail, s32 dirty);

/* remove several buffer heads which are linked into bc */
void nvfuse_remove_bhs_in_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc);

/* insert buffer head into rbree */
int nvfuse_rbnode_insert_bh(struct rb_root *root, struct nvfuse_buffer_head *bh);
struct nvfuse_buffer_head *nvfuse_rbnode_search(struct rb_root *root, u64 bno);
/* increase the number of buffer caches */
int nvfuse_add_buffer_cache(struct nvfuse_superblock *sb, int nr);
/* remove buffer cache if file is deleted */
s32 nvfuse_remove_buffer_cache(struct nvfuse_superblock *sb, s32 nr_buffers);
/* inc bc->bc_ref */
void nvfuse_inc_bc_ref(struct nvfuse_buffer_cache *bc);
/* dec bc->bc_ref */
void nvfuse_dec_bc_ref(struct nvfuse_buffer_cache *bc);
/* deubg bh info */
void nvfuse_print_bh(struct nvfuse_buffer_head *bh);

#endif //__NVFUSE_BUFFER_CACHE_H__
