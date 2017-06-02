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
	struct hlist_node bc_hash;	/* hash list*/
	struct list_head bc_list;	/* main buffer list */
	struct list_head bc_bh_head; /* buffer list to retrieve */
	s32 bc_bh_count;

	union {
		u64 bc_bno;					/* buffer number (type | inode | block number)*/
		struct {
			u64 bc_lbno: 32;				/* logical block number */
			u64 bc_ino: 32;				/* inode number */
		};
	};

	pbno_t bc_pno;				/* physical block no*/

	u32 bc_dirty: 1;				/* dirty status */
	u32 bc_load	: 1;				/* data loaded from storage */
	u32 bc_ref	: 27;					/* reference count*/
	u32 bc_list_type: 3;				/* buffer status (e.g., clean, dirty, unused) */

	s8 *bc_buf;					/* actual buffered data */

	struct nvfuse_superblock *bc_sb; /* FIXME: it must be eliminated. */
};

struct nvfuse_buffer_manager {
	/* block buffer manager */
	struct list_head bm_list[BUFFER_TYPE_NUM];
	struct hlist_head bm_hash[HASH_NUM + 1]; /* regular hash list and unused hash list (1) */

	s32 bm_list_count[BUFFER_TYPE_NUM];
	s32 bm_hash_count[HASH_NUM + 1];
	s32 bm_cache_size;

	u64 bm_cache_ref;
	u64 bm_cache_hit;
};

/* inode context cache manager */
struct nvfuse_ictx_manager {
	struct list_head ictxc_list[BUFFER_TYPE_NUM];
	struct hlist_head ictxc_hash[HASH_NUM + 1]; /* regular hash list and unused hash list (1) */

	void *ictx_buf; /* allocated by spdk_zmalloc() */
	s32 ictxc_list_count[BUFFER_TYPE_NUM];
	s32 ictxc_hash_count[HASH_NUM + 1];
	s32 ictxc_cache_size;
	u64 ictxc_cache_ref;
	u64 ictxc_cache_hit;
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
/* alloc and return buffer_head (bh) with inode, inoe number and lba number */
struct nvfuse_buffer_head *nvfuse_get_new_bh(struct nvfuse_superblock *sb,
											struct nvfuse_inode_ctx *ictx, 
											inode_t ino, lbno_t lblock, s32 is_meta);
/* find out buffer cache (bc) associated with key and lblock */
struct nvfuse_buffer_cache *nvfuse_find_bc(struct nvfuse_superblock *sb, u64 key, lbno_t lblock);
/* replace the buffer cahce located at the end of the LRU list*/
struct nvfuse_buffer_cache *nvfuse_replace_buffer_cache(struct nvfuse_superblock *sb, u64 key);
/* move buffer cache (bc) to another list */
void nvfuse_move_buffer_list(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc,
							 s32 buffer_type, s32 tail);
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

/*
 * Inode Context (ictx) Prototype Declration
 */

/* initialize inode context cache manager */
int nvfuse_init_ictx_cache(struct nvfuse_superblock *sb);
/* destroy inode context cache manager */
void nvfuse_deinit_ictx_cache(struct nvfuse_superblock *sb);

/* alloc inode context to keep inode contents */
struct nvfuse_inode_ctx *nvfuse_alloc_ictx(struct nvfuse_superblock *sb);
/* get inode context */
struct nvfuse_inode_ctx *nvfuse_get_ictx(struct nvfuse_superblock *sb, inode_t ino);
/* release inode context */
s32 nvfuse_release_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 dirty);
/* find buffer head that is keeping by inode context */
struct nvfuse_buffer_head *nvfuse_find_bh_in_ictx(struct nvfuse_superblock *sb,
												struct nvfuse_inode_ctx *ictx,
												inode_t ino, lbno_t lbno);
/* insert a new ictx to the list */
void nvfuse_insert_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);
/* move ictx to other type of list */
void nvfuse_move_ictx_list(struct nvfuse_superblock *sb, 
							struct nvfuse_inode_ctx *ictx,
							s32 desired_type);
/* to be removed */
//s32 nvfuse_free_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);
//
/* insert dirty buffer head (bh) into ictx to track dirty buffers */
s32 nvfuse_insert_dirty_bh_to_ictx(struct nvfuse_buffer_head *bh, struct nvfuse_inode_ctx *ictx);

/* forget buffer head, but this is not referenced by any other functions */
s32 nvfuse_forget_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh);

/* release buffer head, but this is not referenced by any other functions */
s32 nvfuse_release_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh, s32 tail, s32 dirty);

/* remove several buffer heads which are linked into bc */
void nvfuse_remove_bhs_in_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc);

/* reset ictx structure */
void nvfuse_init_ictx(struct nvfuse_inode_ctx *ictx);
/* lookup ictx structure with inode number (ino) */
struct nvfuse_inode_ctx *nvfuse_ictx_hash_lookup(struct nvfuse_ictx_manager *ictxc, inode_t ino);
/* relcae ictx structure */
struct nvfuse_inode_ctx *nvfuse_replcae_ictx(struct nvfuse_superblock *sb);
/* insert buffer head into rbree */
int nvfuse_rbnode_insert_bh(struct rb_root *root, struct nvfuse_buffer_head *bh);
struct nvfuse_buffer_head *nvfuse_rbnode_search(struct rb_root *root, u64 bno);
/* increase the number of buffer caches */
int nvfuse_add_buffer_cache(struct nvfuse_superblock *sb, int nr);
/* remove buffer cache if file is deleted */
s32 nvfuse_remove_buffer_cache(struct nvfuse_superblock *sb, s32 nr_buffers);
/* replace ictx buffer in list */
struct nvfuse_inode_ctx *nvfuse_replace_ictx(struct nvfuse_superblock *sb);

#endif //__NVFUSE_BUFFER_CACHE_H__
