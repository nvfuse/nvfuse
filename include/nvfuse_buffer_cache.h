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

#include "nvfuse_core.h"
#include "list.h"

#ifndef _LRU_CACHE_H
#define _LRU_CACHE_H

//#define HASH_NUM (15331)
#define HASH_NUM (52631)

#define INSERT_HEAD 0
#define INSERT_TAIL 1

#define BUFFER_TYPE_UNUSED	0
#define BUFFER_TYPE_REF		1
#define BUFFER_TYPE_CLEAN	2
#define BUFFER_TYPE_DIRTY	3
#define BUFFER_TYPE_NUM		4

#define BUFFER_STATUS_UNUSED	0
#define BUFFER_STATUS_CLEAN		1
#define BUFFER_STATUS_DIRTY		2
#define BUFFER_STATUS_LOAD		3
#define BUFFER_STATUS_META		4

struct nvfuse_buffer_head{
	s32 bh_type;
	struct list_head bh_list;
	struct hlist_node bh_hash;
	struct nvfuse_superblock *bh_sb;

	u64 bh_bno;
	lbno_t bh_lbno;//logical block no
	inode_t bh_ino; // inode number
	pbno_t bh_pno; //physical block no
	
	s32 bh_meta;
	u32 bh_dirty;
	u32 bh_load;
	s32 bh_ref;
	s8 *bh_buf;
}; 

struct nvfuse_buffer_manager{
	struct list_head bm_list[BUFFER_TYPE_NUM];	
	struct hlist_head bm_hash[HASH_NUM + 1]; /* regular hash list and unused hash list (1) */

	s32 bm_list_count[BUFFER_TYPE_NUM];
	s32 bm_hash_count[HASH_NUM + 1];
	s32 bm_cache_size;
	u64 bm_cache_ref;
	u64 bm_cache_hit;
};

int nvfuse_init_buffer_cache(struct nvfuse_superblock *sb);
void nvfuse_free_buffer_cache(struct nvfuse_superblock *sb);

struct nvfuse_buffer_head *nvfuse_alloc_bh();
struct nvfuse_buffer_head *nvfuse_get_bh(struct nvfuse_superblock *sb,inode_t ino,lbno_t lblock,int read);
struct nvfuse_buffer_head *nvfuse_find_bh(struct nvfuse_superblock *sb, u64 key,inode_t ino, lbno_t lblock);
struct nvfuse_buffer_head *nvfuse_replcae_buffer(struct nvfuse_superblock *sb,u64 key);
struct nvfuse_buffer_head *nvfuse_get_new_bh(struct nvfuse_superblock *sb, inode_t ino, lbno_t lblock);
void nvfuse_move_buffer_type(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh, s32 buffer_type, s32 tail);

s32 nvfuse_get_dirty_count(struct nvfuse_superblock *sb);

s32 nvfuse_mark_dirty_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh);
struct nvfuse_buffer_head *nvfuse_hash_lookup(struct nvfuse_buffer_manager *bm, u64 key);

#endif
