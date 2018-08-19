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
#include "nvfuse_buffer_cache.h"
#include "list.h"

#ifndef __NVFUSE_INODE_CACHE_H__
#define __NVFUSE_INODE_CACHE_H__

/* inode context cache manager */
struct nvfuse_ictx_manager {
	rte_spinlock_t ictxc_lock; /* spin lock */
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
void nvfuse_release_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 dirty);
/* insert a new ictx to the list */
void nvfuse_insert_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);
/* move ictx to other type of list */
void nvfuse_move_ictx_list(struct nvfuse_superblock *sb, 
							struct nvfuse_inode_ctx *ictx,
							s32 desired_type);
/* to be removed */
//s32 nvfuse_free_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);
//
/* reset ictx structure */
void nvfuse_init_ictx(struct nvfuse_inode_ctx *ictx, inode_t ino);
/* lookup ictx structure with inode number (ino) */
struct nvfuse_inode_ctx *nvfuse_ictx_hash_lookup(struct nvfuse_ictx_manager *ictxc, inode_t ino);
/* replace ictx buffer in list */
struct nvfuse_inode_ctx *nvfuse_replace_ictx(struct nvfuse_superblock *sb);

/* debug ictx list */
void nvfuse_print_ictx_list(struct nvfuse_superblock *sb, s32 type);
void nvfuse_print_ictx_list_count(struct nvfuse_superblock *sb, s32 type);
void nvfuse_print_ictx(struct nvfuse_inode_ctx *ictx);
s8 *nvfuse_decode_ictx_status(struct nvfuse_inode_ctx *ictx);

#endif //__NVFUSE_INODE_CACHE_H__ 
