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

#include "spdk/env.h"
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_malloc.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
//#define NDEBUG
#include <assert.h>

#include "nvfuse_core.h"
#include "nvfuse_dep.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_inode_cache.h"
#include "nvfuse_malloc.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_control_plane.h"
#include "nvfuse_debug.h"
#include "list.h"
#include "rbtree.h"

struct nvfuse_inode_ctx *nvfuse_ictx_hash_lookup(struct nvfuse_ictx_manager *ictxc, inode_t ino)
{
	struct hlist_node *node;
	struct hlist_head *head;
	struct nvfuse_inode_ctx *ictx;

	head = &ictxc->ictxc_hash[ino % HASH_NUM];
	hlist_for_each(node, head) {
		ictx = hlist_entry(node, struct nvfuse_inode_ctx, ictx_hash);
		if (ictx->ictx_ino == ino)
			return ictx;
	}

	return NULL;
}

struct nvfuse_inode_ctx *nvfuse_replace_ictx(struct nvfuse_superblock *sb)
{
	struct nvfuse_ictx_manager *ictxc = sb->sb_ictxc;
	struct nvfuse_inode_ctx *ictx;
	s32 type = 0;

	assert(rte_spinlock_is_locked(&ictxc->ictxc_lock));

	if (ictxc->ictxc_list_count[BUFFER_TYPE_UNUSED]) {
		type = BUFFER_TYPE_UNUSED;
	} else if (ictxc->ictxc_list_count[BUFFER_TYPE_CLEAN]) {
		type = BUFFER_TYPE_CLEAN;
	} else {
		type = BUFFER_TYPE_DIRTY;
		dprintf_warn(BUFFER, " Warning: it runs out of clean buffers.\n");
		dprintf_warn(BUFFER, " Warning: it needs to immediately flush dirty pages to disks.\n");
		nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	}

	assert(ictxc->ictxc_list_count[type]);

	list_for_each_entry_reverse(ictx, &ictxc->ictxc_list[type], ictx_cache_list) {
		SPINLOCK_LOCK(&ictx->ictx_lock);

		/* FIXED: clean list is required for better performance. */
		if (ictx->ictx_ref == 0 &&
		    ictx->ictx_data_dirty_count == 0 &&
		    ictx->ictx_meta_dirty_count == 0)
			goto VICTIM_FOUND;

		SPINLOCK_UNLOCK(&ictx->ictx_lock);
	}

	/* TODO: error handling */
	dprintf_error(BUFFER, " Error: unavailable victim inode.");
	while (1) sleep(1);

VICTIM_FOUND:

	/* remove list */
	list_del(&ictx->ictx_cache_list);
	/* remove hlist */
	hlist_del(&ictx->ictx_hash);
	ictxc->ictxc_list_count[type]--;
	if (type == BUFFER_TYPE_UNUSED)
		ictxc->ictxc_hash_count[HASH_NUM]--;
	else
		ictxc->ictxc_hash_count[ictx->ictx_ino % HASH_NUM]--;

	SPINLOCK_UNLOCK(&ictx->ictx_lock);
	return ictx;
}

struct nvfuse_inode_ctx *nvfuse_alloc_ictx(struct nvfuse_superblock *sb)
{
	struct nvfuse_ictx_manager *ictxc = sb->sb_ictxc;
	struct nvfuse_inode_ctx *ictx;

	SPINLOCK_LOCK(&ictxc->ictxc_lock);
	ictx = nvfuse_replace_ictx(sb);
	SPINLOCK_UNLOCK(&ictxc->ictxc_lock);

	nvfuse_init_ictx(ictx, 0);

	return ictx;
}

void nvfuse_insert_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx)
{
	struct nvfuse_ictx_manager *ictxc = sb->sb_ictxc;
	inode_t ino = ictx->ictx_ino;
	s32 type = BUFFER_TYPE_REF;

	/* hash list insertion */
	hlist_add_head(&ictx->ictx_hash, &ictxc->ictxc_hash[ino % HASH_NUM]);
	ictxc->ictxc_hash_count[ino % HASH_NUM]++;

	/* list insertion */
	list_add(&ictx->ictx_cache_list, &ictxc->ictxc_list[type]);
	/* increase count of clean list */
	ictxc->ictxc_list_count[type]++;
	/* initialize key and type values*/
	ictx->ictx_type = type;

	if (type == BUFFER_TYPE_CLEAN && (ictx->ictx_data_dirty_count || ictx->ictx_meta_dirty_count)) {
		assert(0);
	}
}

void nvfuse_init_ictx(struct nvfuse_inode_ctx *ictx, inode_t ino)
{
	SPINLOCK_INIT(&ictx->ictx_lock);

	INIT_LIST_HEAD(&ictx->ictx_meta_bh_head);
	INIT_LIST_HEAD(&ictx->ictx_data_bh_head);
#ifdef USE_RBNODE
	ictx->ictx_meta_bh_rbroot = RB_ROOT;
	ictx->ictx_data_bh_rbroot = RB_ROOT;
#endif

	ictx->ictx_ino = ino;
	ictx->ictx_meta_dirty_count = 0;
	ictx->ictx_data_dirty_count = 0;

	ictx->ictx_status = INODE_STATE_NEW;
	ictx->ictx_ref = 0;
	ictx->ictx_type = 0;

	ictx->ictx_inode = NULL;
	ictx->ictx_bh = NULL;
}

struct nvfuse_inode_ctx *nvfuse_get_ictx(struct nvfuse_superblock *sb, inode_t ino)
{
	struct nvfuse_ictx_manager *ictxc = sb->sb_ictxc;
	struct nvfuse_inode_ctx *ictx;

	SPINLOCK_LOCK(&ictxc->ictxc_lock);

	ictx = nvfuse_ictx_hash_lookup(sb->sb_ictxc, ino);
	if (unlikely(!ictx)) { 
		/* in case of cache misses */
		ictx = nvfuse_replace_ictx(sb);
		if (ictx) {
			/* init ictx structure */
			nvfuse_init_ictx(ictx, ino);
			assert(ictx->ictx_ino == ino);

			/* insert to ictx list and hash table */
			nvfuse_insert_ictx(sb, ictx);
		} else {
			goto OUT;
		}
	}  
	/* in case of cache hit */
	/* ictx already in the ictx cache */

	/* cache move to mru position */
	list_move(&ictx->ictx_cache_list, &ictxc->ictxc_list[ictx->ictx_type]);

	/* type and count debug */
	if (ictx->ictx_type == BUFFER_TYPE_CLEAN && (ictx->ictx_data_dirty_count ||
			ictx->ictx_meta_dirty_count)) {
		assert(0);
	}

	/* lock ictx */
	SPINLOCK_LOCK(&ictx->ictx_lock);
	/* this inode context is locked until nvfuse_inode_release is called. */
	set_bit(&ictx->ictx_status, INODE_STATE_LOCK);

OUT:
	SPINLOCK_UNLOCK(&ictxc->ictxc_lock);

	return ictx;
}

void nvfuse_move_ictx_list(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
			   s32 desired_type)
{
	struct nvfuse_ictx_manager *ictxc = sb->sb_ictxc;

	SPINLOCK_LOCK(&ictxc->ictxc_lock);

	list_del(&ictx->ictx_cache_list);
	ictxc->ictxc_list_count[ictx->ictx_type]--;

	ictx->ictx_type = desired_type;

	list_add(&ictx->ictx_cache_list, &ictxc->ictxc_list[ictx->ictx_type]);
	ictxc->ictxc_list_count[ictx->ictx_type]++;

	hlist_del(&ictx->ictx_hash);
	if (desired_type == BUFFER_TYPE_UNUSED) {
		hlist_add_head(&ictx->ictx_hash, &ictxc->ictxc_hash[HASH_NUM]);
		ictxc->ictxc_hash_count[HASH_NUM]++;
	} else {
		hlist_add_head(&ictx->ictx_hash, &ictxc->ictxc_hash[ictx->ictx_ino % HASH_NUM]);
		ictxc->ictxc_hash_count[ictx->ictx_ino % HASH_NUM]++;
	}

	SPINLOCK_UNLOCK(&ictxc->ictxc_lock);
}

void nvfuse_release_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 dirty)
{
	s32 type;

	assert (ictx != NULL);

	/* dirty means that a given inode has dirty pages to be written to underlying storage */
#ifdef NVFUSE_KEEP_DIRTY_BH_IN_ICTX
	if (dirty || test_bit(&ictx->ictx_status, INODE_STATE_DIRTY)) {
		/* inode context must contain dirty pages. */
		assert(nvfuse_inode_has_dirty(ictx));
		type = BUFFER_TYPE_DIRTY;
		set_bit(&ictx->ictx_status, INODE_STATE_DIRTY);
	} else {
#else
	{
#endif
		assert(!ictx->ictx_data_dirty_count && !ictx->ictx_meta_dirty_count);
		type = BUFFER_TYPE_CLEAN;
		set_bit(&ictx->ictx_status, INODE_STATE_CLEAN);
	}

	ictx->ictx_bh = NULL;
	ictx->ictx_ref--;
	assert(ictx->ictx_ref >= 0);

	/* move ictx to type (clean or dirty) list */
	nvfuse_move_ictx_list(sb, ictx, type);

	/* mark unlock state */
	clear_bit(&ictx->ictx_status, INODE_STATE_LOCK);

	/* unlock spinlock */
	SPINLOCK_UNLOCK(&ictx->ictx_lock);
}
				
/* initialization of inode context cache manager */
int nvfuse_init_ictx_cache(struct nvfuse_superblock *sb)
{
	struct nvfuse_ictx_manager *ictxc;
	s32 i;

	ictxc = (struct nvfuse_ictx_manager *)spdk_dma_malloc(sizeof(struct nvfuse_ictx_manager), 0, NULL);
	if (ictxc == NULL) {
		dprintf_error(BUFFER, " %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
		return -1;
	}
	memset(ictxc, 0x00, sizeof(struct nvfuse_ictx_manager));
	sb->sb_ictxc = ictxc;

	SPINLOCK_INIT(&ictxc->ictxc_lock);

	for (i = BUFFER_TYPE_UNUSED; i < BUFFER_TYPE_NUM; i++) {
		INIT_LIST_HEAD(&ictxc->ictxc_list[i]);
		ictxc->ictxc_list_count[i] = 0;
	}

	for (i = 0; i < HASH_NUM + 1; i++) {
		INIT_HLIST_HEAD(&ictxc->ictxc_hash[i]);
		ictxc->ictxc_hash_count[i] = 0;
	}

	ictxc->ictx_buf = spdk_dma_malloc(sizeof(struct nvfuse_inode_ctx) * NVFUSE_ICTXC_SIZE, 0, NULL);

	dprintf_info(BUFFER, " ictx cache size = %d \n", (int)sizeof(struct nvfuse_inode_ctx) * NVFUSE_ICTXC_SIZE);

	/* alloc unsed list buffer cache */
	for (i = 0; i < NVFUSE_ICTXC_SIZE; i++) {
		struct nvfuse_inode_ctx *ictx;

		ictx = ((struct nvfuse_inode_ctx *)ictxc->ictx_buf) + i;

		list_add(&ictx->ictx_cache_list, &ictxc->ictxc_list[BUFFER_TYPE_UNUSED]);
		hlist_add_head(&ictx->ictx_hash, &ictxc->ictxc_hash[HASH_NUM]);
		ictxc->ictxc_hash_count[HASH_NUM]++;
		ictxc->ictxc_list_count[BUFFER_TYPE_UNUSED]++;
	}

	return 0;
}

s8 *nvfuse_decode_ictx_status(struct nvfuse_inode_ctx *ictx)
{
	static s8 str[128];
	s8 *ptr = str;

	if(ictx->ictx_status & (1 << INODE_STATE_NEW))
		ptr += sprintf(ptr, "NEW ");
	if(ictx->ictx_status & (1 << INODE_STATE_CLEAN))
		ptr += sprintf(ptr, "CLEAN ");
	if(ictx->ictx_status & (1 << INODE_STATE_DIRTY))
		ptr += sprintf(ptr, "DIRTY ");
	if(ictx->ictx_status & (1 << INODE_STATE_SYNC))
		ptr += sprintf(ptr, "SYNC ");
	if(ictx->ictx_status & (1 << INODE_STATE_LOCK))
		ptr += sprintf(ptr, "LOCK ");

	return str;
}

void nvfuse_print_ictx(struct nvfuse_inode_ctx *ictx)
{
	struct nvfuse_inode *inode;

	inode = ictx->ictx_inode;
	dprintf_debug(INODE, " ino: ino = %d type = %d status = %s(0x%x) dirty bh = %d\n", inode->i_ino,
			ictx->ictx_type, nvfuse_decode_ictx_status(ictx), ictx->ictx_status, nvfuse_inode_has_dirty(ictx));

}

void nvfuse_print_ictx_dirty_bhs(struct nvfuse_inode_ctx *ictx)
{
	struct list_head *dirty_head;
	struct nvfuse_buffer_head *bh;
	s32 data_count = 0;
	s32 meta_count = 0;

	/* ictx doesn't keep dirty data */
	if (!ictx->ictx_data_dirty_count &&
	    !ictx->ictx_meta_dirty_count)
		return;

	/* dirty list for file data */
	dirty_head = &ictx->ictx_data_bh_head;
	list_for_each_entry(bh, dirty_head, bh_dirty_list) {
		assert(test_bit(&bh->bh_status, BUFFER_STATUS_DIRTY));

		nvfuse_print_bh(bh);
		data_count++;
	}

	/* dirty list for meta data */
	dirty_head = &ictx->ictx_meta_bh_head;
	list_for_each_entry(bh, dirty_head, bh_dirty_list) {
		assert(test_bit(&bh->bh_status, BUFFER_STATUS_DIRTY));

		nvfuse_print_bh(bh);
		meta_count++;
	}

	assert(data_count == ictx->ictx_data_dirty_count);
	assert(meta_count == ictx->ictx_meta_dirty_count);
}

void nvfuse_print_ictx_list(struct nvfuse_superblock *sb, s32 type)
{
	struct list_head *head;
	struct nvfuse_inode_ctx *ictx;

	head = &sb->sb_ictxc->ictxc_list[type];

	dprintf_debug(INODE, " print ictx list type (%s)\n", buffer_type_to_str(type));
	list_for_each_entry(ictx, head, ictx_cache_list) {
		nvfuse_print_ictx(ictx);
		nvfuse_print_ictx_dirty_bhs(ictx);
	}
}

void nvfuse_print_ictx_list_count(struct nvfuse_superblock *sb, s32 type)
{
	dprintf_debug(INODE, " inode dirty count = %d \n", sb->sb_ictxc->ictxc_list_count[type]);
}

/* uninitialization of inode context cache manager */
void nvfuse_deinit_ictx_cache(struct nvfuse_superblock *sb)
{
	struct list_head *head;
	struct list_head *ptr, *temp;
	struct nvfuse_inode_ctx *ictx;
	s32 type;
	s32 removed_count = 0;

	/* dealloc buffer cache */
	for (type = BUFFER_TYPE_UNUSED; type < BUFFER_TYPE_NUM; type++) {
		head = &sb->sb_ictxc->ictxc_list[type];
		list_for_each_safe(ptr, temp, head) {
			ictx = (struct nvfuse_inode_ctx *)list_entry(ptr, struct nvfuse_inode_ctx, ictx_cache_list);
			list_del(&ictx->ictx_cache_list);
			removed_count++;
		}
	}
	/* deallocate whole ictx buffer */
	spdk_dma_free(sb->sb_ictxc->ictx_buf);
	assert(removed_count == NVFUSE_ICTXC_SIZE);
	spdk_dma_free(sb->sb_ictxc);
}

