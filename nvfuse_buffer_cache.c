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

void nvfuse_move_buffer_list_nolock(struct nvfuse_superblock *sb, 
							struct nvfuse_buffer_cache *bc,
							 s32 desired_type, s32 tail)
{
	struct nvfuse_buffer_manager *bm = sb->sb_bm;

	if (bc->bc_list_type == desired_type)
		return;

	list_del(&bc->bc_list);
	rte_atomic32_dec(&bm->bm_list_count[bc->bc_list_type]);
	assert(bc->bc_list_type < BUFFER_TYPE_NUM);

	bc->bc_list_type = desired_type;

	if (tail)
		list_add_tail(&bc->bc_list, &bm->bm_list[bc->bc_list_type]);
	else
		list_add(&bc->bc_list, &bm->bm_list[bc->bc_list_type]);

	rte_atomic32_inc(&bm->bm_list_count[bc->bc_list_type]);
}

void nvfuse_move_buffer_list(struct nvfuse_superblock *sb, 
							struct nvfuse_buffer_cache *bc,
							 s32 desired_type, s32 tail)
{
	struct nvfuse_buffer_manager *bm = sb->sb_bm;

	SPINLOCK_LOCK(&bm->bm_lock);
	nvfuse_move_buffer_list_nolock(sb,bc, desired_type, tail);
	SPINLOCK_UNLOCK(&bm->bm_lock);

#if 0
	hlist_del(&bc->bc_hash);
	if (desired_type == BUFFER_TYPE_UNUSED) {
		hlist_add_head(&bc->bc_hash, &bm->bm_hash[HASH_NUM]);
		bm->bm_hash_count[HASH_NUM]++;
	} else {
		hlist_add_head(&bc->bc_hash, &bm->bm_hash[bc->bc_bno % HASH_NUM]);
		bm->bm_hash_count[bc->bc_bno % HASH_NUM]++;
	}
#endif
}

void nvfuse_init_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc)
{
	bc->bc_bno = 0;
	bc->bc_dirty = 0;
	bc->bc_ino = 0;
	bc->bc_lbno = 0;
	bc->bc_load = 0;
	bc->bc_pno = 0;

	/* init spinlock */
	SPINLOCK_INIT(&bc->bc_lock);

	/* init ref count */
	rte_atomic32_init(&bc->bc_ref);
	bc->bc_temp = 0;

	memset(bc->bc_buf, 0x00, CLUSTER_SIZE);
}

struct nvfuse_buffer_cache *nvfuse_replace_buffer_cache(struct nvfuse_superblock *sb, u64 key)
{

	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_cache *bc;
	struct list_head *remove_ptr;
	s32 type = 0;

	/* if buffers are insufficient, it sens buffer allocation mesg to control plane */
	if (rte_atomic32_read(&bm->bm_list_count[BUFFER_TYPE_UNUSED]) == 0 && nvfuse_process_model_is_dataplane()) {
		s32 nr_buffers;

		/* try to allocate buffers from primary process */
		nr_buffers = NVFUSE_BUFFER_DEFAULT_ALLOC_SIZE_PER_MSG;
		nr_buffers = nvfuse_send_alloc_buffer_req(sb->sb_nvh, nr_buffers);
		if (nr_buffers > 0) {
			nvfuse_add_buffer_cache(sb, nr_buffers);
			assert(rte_atomic32_read(&bm->bm_list_count[BUFFER_TYPE_UNUSED]));
		}
	}

	if (rte_atomic32_read(&bm->bm_list_count[BUFFER_TYPE_UNUSED])) {
		type = BUFFER_TYPE_UNUSED;
	} else if (rte_atomic32_read(&bm->bm_list_count[BUFFER_TYPE_CLEAN])) {
		type = BUFFER_TYPE_CLEAN;
	} else {
		type = BUFFER_TYPE_DIRTY;
		dprintf_warn(BUFFER, " Warning: it runs out of clean buffers.\n");
		dprintf_warn(BUFFER, " Warning: it needs to flush dirty pages to disks.\n");
		nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	}

	assert(rte_atomic32_read(&bm->bm_list_count[type]));
	remove_ptr = (struct list_head *)(&bm->bm_list[type])->prev;
	do {
		bc = list_entry(remove_ptr, struct nvfuse_buffer_cache, bc_list);
		if (rte_atomic32_read(&bc->bc_ref) == 0 && rte_atomic32_read(&bc->bc_bh_count) == 0) {
			break;
		}
		remove_ptr = remove_ptr->prev;
		if (remove_ptr == &bm->bm_list[type]) {
			dprintf_warn(BUFFER, " no more buffer head.");
			while (1) sleep(1);
		}
		assert(remove_ptr != &bm->bm_list[type]);
	} while (1);

	/* remove list */
	list_del(&bc->bc_list);
	/* remove hlist */
	hlist_del(&bc->bc_hash);

	rte_atomic32_dec(&bm->bm_list_count[type]);
	if (type == BUFFER_TYPE_UNUSED)
		rte_atomic32_dec(&bm->bm_hash_count[HASH_NUM]);
	else
		rte_atomic32_dec(&bm->bm_hash_count[bc->bc_bno % HASH_NUM]);

	return bc;
}

struct nvfuse_buffer_cache *nvfuse_hash_lookup(struct nvfuse_buffer_manager *bm, u64 key)
{
	struct hlist_node *node;
	struct hlist_head *head;
	struct nvfuse_buffer_cache *bh;

	head = &bm->bm_hash[key % HASH_NUM];
	hlist_for_each(node, head) {
		bh = hlist_entry(node, struct nvfuse_buffer_cache, bc_hash);
		if (bh->bc_bno == key)
			return bh;
	}

	return NULL;
}

struct nvfuse_buffer_cache *nvfuse_find_bc(struct nvfuse_superblock *sb, u64 key, lbno_t lblock)
{
	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_cache *bc;
	s32 status;

	SPINLOCK_LOCK(&bm->bm_lock);

	bm->bm_cache_ref++;
	bc = nvfuse_hash_lookup(sb->sb_bm, key);
	if (bc) {
		/* in case of cache hit */
		assert(bc->bc_lbno == lblock);

		// cache move to mru position
		list_del(&bc->bc_list);
		rte_atomic32_dec(&bm->bm_list_count[bc->bc_list_type]);

		list_add(&bc->bc_list, &bm->bm_list[bc->bc_list_type]);
		rte_atomic32_inc(&bm->bm_list_count[bc->bc_list_type]);
		bm->bm_cache_hit++;

		//printf(" hit count = %d, inode = %d, hit rate = %f \n", bc->bc_hit, bc->bc_ino,
		//(double)bm->bm_cache_hit/bm->bm_cache_ref);
	} else {
		bc = nvfuse_replace_buffer_cache(sb, key);
		if (bc) {
			/* init bc structure */
			nvfuse_init_bc(sb, bc);

			/* hash list insertion */
			hlist_add_head(&bc->bc_hash, &bm->bm_hash[key % HASH_NUM]);
			rte_atomic32_inc(&bm->bm_hash_count[key % HASH_NUM]);

			status = BUFFER_TYPE_REF;
			/* list insertion */
			list_add(&bc->bc_list, &bm->bm_list[status]);
			/* increase count of clean list */
			rte_atomic32_inc(&bm->bm_list_count[status]);

			/* initialize key and type values*/
			bc->bc_bno = key;
			bc->bc_list_type = status;

			/* bc is shared among bhs */
			INIT_LIST_HEAD(&bc->bc_bh_head);
			rte_atomic32_set(&bc->bc_bh_count, 0);
		}
	}

	/* this counter will be decremented when release_bc() is called */
	SPINLOCK_LOCK(&bc->bc_lock);
	nvfuse_inc_bc_ref(bc);

	/* FIXME: needed to be in ref list? */
	if (rte_atomic32_read(&bc->bc_ref) && bc->bc_list_type != BUFFER_TYPE_REF) {
		nvfuse_move_buffer_list_nolock(sb, bc, BUFFER_TYPE_REF, 0);
	}

	SPINLOCK_UNLOCK(&bm->bm_lock);

	return bc;
}

#ifdef DEBUG_BC_COUNT
static int bc_count; 
#endif

struct nvfuse_buffer_head *nvfuse_alloc_buffer_head(struct nvfuse_superblock *sb)
{
	struct nvfuse_buffer_head *bh;

	bh = spdk_mempool_get(sb->bh_mempool);
	if (!bh) {
		dprintf_error(BUFFER, " Error: spdk_mempool_get() \n");
		return NULL;
	}
	memset(bh, 0x00, sizeof(struct nvfuse_buffer_head));
	INIT_LIST_HEAD(&bh->bh_bc_list);
	INIT_LIST_HEAD(&bh->bh_dirty_list);

	rb_init_node(&bh->bh_dirty_rbnode);

	SPINLOCK_INIT(&bh->bh_lock);
#ifdef DEBUG_BC_COUNT
	bc_count++;
	dprintf_debug(BC, "bc count = %d \n", bc_count);
#endif

	return bh;
}

void nvfuse_free_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc)
{
	spdk_mempool_put(sb->bc_mempool, bc);
}

void nvfuse_free_buffer_head(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh)
{
	spdk_mempool_put(sb->bh_mempool, bh);
#ifdef DEBUG_BC_COUNT
	dprintf_debug(BC, "bc count = %d \n", bc_count);
	bc_count--;
#endif
}

void nvfuse_inc_bc_ref(struct nvfuse_buffer_cache *bc)
{
	rte_atomic32_inc(&bc->bc_ref);
	bc->bc_temp++;

	dprintf_debug(BUFFER, " ino = %d lbno = %d inc refcnt = %d %d\n", bc->bc_ino, bc->bc_lbno, rte_atomic32_read(&bc->bc_ref), bc->bc_temp);

	assert(rte_atomic32_read(&bc->bc_ref) == 1);
}

void nvfuse_dec_bc_ref(struct nvfuse_buffer_cache *bc)
{
	rte_atomic32_dec(&bc->bc_ref);
	bc->bc_temp--;

	dprintf_debug(BUFFER, " ino = %d lbno = %d dec refcnt = %d %d\n", bc->bc_ino, bc->bc_lbno, rte_atomic32_read(&bc->bc_ref), bc->bc_temp);

	assert(rte_atomic32_read(&bc->bc_ref) >= 0);
}

struct nvfuse_buffer_cache *nvfuse_get_bc(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lblock, s32 sync_read)
{
	struct nvfuse_buffer_cache *bc;
	u64 key;

	nvfuse_make_pbno_key(ino, lblock, &key, NVFUSE_BP_TYPE_DATA);
	bc = nvfuse_find_bc(sb, key, lblock);
	if (bc == NULL) {
		return NULL;
	}

	if (!bc->bc_pno) {
		pbno_t new_pno;
		/* logical to physical address translation */
		new_pno = nvfuse_get_pbn(sb, ictx, ino, lblock);
		assert(new_pno);
		bc->bc_pno = new_pno;
	}

	if (bc->bc_pno) {
		if (sync_read && !bc->bc_load) {
			if (nvfuse_read_block(bc->bc_buf, bc->bc_pno, sb->target)) {
				/* FIXME: how can we handle this case? */
				dprintf_error(BUFFER, " Error: block read in %s\n", __FUNCTION__);
				/* necesary to release bc and bh here */
				assert(0);
			} else {
				bc->bc_load = 1;
			}
		}
	} else if (!bc->bc_pno && sync_read && !bc->bc_load) {
		/* FIXME: how can we handle this case? */
		dprintf_error(BUFFER, " Error: bc has no pblock addr \n");
		bc->bc_pno = nvfuse_get_pbn(sb, ictx, ino, lblock);
		assert(0);
	}

	assert(bc->bc_ino == ino);
	assert(bc->bc_lbno == lblock);
	assert(rte_atomic32_read(&bc->bc_ref) >= 1);
	assert(bc->bc_pno);

	return bc;
}

struct nvfuse_buffer_head *_nvfuse_get_bh(struct nvfuse_superblock *sb,
		struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lblock, s32 sync_read, s32 is_meta)
{
	struct nvfuse_buffer_head *bh;
	struct nvfuse_buffer_cache *bc;

	/* bh is protected by its inode lock */
	bh = nvfuse_find_bh_in_ictx(sb, ictx, ino, lblock);
	if (bh) {
#ifndef NVFUSE_KEEP_DIRTY_BH_IN_ICTX
		dprintf_error(BH, " BH cannot be cached in ictx!\n");
		assert(0);
#endif
		bc = bh->bh_bc;
		assert(bh->bh_buf == bc->bc_buf);
		goto FOUND_BH;
	} else {
		dprintf_debug(BC, " alloc ino = %d lblock = %d\n", ino, lblock);
		bh = nvfuse_alloc_buffer_head(sb);
		if (!bh)
			return NULL;
	}

	bc = nvfuse_get_bc(sb, ictx, ino, lblock, sync_read);
	if (!bc) {
		dprintf_error(BUFFER, " cannot get bc ino %d lblock = %d \n", ino, lblock);
		return NULL;
	}

	bh->bh_bc = bc;
	bh->bh_buf = bc->bc_buf;
	bh->bh_ictx = ictx ? ictx : NULL;

FOUND_BH:

	if (is_meta)
		nvfuse_set_bh_status(bh, BUFFER_STATUS_META);
	else
		nvfuse_clear_bh_status(bh, BUFFER_STATUS_META);

//	/* this counter will be decremented when release_bh() is called */
//	nvfuse_inc_bc_ref(bc);

	assert(bh->bh_bc);

#if 0
#ifdef NVFUSE_KEEP_DIRTY_BH_IN_ICTX
	/* if bc is found in ictx, bc is move to ref list */
	if (rte_atomic32_read(&bc->bc_ref) && bc->bc_list_type != BUFFER_TYPE_REF) {
		nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_REF, 0);
	}
#endif
#endif

	return bh;
}

struct nvfuse_buffer_head *nvfuse_get_bh(struct nvfuse_superblock *sb,
		struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lblock, s32 sync_read, s32 is_meta)
{
	struct nvfuse_buffer_head *bh;

	bh = _nvfuse_get_bh(sb, ictx, ino, lblock, sync_read, is_meta); 

	return bh;
}

struct nvfuse_buffer_head *nvfuse_get_new_bh(struct nvfuse_superblock *sb,
		struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lblock, s32 is_meta)
{
	struct nvfuse_buffer_cache *bc;
	struct nvfuse_buffer_head *bh;

	/* unnecessary to read data from disk due to new data */
	bh = _nvfuse_get_bh(sb, ictx, ino, lblock, 0 /* sync read */, is_meta);
	if (bh == NULL)
		return bh;

	bc = bh->bh_bc;

	if (bc->bc_pno) {
		memset(bc->bc_buf, 0x00, CLUSTER_SIZE);
	}

	/* this buffer will be update and then written out to disk later */
	nvfuse_mark_dirty_bh(sb, bh);

	return bh;
}

struct nvfuse_buffer_cache *nvfuse_alloc_bc(struct nvfuse_superblock *sb)
{
	struct nvfuse_buffer_cache *bc;

	bc = (struct nvfuse_buffer_cache *)spdk_mempool_get(sb->bc_mempool);
	if (bc == NULL) {
		dprintf_error(BUFFER, " %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
		return bc;
	}
	memset(bc, 0x00, sizeof(struct nvfuse_buffer_cache));

	SPINLOCK_INIT(&bc->bc_lock);

	return bc;
}

void nvfuse_move_bc_to_unused_list(struct nvfuse_superblock *sb, u64 key) {
	struct nvfuse_buffer_cache *bc;

	SPINLOCK_LOCK(&sb->sb_bm->bm_lock);
	bc = (struct nvfuse_buffer_cache *)nvfuse_hash_lookup(sb->sb_bm, key);
	if (bc) {
		SPINLOCK_LOCK(&bc->bc_lock);

		nvfuse_remove_bhs_in_bc(sb, bc);
		/* FIXME: reinitialization is necessary */
		bc->bc_load = 0;
		bc->bc_pno = 0;
		bc->bc_dirty = 0;
		rte_atomic32_init(&bc->bc_ref);

		SPINLOCK_UNLOCK(&bc->bc_lock);

		nvfuse_move_buffer_list_nolock(sb, bc, BUFFER_TYPE_UNUSED, INSERT_HEAD);
	}
	SPINLOCK_UNLOCK(&sb->sb_bm->bm_lock);
}

s32 nvfuse_remove_buffer_cache(struct nvfuse_superblock *sb, s32 nr_buffers)
{
	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_cache *bc;
	struct list_head *head;
	struct list_head *ptr, *temp;

	assert(nr_buffers > 0);

	if (bm->bm_cache_size - nr_buffers < NVFUSE_INITIAL_BUFFER_SIZE_DATA) {
		dprintf_warn(BUFFER, " Warninig: current buffer size = %.3f \n", (double)bm->bm_cache_size / 256);
		return -1;
	}

	if (nr_buffers > rte_atomic32_read(&bm->bm_list_count[BUFFER_TYPE_UNUSED])) {
		dprintf_warn(BUFFER, " Warninig: current unused buffer size = %.3f \n",
		       (double)rte_atomic32_read(&bm->bm_list_count[BUFFER_TYPE_UNUSED]) / 256);
		return -1;
	}

	//printf(" remove buffer cache (%d 4K pages) to process\n", nr);
	
	SPINLOCK_LOCK(&bm->bm_lock);

	head = &sb->sb_bm->bm_list[BUFFER_TYPE_UNUSED];
	list_for_each_safe(ptr, temp, head) {
		bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);

		SPINLOCK_LOCK(&bc->bc_lock);

		if (rte_atomic32_read(&bc->bc_bh_count)) {
			dprintf_error(BUFFER, " removing bhs in bc is not considered.\n");
			assert(0);
			nvfuse_remove_bhs_in_bc(sb, bc);
		}

		assert(!rte_atomic32_read(&bc->bc_bh_count));
		assert(!bc->bc_dirty);
		list_del(&bc->bc_list);
		hlist_del(&bc->bc_hash);

		SPINLOCK_UNLOCK(&bc->bc_lock);

		nvfuse_free_aligned_buffer(bc->bc_buf);
		nvfuse_free_bc(sb, bc);

		rte_atomic32_dec(&bm->bm_hash_count[HASH_NUM]);
		rte_atomic32_dec(&bm->bm_list_count[BUFFER_TYPE_UNUSED]);
		bm->bm_cache_size--;

		if (--nr_buffers == 0)
			break;
	}
	SPINLOCK_UNLOCK(&bm->bm_lock);

	return 0;
}


int nvfuse_add_buffer_cache(struct nvfuse_superblock *sb, int nr)
{
	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_cache *bc;

	assert(nr > 0);

	if (nvfuse_process_model_is_dataplane() &&
	    bm->bm_cache_size / 256 >= NVFUSE_MAX_BUFFER_SIZE_DATA) {
		dprintf_warn(BUFFER, " Current buffer size = %.3f \n", (double)bm->bm_cache_size / 256);
		return -1;
	}

	//printf(" Add buffer cache (%d 4K pages) to process\n", nr);

	while (nr--) {
		bc = nvfuse_alloc_bc(sb);
		if (!bc) {
			return -1;
		}

		bc->bc_sb = sb;
		bc->bc_buf = (s8 *)nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
		if (bc->bc_buf == NULL) {
			dprintf_error(BUFFER, " %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
#ifdef SPDK_ENABLED
			dprintf_warn(BUFFER, " Please, increase # of huge pages in scripts/setup.sh\n");
#endif
			return -1;
		}

		memset(bc->bc_buf, 0x00, CLUSTER_SIZE);

		/* bm already is locked. */
		//SPINLOCK_LOCK(&bm->bm_lock);

		list_add(&bc->bc_list, &bm->bm_list[BUFFER_TYPE_UNUSED]);
		hlist_add_head(&bc->bc_hash, &bm->bm_hash[HASH_NUM]);
		rte_atomic32_inc(&bm->bm_hash_count[HASH_NUM]);
		rte_atomic32_inc(&bm->bm_list_count[BUFFER_TYPE_UNUSED]);
		bm->bm_cache_size++;

		//SPINLOCK_UNLOCK(&bm->bm_lock);
	}

#if 0
	dprintf_info(BUFFER, " Buffer Size = %.3f MB\n", (double)bm->bm_cache_size / 256);
	dprintf_info(BUFFER, " buffer Unused = %.3f MB\n", (double)bm->bm_list_count[BUFFER_TYPE_UNUSED] / 256);
#endif

	return 0;
}

/* buffe_size in MB units */
int nvfuse_init_buffer_cache(struct nvfuse_superblock *sb, s32 buffer_size)
{
	struct nvfuse_buffer_manager *bm;
	s32 buffer_size_in_4k;
	s8 mempool_name[16];
	s32 mempool_size;
	s32 i;

	sprintf(mempool_name, "nvfuse_bh_%d", rte_lcore_id());

	if (!spdk_process_is_primary() || nvfuse_process_model_is_standalone())
		mempool_size = NVFUSE_BH_MEMPOOL_TOTAL_SIZE;
	else
		mempool_size = 2048; /* 8MB = 4K * 2048 */

	dprintf_info(BUFFER, " mempool for bh head size = %d \n",
	       (int)(mempool_size * sizeof(struct nvfuse_buffer_head)));

	sb->bh_mempool = spdk_mempool_create(mempool_name, mempool_size,
					     sizeof(struct nvfuse_buffer_head), NVFUSE_BH_MEMPOOL_CACHE_SIZE, -1);
	if (sb->bh_mempool == NULL) {
		dprintf_error(BUFFER, "allocation of bh mempool \n");
		exit(0);
	}

	sprintf(mempool_name, "nvfuse_bc");

	/* mempool is created by a primary process and shared to all processes. */
	if (spdk_process_is_primary() || nvfuse_process_model_is_standalone()) {
		mempool_size = NVFUSE_BC_MEMPOOL_TOTAL_SIZE;
		dprintf_info(BUFFER, " create mempool for bc head size = %d \n",
		       (int)(mempool_size * sizeof(struct nvfuse_buffer_cache)));
		sb->bc_mempool = (struct spdk_mempool *)spdk_mempool_create(mempool_name, mempool_size,
				 sizeof(struct nvfuse_buffer_cache), NVFUSE_BC_MEMPOOL_CACHE_SIZE, -1);
		if (sb->bc_mempool == NULL) {
			dprintf_error(BUFFER, "allocation of bc mempool \n");
			exit(0);
		}
	} else {
		dprintf_info(BUFFER, " use shared mempool for bc head size = %d \n",
		       (int)(mempool_size * sizeof(struct nvfuse_buffer_cache)));
		sb->bc_mempool = (struct spdk_mempool *)rte_mempool_lookup(mempool_name);
		if (sb->bc_mempool == NULL) {
			dprintf_error(BUFFER, " Error: allocation of bc mempool \n");
			exit(0);
		}
	}

	bm = (struct nvfuse_buffer_manager *)spdk_dma_malloc(sizeof(struct nvfuse_buffer_manager), 0, NULL);
	if (bm == NULL) {
		dprintf_error(BUFFER, " %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
		return -1;
	}
	memset(bm, 0x00, sizeof(struct nvfuse_buffer_manager));
	sb->sb_bm = bm;

	bm->bm_state = BM_STATE_UNINITIALIZED;

	for (i = BUFFER_TYPE_UNUSED; i < BUFFER_TYPE_NUM; i++) {
		INIT_LIST_HEAD(&bm->bm_list[i]);
		rte_atomic32_set(&bm->bm_list_count[i], 0);
	}

	for (i = 0; i < HASH_NUM + 1; i++) {
		INIT_HLIST_HEAD(&bm->bm_hash[i]);
		rte_atomic32_set(&bm->bm_hash_count[i], 0);
	}

	if (nvfuse_process_model_is_standalone()) {
		s32 recommended_size;

		/* 0.1% buffer of device size is recommended for better performance. */
		recommended_size = sb->sb_nvh->total_blkcount / SECTORS_PER_CLUSTER / 1000;

		if (buffer_size) {
			buffer_size_in_4k = buffer_size * (NVFUSE_MEGA_BYTES / CLUSTER_SIZE);
		} else {
			buffer_size_in_4k = recommended_size;
		}

		if (buffer_size_in_4k < recommended_size) {
			dprintf_info(BUFFER, " Performance will degrade due to small buffer size (%.3fMB)\n",
			       (double)buffer_size_in_4k * CLUSTER_SIZE / NVFUSE_MEGA_BYTES);
		} else {
			dprintf_info(BUFFER, " Buffer cache size = %.3f MB\n",
			       (double)buffer_size_in_4k * CLUSTER_SIZE / NVFUSE_MEGA_BYTES);
		}

	} else {
		buffer_size_in_4k = buffer_size * (NVFUSE_MEGA_BYTES / CLUSTER_SIZE);
	}
	dprintf_info(BUFFER, " Set Default Buffer Cache = %dMB\n", buffer_size_in_4k / 256);
	assert(buffer_size_in_4k);

	if (!spdk_process_is_primary()) {
		buffer_size_in_4k = nvfuse_send_alloc_buffer_req(sb->sb_nvh, buffer_size_in_4k);
		if (buffer_size_in_4k < 0)
			return -1;
	}

	/* alloc unsed list buffer cache */
	for (i = 0; i < buffer_size_in_4k; i++) {
		s32 res;

		res = nvfuse_add_buffer_cache(sb, 1);
		if (res < 0) {
			dprintf_error(BUFFER, " Error: buffer cannot be allocated. \n");
			break;
		}
	}

	SPINLOCK_INIT(&bm->bm_lock);
	bm->bm_state = BM_STATE_RUNNING;

	/* debug */
	//rte_malloc_dump_stats(stdout, NULL);

	return 0;
}

void nvfuse_deinit_buffer_cache(struct nvfuse_superblock *sb)
{
	struct list_head *head;
	struct list_head *ptr, *temp;
	struct nvfuse_buffer_cache *bc;
	s32 type;
	s32 removed_count = 0;

	/* dealloc buffer cache */
	for (type = BUFFER_TYPE_UNUSED; type < BUFFER_TYPE_NUM; type++) {
		head = &sb->sb_bm->bm_list[type];
		list_for_each_safe(ptr, temp, head) {
			bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);

			if (rte_atomic32_read(&bc->bc_bh_count)) {
				dprintf_error(BUFFER, " removing bhs in bc is not considered.\n");
				assert(0);
				nvfuse_remove_bhs_in_bc(sb, bc);
			}

			assert(!rte_atomic32_read(&bc->bc_bh_count));
			assert(!bc->bc_dirty);
			list_del(&bc->bc_list);
			nvfuse_free_aligned_buffer(bc->bc_buf);
			nvfuse_free_bc(sb, bc);
			removed_count++;
		}
	}

	assert(removed_count == sb->sb_bm->bm_cache_size);
	if (!spdk_process_is_primary() && nvfuse_process_model_is_dataplane()) {
		nvfuse_send_dealloc_buffer_req(sb->sb_nvh, removed_count);
	}

	spdk_mempool_free(sb->bh_mempool);

	if (spdk_process_is_primary()) {
		spdk_mempool_free(sb->bc_mempool);
	}
	dprintf_info(BUFFER, " > buffer cache hit rate = %f \n",
	       (double)sb->sb_bm->bm_cache_hit / sb->sb_bm->bm_cache_ref);

	sb->sb_bm->bm_state = BM_STATE_FINALIZED;

	spdk_dma_free(sb->sb_bm);
}

void nvfuse_mark_dirty_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh)
{
	struct nvfuse_buffer_cache *bc = bh->bh_bc;
	//int check = 0;

	assert(bh != NULL);

//	while (!SPINLOCK_IS_LOCKED(&bc->bc_lock)) {
//		SPINLOCK_LOCK(&bc->bc_lock);
//		check = 1;
//	}

	assert (SPINLOCK_IS_LOCKED(&bc->bc_lock));

	bc->bc_dirty = 1;

//	if (check)
//		SPINLOCK_UNLOCK(&bc->bc_lock);

	set_bit(&bh->bh_status, BUFFER_STATUS_DIRTY);
	if (bh->bh_ictx) {
		set_bit(&bh->bh_ictx->ictx_status, INODE_STATE_DIRTY);
	}
}

void nvfuse_set_bh_status(struct nvfuse_buffer_head *bh, s32 status)
{
	assert(status < BUFFER_STATUS_MAX);

	set_bit(&bh->bh_status, status);
}

void nvfuse_clear_bh_status(struct nvfuse_buffer_head *bh, s32 status)
{
	assert(status < BUFFER_STATUS_MAX);

	clear_bit(&bh->bh_status, status);
}

#ifdef USE_RBNODE
int nvfuse_rbnode_insert_bh(struct rb_root *root, struct nvfuse_buffer_head *bh)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct nvfuse_buffer_head *this = container_of(*new, struct nvfuse_buffer_head, bh_dirty_rbnode);
		s64 result = bh->bh_bc->bc_bno - this->bh_bc->bc_bno;

		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else
			return 0;
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&bh->bh_dirty_rbnode, parent, new);
	rb_insert_color(&bh->bh_dirty_rbnode, root);

	return 1;
}

struct nvfuse_buffer_head *nvfuse_rbnode_search(struct rb_root *root, u64 bno)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct nvfuse_buffer_head *bh = container_of(node, struct nvfuse_buffer_head, bh_dirty_rbnode);
		s64 result;

		result = (s64)bno - (s64)bh->bh_bc->bc_bno;

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return bh;
	}

	return NULL;
}
#endif

s32 nvfuse_insert_dirty_bh_to_ictx(struct nvfuse_buffer_head *bh, struct nvfuse_inode_ctx *ictx)
{
	/* check whether bh is involed in dirty list */
//#ifdef USE_RBNODE
	//if (!RB_EMPTY_NODE(&bh->bh_dirty_rbnode))
	//	return 0;
//#else
	if (!list_empty(&bh->bh_dirty_list))
		return 0;
//#endif

	if (test_bit(&bh->bh_status, BUFFER_STATUS_META)) {
		list_add(&bh->bh_dirty_list, &ictx->ictx_meta_bh_head);
#ifdef USE_RBNODE
		nvfuse_rbnode_insert_bh(&ictx->ictx_meta_bh_rbroot, bh);
#endif
		ictx->ictx_meta_dirty_count++;
	} else {
		assert(ictx->ictx_ino != ROOT_INO);
		list_add(&bh->bh_dirty_list, &ictx->ictx_data_bh_head);
#ifdef USE_RBNODE
		nvfuse_rbnode_insert_bh(&ictx->ictx_data_bh_rbroot, bh);
#endif
		ictx->ictx_data_dirty_count++;
	}

	assert(list_empty(&bh->bh_bc_list));

	if (list_empty(&bh->bh_bc_list)) {
		struct nvfuse_buffer_cache *bc = bh->bh_bc;

		list_add(&bh->bh_bc_list, &bc->bc_bh_head);
		rte_atomic32_inc(&bc->bc_bh_count);
		dprintf_debug(BH, " job_count ++ = %d, ino = %d lbno = %d \n", rte_atomic32_read(&bc->bc_bh_count), ictx->ictx_ino, bh->bh_bc->bc_lbno);
	} else {
		dprintf_warn(BUFFER, " warning:");
	}

	return 0;
}

#if 0
s32 nvfuse_forget_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh)
{
	struct nvfuse_buffer_cache *bc;

	bc = bh->bh_bc;
	nvfuse_dec_bc_ref(bc);
	nvfuse_remove_bhs_in_bc(sb, bc);

	bc->bc_dirty = 0;
	bc->bc_load = 0;

	if (rte_atomic32_read(&bc->bc_ref)) {
		dprintf_warn(BUFFER, "debug\n");
	}

	nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_UNUSED, INSERT_HEAD);

	nvfuse_free_buffer_head(sb, bh);

	return 0;
}
#endif

void nvfuse_release_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc, s32 tail, s32 dirty)
{
	nvfuse_dec_bc_ref(bc);

	dirty = dirty + bc->bc_dirty;

	if (dirty) {
		bc->bc_dirty = 1;
		bc->bc_load = 1;
		SPINLOCK_UNLOCK(&bc->bc_lock);

		nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_DIRTY, tail);
	} else {

		bc->bc_dirty = 0;
		SPINLOCK_UNLOCK(&bc->bc_lock);

		if (rte_atomic32_read(&bc->bc_ref) == 0)
			nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_CLEAN, tail);
		else {
			assert(0);
			nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_REF, tail);
		}
	}
}

void nvfuse_release_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh, s32 tail, s32 dirty)
{
	struct nvfuse_buffer_cache *bc;

	if (bh == NULL) {
		return ;
	}

	bc = bh->bh_bc;
	nvfuse_release_bc(sb, bc, tail, dirty);

	if (dirty)
		set_bit(&bh->bh_status, BUFFER_STATUS_DIRTY);

	/* insert dirty buffer head to inode context */
#ifdef NVFUSE_KEEP_DIRTY_BH_IN_ICTX
	if (bh->bh_ictx && test_bit(&bh->bh_status, BUFFER_STATUS_DIRTY))
		nvfuse_insert_dirty_bh_to_ictx(bh, bh->bh_ictx);
	else
#endif
	{
		assert(list_empty(&bh->bh_dirty_list));
 
		dprintf_debug(BC, " free ino = %d lblock = %d\n", bc->bc_ino, bc->bc_lbno);
		nvfuse_free_buffer_head(sb, bh);
	}
}

void nvfuse_remove_bhs_in_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc)
{
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_buffer_head *bh;
	struct list_head *head, *ptr, *temp;

	head = &bc->bc_bh_head;

	if (list_empty(head))
		return;

	list_for_each_safe(ptr, temp, head) {
		bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_bc_list);
		ictx = bh->bh_ictx;

		if (test_bit(&bh->bh_status, BUFFER_STATUS_META)) {
			ictx->ictx_meta_dirty_count--;
			assert(ictx->ictx_meta_dirty_count >= 0);
		} else {
			ictx->ictx_data_dirty_count--;
			assert(ictx->ictx_data_dirty_count >= 0);
		}

		list_del(&bh->bh_bc_list);
		list_del(&bh->bh_dirty_list);
		/*
		if (test_bit(&bh->bh_status, BUFFER_STATUS_META))
			printf(" remove dirty Meta: ino = %d lbno = %d count = %d pno = %d \n", ictx->ictx_ino, bh->bh_bc->bc_lbno, ictx->ictx_meta_dirty_count, bh->bh_bc->bc_pno);
		else
			printf(" remove dirty Data: ino = %d lbno = %d count = %d pno = %d \n", ictx->ictx_ino, bh->bh_bc->bc_lbno, ictx->ictx_meta_dirty_count, bh->bh_bc->bc_pno);
		*/
#ifdef USE_RBNODE
		if (test_bit(&bh->bh_status, BUFFER_STATUS_META))
			rb_erase(&bh->bh_dirty_rbnode, &ictx->ictx_meta_bh_rbroot);
		else
			rb_erase(&bh->bh_dirty_rbnode, &ictx->ictx_data_bh_rbroot);
#endif

		/* decrement count in buffer cache node */
		rte_atomic32_dec(&bc->bc_bh_count);
		dprintf_debug(BH, " job_count -- = %d, ino = %d lbno = %d \n", rte_atomic32_read(&bc->bc_bh_count), ictx->ictx_ino, bh->bh_bc->bc_lbno);

		/* FIXME: */
		if (ictx->ictx_bh == bh) {
			ictx->ictx_bh = NULL;
			nvfuse_dec_bc_ref(bc);
		}

		/* removal of buffer head */
		nvfuse_free_buffer_head(sb, bh);

		/* move inode context to clean list */
		if (nvfuse_inode_has_dirty(ictx) == 0) {
			assert(ictx->ictx_ino);
#ifdef USE_RBNODE
			assert(RB_EMPTY_ROOT(&ictx->ictx_data_bh_rbroot));
			assert(RB_EMPTY_ROOT(&ictx->ictx_meta_bh_rbroot));
#endif

			clear_bit(&ictx->ictx_status, BUFFER_STATUS_DIRTY);
			/* why assert is called here? */
			//assert(rte_spinlock_is_locked(&ictx->ictx_lock));
			nvfuse_move_ictx_list(sb, ictx, BUFFER_TYPE_CLEAN);
		}
	}

	assert(list_empty(head));
}


/* TODO: rbtree will be employed instead of simple linked list */
struct nvfuse_buffer_head *nvfuse_find_bh_in_ictx(struct nvfuse_superblock *sb,
		struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lbno)
{
	struct nvfuse_buffer_head *bh;
#ifndef USE_RBNODE
	struct nvfuse_buffer_cache *bc;
#endif
	u64 key;

	if (!ictx)
		return NULL;

	assert(rte_spinlock_is_locked(&ictx->ictx_lock));
	assert(test_bit(&ictx->ictx_status, INODE_STATE_LOCK));

	if (ictx->ictx_data_dirty_count == 0 &&
	    ictx->ictx_meta_dirty_count == 0)
		return NULL;

	/* dirty data list */	
#ifdef USE_RBNODE
	nvfuse_make_pbno_key(ino, lbno, &key, NVFUSE_BP_TYPE_DATA);
	bh = nvfuse_rbnode_search(&ictx->ictx_data_bh_rbroot, key);
	if (bh) {
		//printf(" found bh in ictx through rbtree \n");
		assert(bh->bh_bc->bc_ino == ino && bh->bh_bc->bc_lbno == lbno);
		return bh;
	}
#else
	dirty_head = &ictx->ictx_data_bh_head;
	list_for_each_safe(ptr, temp, dirty_head) {
		bh = list_entry(ptr, struct nvfuse_buffer_head, bh_dirty_list);
		bc = bh->bh_bc;

		if (bc->bc_ino == ino && bc->bc_lbno == lbno) {
			//printf(" found bh in ictx through list \n");
#ifdef USE_RBNODE
			assert(0);
#endif
			return bh;
		}
	}
#endif

	/* dirty meta list */	
#ifdef USE_RBNODE
	bh = nvfuse_rbnode_search(&ictx->ictx_meta_bh_rbroot, key);
	if (bh) {
		//printf(" found bh in ictx through rbtree \n");
		assert(bh->bh_bc->bc_ino == ino && bh->bh_bc->bc_lbno == lbno);
		return bh;
	}
#else
	dirty_head = &ictx->ictx_meta_bh_head;
	list_for_each_safe(ptr, temp, dirty_head) {
		bh = list_entry(ptr, struct nvfuse_buffer_head, bh_dirty_list);
		bc = bh->bh_bc;

		if (bc->bc_ino == ino && bc->bc_lbno == lbno) {
			//printf(" found bh in ictx through list \n");
#ifdef USE_RBNODE
			assert(0);
#endif
			return bh;
		}
	}
#endif

	return NULL;
}

#if 0
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
	sb->sb_bm->bm_list_count[BUFFER_TYPE_CLEAN]++;
}

__inline void dirty_count_dec(struct nvfuse_superblock *sb)
{
	sb->sb_bm->bm_list_count[BUFFER_TYPE_DIRTY]--;
}

__inline void dirty_count_inc(struct nvfuse_superblock *sb)
{
	sb->sb_bm->bm_list_count[BUFFER_TYPE_DIRTY]++;
}
#endif

__inline s32 nvfuse_get_dirty_count(struct nvfuse_superblock *sb)
{
	return rte_atomic32_read(&sb->sb_bm->bm_list_count[BUFFER_TYPE_DIRTY]);
}

void nvfuse_print_bh(struct nvfuse_buffer_head *bh)
{
	struct nvfuse_buffer_cache *bc = bh->bh_bc;

	dprintf_debug(INODE, " bh: ino = %d lbno = %d dirty = %d ref = %d\n", bc->bc_ino, bc->bc_lbno, bc->bc_dirty,
			rte_atomic32_read(&bc->bc_ref));
}
