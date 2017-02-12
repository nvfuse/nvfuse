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

#ifndef _LRU_CACHE_H
#define _LRU_CACHE_H

#define INSERT_HEAD 0
#define INSERT_TAIL 1

#define BUFFER_TYPE_UNUSED		0
#define BUFFER_TYPE_REF			1
#define BUFFER_TYPE_CLEAN		2
#define BUFFER_TYPE_DIRTY		3
#define BUFFER_TYPE_FLUSHING	4
#define BUFFER_TYPE_NUM			5

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
	s32 bh_seq;
	s8 *bh_buf;	
};

/* buffer cache allocated to each physical block */
struct nvfuse_buffer_cache {
	struct hlist_node bc_hash;	/* hash list*/
	struct list_head bc_list;	/* main buffer list */	
	struct list_head bc_bh_head; /* buffer list to retrieve */
	s32 bc_bh_count; 

	s32 bc_list_type;				/* buffer status (e.g., clean, dirty, unused) */
	s64 bc_bno;					/* buffer number (type | inode | block number)*/
	lbno_t bc_lbno;				/* logical block number */
	inode_t bc_ino;				/* inode number */
	pbno_t bc_pno;				/* physical block no*/

	u32 bc_dirty:1;				/* dirty status */
	u32 bc_load	:1;				/* data loaded from storage */
	s32 bc_ref	:30;					/* reference count*/
	u32 bc_hit;
	
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


int nvfuse_init_buffer_cache(struct nvfuse_superblock *sb, s32 buffer_size);
void nvfuse_deinit_buffer_cache(struct nvfuse_superblock *sb);

struct nvfuse_buffer_cache *nvfuse_alloc_bc(struct nvfuse_superblock *sb);
struct nvfuse_buffer_head *nvfuse_get_bh(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lblock, s32 read, s32 is_meta);
struct nvfuse_buffer_cache *nvfuse_find_bc(struct nvfuse_superblock *sb, u64 key, lbno_t lblock);
struct nvfuse_buffer_cache *nvfuse_replcae_buffer(struct nvfuse_superblock *sb,u64 key);
struct nvfuse_buffer_head *nvfuse_get_new_bh(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lblock, s32 is_meta);
void nvfuse_move_buffer_type(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bh, s32 buffer_type, s32 tail);

s32 nvfuse_get_dirty_count(struct nvfuse_superblock *sb);

s32 nvfuse_mark_dirty_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh);
struct nvfuse_buffer_cache *nvfuse_hash_lookup(struct nvfuse_buffer_manager *bm, u64 key);

struct nvfuse_inode_ctx *nvfuse_get_ictx(struct nvfuse_superblock *sb, inode_t ino);
s32 nvfuse_release_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 dirty);
int nvfuse_init_ictx_cache(struct nvfuse_superblock *sb);
void nvfuse_deinit_ictx_cache(struct nvfuse_superblock *sb);
struct nvfuse_inode_ctx *nvfuse_alloc_ictx(struct nvfuse_superblock *sb);
struct nvfuse_buffer_head *nvfuse_find_bh_in_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lbno);
void nvfuse_set_bh_status(struct nvfuse_buffer_head *bh, s32 status);
void nvfuse_clear_bh_status(struct nvfuse_buffer_head *bh, s32 status);
void nvfuse_insert_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);
void nvfuse_move_ictx_type(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 desired_type);
s32 nvfuse_insert_dirty_bh_to_ictx(struct nvfuse_buffer_head *bh, struct nvfuse_inode_ctx *ictx);

s32 nvfuse_forget_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh);
void nvfuse_remove_bh_in_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc);

void nvfuse_init_ictx(struct nvfuse_inode_ctx *ictx);
struct nvfuse_inode_ctx *nvfuse_ictx_hash_lookup(struct nvfuse_ictx_manager *ictxc, inode_t ino);
struct nvfuse_inode_ctx *nvfuse_replcae_ictx(struct nvfuse_superblock *sb);

int nvfuse_rbnode_insert(struct rb_root *root, struct nvfuse_buffer_head *bh);
struct nvfuse_buffer_head *nvfuse_rbnode_search(struct rb_root *root, u64 bno);
int nvfuse_add_buffer_cache(struct nvfuse_superblock *sb, int nr);
s32 nvfuse_remove_buffer_cache(struct nvfuse_superblock *sb, s32 nr_buffers);

#endif
