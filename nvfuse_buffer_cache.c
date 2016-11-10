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


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "nvfuse_core.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_malloc.h"
#include "list.h"

void nvfuse_move_buffer_type(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh, s32 desired_type, s32 tail){
	struct nvfuse_buffer_manager *bm = sb->sb_bm;		
	s32 res;

	list_del(&bh->bh_list);
	bm->bm_list_count[bh->bh_type]--;
	
	bh->bh_type = desired_type;
	if(tail)
		list_add_tail(&bh->bh_list, &bm->bm_list[bh->bh_type]);
	else
		list_add(&bh->bh_list, &bm->bm_list[bh->bh_type]);
	bm->bm_list_count[bh->bh_type]++;

	hlist_del(&bh->bh_hash);
	if (desired_type == BUFFER_TYPE_UNUSED) {
		hlist_add_head(&bh->bh_hash, &bm->bm_hash[HASH_NUM]);
		bm->bm_hash_count[HASH_NUM]++;
	}
	else 
	{
		hlist_add_head(&bh->bh_hash, &bm->bm_hash[bh->bh_bno % HASH_NUM]);
		bm->bm_hash_count[bh->bh_bno % HASH_NUM]++;
	}
}

void nvfuse_init_buffer_head(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh){
	bh->bh_bno = 0;
	bh->bh_dirty = 0;
	bh->bh_ino = 0;
	bh->bh_lbno = 0;
	bh->bh_load = 0;	
	bh->bh_pno = 0;
	bh->bh_ref = 0;
	memset(bh->bh_buf, 0x00, CLUSTER_SIZE);	
}

struct nvfuse_buffer_head *nvfuse_replcae_buffer(struct nvfuse_superblock *sb, u64 key){	
	
	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_head *bh;
	struct list_head *remove_ptr;
	s32 type = 0;

	if (bm->bm_list_count[BUFFER_TYPE_UNUSED])
	{
		type = BUFFER_TYPE_UNUSED;
	}
	else if (bm->bm_list_count[BUFFER_TYPE_CLEAN])
	{
		type = BUFFER_TYPE_CLEAN;
	}
	else
	{
		type = BUFFER_TYPE_DIRTY;
		printf(" Warning: it runs out of clean buffers.\n");
		printf(" Warning: it needs to flush dirty pages to disks.\n");
		nvfuse_check_flush_segment(sb);
	}

	assert(bm->bm_list_count[type]);
	remove_ptr = (struct list_head *)(&bm->bm_list[type])->prev;
	do {
		bh = list_entry(remove_ptr, struct nvfuse_buffer_head, bh_list);
		if (bh->bh_ref == 0)
			break;
		remove_ptr = remove_ptr->prev;
		assert(remove_ptr != &bm->bm_list[type]);
	} while (1);

	/* remove list */
	list_del(&bh->bh_list);
	/* remove hlist */
	hlist_del(&bh->bh_hash);
	bm->bm_list_count[type]--;
	if (type == BUFFER_TYPE_UNUSED)
		bm->bm_hash_count[HASH_NUM]--;
	else
		bm->bm_hash_count[bh->bh_bno % HASH_NUM]--;
	return bh;
}


struct nvfuse_buffer_head *nvfuse_hash_lookup(struct nvfuse_buffer_manager *bm, u64 key)
{
	struct hlist_node *node;
	struct hlist_head *head;
	struct nvfuse_buffer_head *bh;

	head = &bm->bm_hash[key % HASH_NUM];
	hlist_for_each(node, head) {
		bh = hlist_entry(node, struct nvfuse_buffer_head, bh_hash);
		if (bh->bh_bno == key)
			return bh;
	}

	return NULL;
}

struct nvfuse_buffer_head *nvfuse_find_bh(struct nvfuse_superblock *sb, u64 key, inode_t ino, lbno_t lblock){
	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_head *bh;

	bh = nvfuse_hash_lookup(sb->sb_bm, key);
	if(bh){ /* in case of cache hit */
		bh->bh_lbno = lblock;

		// cache move to mru position
		list_del(&bh->bh_list);
		bm->bm_list_count[bh->bh_type]--;

		list_add(&bh->bh_list, &bm->bm_list[bh->bh_type]);
		bm->bm_list_count[bh->bh_type]++;
	}else{
		bh = nvfuse_replcae_buffer(sb, key);
		if (bh) {
			nvfuse_init_buffer_head(sb, bh);			
			/* hash list insertion */
			hlist_add_head(&bh->bh_hash, &bm->bm_hash[key % HASH_NUM]);			
			bm->bm_hash_count[key % HASH_NUM]++;

			{
				s32 type = BUFFER_TYPE_REF;				
				/* list insertion */
				list_add(&bh->bh_list, &bm->bm_list[type]);
				/* increase count of clean list */
				bm->bm_list_count[type]++;
				/* initialize key and type values*/
				bh->bh_bno = key;
				bh->bh_type = type;
			}
		}
	}
	
	return bh;	
}

struct nvfuse_buffer_head *nvfuse_get_bh(struct nvfuse_superblock *sb, inode_t ino, lbno_t lblock, int read){
	struct nvfuse_buffer_head *bh;	
	u64 key = 0;
	
	nvfuse_make_key(ino, lblock, &key, NVFUSE_BP_TYPE_DATA);
	bh = nvfuse_find_bh(sb, key, ino, lblock);
	if (bh == NULL)
	{
		return NULL;
	}

	if(!bh->bh_pno)
	{	/* logical to physical address translation */
		bh->bh_pno = nvfuse_get_pbn(sb, ino, lblock);
	}

	if(bh->bh_pno)
	{
		if(read && !bh->bh_load)
		{
			sb->sb_read_blocks++;
			sb->sb_read_ios++;	
			nvfuse_read_block(bh->bh_buf, bh->bh_pno, nvfuse_io_manager);			
		}
	}else if(!bh->bh_pno && read && !bh->bh_load){
		/* FIXME: how can we handle this case? */
		bh->bh_pno = nvfuse_get_pbn(sb, ino, lblock);
		printf(" Error: bh has no pblock addr \n");
		bh->bh_pno = nvfuse_get_pbn(sb, ino, lblock);
		assert(0);		
	}

	bh->bh_load = 1;
	bh->bh_ino = ino;
	bh->bh_lbno = lblock;
	bh->bh_ref++;
	assert(bh->bh_ref >= 0);
	return bh;
}

struct nvfuse_buffer_head *nvfuse_get_new_bh(struct nvfuse_superblock *sb, inode_t ino, lbno_t lblock) {
	struct nvfuse_buffer_head *bh;
	u64 key = 0;
	
	nvfuse_make_key(ino, lblock, &key, NVFUSE_BP_TYPE_DATA);
	bh = nvfuse_find_bh(sb, key, ino, lblock);
	if (bh == NULL)
	{
		return NULL;
	}

	if (!bh->bh_pno)
	{	/* logical to physical address translation */
		bh->bh_pno = nvfuse_get_pbn(sb, ino, lblock);
	}

	if (bh->bh_pno)
	{
		memset(bh->bh_buf, 0x00, CLUSTER_SIZE);
	}

	if (bh->bh_dirty)
		bh->bh_dirty = 1;

	bh->bh_load = 1;
	bh->bh_ino = ino;
	bh->bh_lbno = lblock;
	bh->bh_ref++;
	assert(bh->bh_ref == 1);
	nvfuse_mark_dirty_bh(sb, bh);

	return bh;
}


struct nvfuse_buffer_head *nvfuse_alloc_bh(){
	struct nvfuse_buffer_head *bh;

	bh = (struct nvfuse_buffer_head *)nvfuse_malloc(sizeof(struct nvfuse_buffer_head));
	if (bh == NULL) {
		printf(" nvfuse_malloc error \n");
		return bh;
	}
	memset(bh, 0x00, sizeof(struct nvfuse_buffer_head));
		
	return bh;
}

int nvfuse_init_buffer_cache(struct nvfuse_superblock *sb){
	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_head *bh;
	s32 i, res;
	
	for (i = BUFFER_TYPE_UNUSED; i < BUFFER_TYPE_NUM; i++) {
		INIT_LIST_HEAD(&bm->bm_list[i]);
		bm->bm_list_count[i] = 0;
	}
		
	for (i = 0; i < HASH_NUM + 1; i++) {
		INIT_HLIST_HEAD(&bm->bm_hash[i]);
		bm->bm_hash_count[i] = 0;
	}

	/* alloc unsed list buffer cache */
	for(i = 0;i < NVFUSE_BUFFER_SIZE;i++){			
		bh = nvfuse_alloc_bh(sb);
		if (!bh) {
			return -1;
		}

		bh->bh_sb = sb;
		bh->bh_buf = (s8 *)nvfuse_malloc(CLUSTER_SIZE);
		if (bh->bh_buf == NULL) {
			printf(" nvfuse_malloc error \n");
			return -1;
		}

		memset(bh->bh_buf, 0x00, CLUSTER_SIZE);

		list_add(&bh->bh_list, &bm->bm_list[BUFFER_TYPE_UNUSED]);
		hlist_add_head(&bh->bh_hash, &bm->bm_hash[HASH_NUM]);
		bm->bm_hash_count[HASH_NUM]++;
		bm->bm_list_count[BUFFER_TYPE_UNUSED]++;
	}	

	return 0;
}

void nvfuse_free_buffer_cache(struct nvfuse_superblock *sb)
{	
	struct list_head *head;
	struct list_head *ptr, *temp;
	struct nvfuse_buffer_head *bh;
	s32 i, res;
	s32 type;
	s32 removed_count = 0;

	/* dealloc buffer cache */
	for (type = BUFFER_TYPE_UNUSED; type < BUFFER_TYPE_NUM; type++) {
		head = &sb->sb_bm->bm_list[type];
		list_for_each_safe(ptr, temp, head) {
			bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_list);
			assert(!bh->bh_dirty);
			list_del(&bh->bh_list);
			nvfuse_free(bh->bh_buf);
			nvfuse_free(bh);
			removed_count++;
		}
	}
	assert(removed_count == NVFUSE_BUFFER_SIZE);
}


s32 nvfuse_mark_dirty_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh) 
{
	if (bh == NULL)
		return 0;
		
	bh->bh_dirty = 1;
	//nvfuse_move_buffer_type(sb, bh, BUFFER_TYPE_DIRTY, 0);
	return 0;
}

s32 nvfuse_release_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh, s32 tail, s32 dirty) {

	if (bh == NULL)
		return 0;

	bh->bh_ref--;
	if (bh->bh_ref < 0)
		bh->bh_ref = -1;
	assert(bh->bh_ref >= 0);

	if (dirty)
	{
		bh->bh_dirty = 1;
		nvfuse_move_buffer_type(sb, bh, BUFFER_TYPE_DIRTY, tail);
	}
	else
	{
		if (!bh->bh_dirty) {
			bh->bh_dirty = 0;
			nvfuse_move_buffer_type(sb, bh, BUFFER_TYPE_CLEAN, tail);
		}
		else
		{
			nvfuse_move_buffer_type(sb, bh, BUFFER_TYPE_DIRTY, tail);
		}
	}

	return 0;
}

__inline void unused_count_dec(struct nvfuse_superblock *sb)
{
	sb->sb_bm->bm_list_count[BUFFER_TYPE_UNUSED]--;
}

__inline void unused_count_inc(struct nvfuse_superblock *sb)
{
	sb->sb_bm->bm_list_count[BUFFER_TYPE_UNUSED]++;
}


__inline void clean_count_dec(struct nvfuse_superblock *sb)
{
	sb->sb_bm->bm_list_count[BUFFER_TYPE_CLEAN]--;
}

__inline void clean_count_inc(struct nvfuse_superblock *sb)
{
	sb->sb_bm->bm_list_count[BUFFER_TYPE_CLEAN];
}

__inline void dirty_count_dec(struct nvfuse_superblock *sb)
{
	sb->sb_bm->bm_list_count[BUFFER_TYPE_DIRTY]--;
}

__inline void dirty_count_inc(struct nvfuse_superblock *sb)
{
	sb->sb_bm->bm_list_count[BUFFER_TYPE_DIRTY]++;
}

s32 nvfuse_get_dirty_count(struct nvfuse_superblock *sb) {
	return sb->sb_bm->bm_list_count[BUFFER_TYPE_DIRTY];
}
