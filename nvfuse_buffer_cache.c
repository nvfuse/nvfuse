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
#include "nvfuse_malloc.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_control_plane.h"
#include "list.h"
#include "rbtree.h"

void nvfuse_move_buffer_list(struct nvfuse_superblock *sb, 
							struct nvfuse_buffer_cache *bc,
							 s32 desired_type, s32 tail)
{
	struct nvfuse_buffer_manager *bm = sb->sb_bm;

	if (bc->bc_list_type == desired_type)
		return;

	list_del(&bc->bc_list);
	bm->bm_list_count[bc->bc_list_type]--;
	assert(bc->bc_list_type < BUFFER_TYPE_NUM);

	bc->bc_list_type = desired_type;

	if (tail)
		list_add_tail(&bc->bc_list, &bm->bm_list[bc->bc_list_type]);
	else
		list_add(&bc->bc_list, &bm->bm_list[bc->bc_list_type]);

	bm->bm_list_count[bc->bc_list_type]++;

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
	bc->bc_ref = 0;
	memset(bc->bc_buf, 0x00, CLUSTER_SIZE);
}

struct nvfuse_buffer_cache *nvfuse_replace_buffer_cache(struct nvfuse_superblock *sb, u64 key)
{

	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_cache *bc;
	struct list_head *remove_ptr;
	s32 type = 0;

	/* if buffers are insufficient, it sens buffer allocation mesg to control plane */
	if (bm->bm_list_count[BUFFER_TYPE_UNUSED] == 0 && nvfuse_process_model_is_dataplane()) {
		s32 nr_buffers;

		/* try to allocate buffers from primary process */
		nr_buffers = NVFUSE_BUFFER_DEFAULT_ALLOC_SIZE_PER_MSG;
		nr_buffers = nvfuse_send_alloc_buffer_req(sb->sb_nvh, nr_buffers);
		if (nr_buffers > 0) {
			nvfuse_add_buffer_cache(sb, nr_buffers);
			assert(bm->bm_list_count[BUFFER_TYPE_UNUSED]);
		}
	}

	if (bm->bm_list_count[BUFFER_TYPE_UNUSED]) {
		type = BUFFER_TYPE_UNUSED;
	} else if (bm->bm_list_count[BUFFER_TYPE_CLEAN]) {
		type = BUFFER_TYPE_CLEAN;
	} else {
		type = BUFFER_TYPE_DIRTY;
		printf(" Warning: it runs out of clean buffers.\n");
		printf(" Warning: it needs to flush dirty pages to disks.\n");
		nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	}

	assert(bm->bm_list_count[type]);
	remove_ptr = (struct list_head *)(&bm->bm_list[type])->prev;
	do {
		bc = list_entry(remove_ptr, struct nvfuse_buffer_cache, bc_list);
		if (bc->bc_ref == 0 && bc->bc_bh_count == 0) {
			break;
		}
		remove_ptr = remove_ptr->prev;
		if (remove_ptr == &bm->bm_list[type]) {
			printf(" no more buffer head.");
			while (1);
		}
		assert(remove_ptr != &bm->bm_list[type]);
	} while (1);

	/* remove list */
	list_del(&bc->bc_list);
	/* remove hlist */
	hlist_del(&bc->bc_hash);
	bm->bm_list_count[type]--;
	if (type == BUFFER_TYPE_UNUSED)
		bm->bm_hash_count[HASH_NUM]--;
	else
		bm->bm_hash_count[bc->bc_bno % HASH_NUM]--;
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

	bm->bm_cache_ref++;
	bc = nvfuse_hash_lookup(sb->sb_bm, key);
	if (bc) {
		/* in case of cache hit */
		bc->bc_lbno = lblock;

		// cache move to mru position
		list_del(&bc->bc_list);
		bm->bm_list_count[bc->bc_list_type]--;

		list_add(&bc->bc_list, &bm->bm_list[bc->bc_list_type]);
		bm->bm_list_count[bc->bc_list_type]++;
		bm->bm_cache_hit++;

		//printf(" hit count = %d, inode = %d, hit rate = %f \n", bc->bc_hit, bc->bc_ino,
		//(double)bm->bm_cache_hit/bm->bm_cache_ref);
	} else {
		bc = nvfuse_replace_buffer_cache(sb, key);
		if (bc) {
			nvfuse_init_bc(sb, bc);
			/* hash list insertion */
			hlist_add_head(&bc->bc_hash, &bm->bm_hash[key % HASH_NUM]);
			bm->bm_hash_count[key % HASH_NUM]++;

			status = BUFFER_TYPE_REF;
			/* list insertion */
			list_add(&bc->bc_list, &bm->bm_list[status]);
			/* increase count of clean list */
			bm->bm_list_count[status]++;
			/* initialize key and type values*/
			bc->bc_bno = key;
			bc->bc_list_type = status;
			INIT_LIST_HEAD(&bc->bc_bh_head);
			bc->bc_bh_count = 0;
		}
	}

	return bc;
}

struct nvfuse_buffer_head *nvfuse_alloc_buffer_head(struct nvfuse_superblock *sb)
{
	struct nvfuse_buffer_head *bh;

	bh = spdk_mempool_get(sb->bh_mempool);
	if (!bh) {
		printf(" Error: spdk_mempool_get() \n");
		return NULL;
	}
	memset(bh, 0x00, sizeof(struct nvfuse_buffer_head));
	INIT_LIST_HEAD(&bh->bh_bc_list);
	INIT_LIST_HEAD(&bh->bh_dirty_list);

	rb_init_node(&bh->bh_dirty_rbnode);

	return bh;
}

void nvfuse_free_bc(struct nvfuse_superblock *sb, struct nvfuse_buffer_cache *bc)
{
	spdk_mempool_put(sb->bc_mempool, bc);
}

void nvfuse_free_buffer_head(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh)
{
	spdk_mempool_put(sb->bh_mempool, bh);
}

struct nvfuse_buffer_head *nvfuse_get_bh(struct nvfuse_superblock *sb,
		struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lblock, s32 sync_read, s32 is_meta)
{
	struct nvfuse_buffer_head *bh;
	struct nvfuse_buffer_cache *bc;
	u64 key;

	bh = nvfuse_find_bh_in_ictx(sb, ictx, ino, lblock);
	if (bh) {
		bc = bh->bh_bc;
		assert(bh->bh_buf == bc->bc_buf);
		goto FOUND_BH;
	} else {
		bh = nvfuse_alloc_buffer_head(sb);
		if (!bh)
			return NULL;
	}

	nvfuse_make_pbno_key(ino, lblock, &key, NVFUSE_BP_TYPE_DATA);
	bc = nvfuse_find_bc(sb, key, lblock);
	if (bc == NULL) {
		return NULL;
	}

	if (!bc->bc_pno) {
		/* logical to physical address translation */
		bc->bc_pno = nvfuse_get_pbn(sb, ictx, ino, lblock);
		assert(bc->bc_pno);
	}

	if (bc->bc_pno) {
		if (sync_read && !bc->bc_load) {
			if (!nvfuse_read_block(bc->bc_buf, bc->bc_pno, sb->io_manager)) {
				/* FIXME: how can we handle this case? */
				printf(" Error: block read in %s\n", __FUNCTION__);
				/* necesary to release bc here */
				/* necesary to release bh here */
				return NULL;
			}
			bc->bc_load = 1;
		}
	} else if (!bc->bc_pno && sync_read && !bc->bc_load) {
		/* FIXME: how can we handle this case? */
		printf(" Error: bc has no pblock addr \n");
		bc->bc_pno = nvfuse_get_pbn(sb, ictx, ino, lblock);
		assert(0);
	}

	bh->bh_bc = bc;
	bh->bh_buf = bc->bc_buf;
	bh->bh_ictx = ictx ? ictx : NULL;

FOUND_BH:

	if (is_meta)
		nvfuse_set_bh_status(bh, BUFFER_STATUS_META);
	else
		nvfuse_clear_bh_status(bh, BUFFER_STATUS_META);

	bc->bc_ino = ino;
	bc->bc_lbno = lblock;
	bc->bc_ref++;
	assert(bc->bc_ref >= 0);
	assert(bc->bc_pno);
	assert(bh->bh_bc);

	if (bc->bc_ref && bc->bc_list_type != BUFFER_TYPE_REF) {
		nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_REF, 0);
	}
	return bh;
}

struct nvfuse_buffer_head *nvfuse_get_new_bh(struct nvfuse_superblock *sb,
		struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t lblock, s32 is_meta)
{
	struct nvfuse_buffer_cache *bc;
	struct nvfuse_buffer_head *bh;
	u64 key = 0;

	nvfuse_make_pbno_key(ino, lblock, &key, NVFUSE_BP_TYPE_DATA);
	bc = nvfuse_find_bc(sb, key, lblock);
	if (bc == NULL) {
		return NULL;
	}

	if (!bc->bc_pno) {
		/* logical to physical address translation */
		bc->bc_pno = nvfuse_get_pbn(sb, ictx, ino, lblock);
	}

	if (bc->bc_pno) {
		memset(bc->bc_buf, 0x00, CLUSTER_SIZE);
	}

	if (bc->bc_dirty)
		bc->bc_dirty = 1;

	bc->bc_load = 1;
	bc->bc_ino = ino;
	bc->bc_lbno = lblock;
	bc->bc_ref++;
	assert(bc->bc_ref == 1);

	bh = nvfuse_alloc_buffer_head(sb);
	if (!bh)
		return NULL;

	bh->bh_bc = bc;
	bh->bh_buf = bc->bc_buf;
	bh->bh_ictx = ictx ? ictx : NULL;

	if (is_meta)
		nvfuse_set_bh_status(bh, BUFFER_STATUS_META);
	else
		nvfuse_clear_bh_status(bh, BUFFER_STATUS_META);

	nvfuse_mark_dirty_bh(sb, bh);

	return bh;
}

struct nvfuse_buffer_cache *nvfuse_alloc_bc(struct nvfuse_superblock *sb)
{
	struct nvfuse_buffer_cache *bc;

	bc = (struct nvfuse_buffer_cache *)spdk_mempool_get(sb->bc_mempool);
	if (bc == NULL) {
		printf(" %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
		return bc;
	}
	memset(bc, 0x00, sizeof(struct nvfuse_buffer_cache));

	return bc;
}

s32 nvfuse_remove_buffer_cache(struct nvfuse_superblock *sb, s32 nr_buffers)
{
	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_cache *bc;
	struct list_head *head;
	struct list_head *ptr, *temp;

	assert(nr_buffers > 0);

	if (bm->bm_cache_size - nr_buffers < NVFUSE_INITIAL_BUFFER_SIZE_DATA) {
		printf(" Warninig: current buffer size = %.3f \n", (double)bm->bm_cache_size / 256);
		return -1;
	}

	if (nr_buffers > bm->bm_list_count[BUFFER_TYPE_UNUSED]) {
		printf(" Warninig: current unused buffer size = %.3f \n",
		       (double)bm->bm_list_count[BUFFER_TYPE_UNUSED] / 256);
		return -1;
	}

	//printf(" remove buffer cache (%d 4K pages) to process\n", nr);
	head = &sb->sb_bm->bm_list[BUFFER_TYPE_UNUSED];
	list_for_each_safe(ptr, temp, head) {
		bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);

		if (bc->bc_bh_count)
			nvfuse_remove_bhs_in_bc(sb, bc);

		assert(!bc->bc_bh_count);
		assert(!bc->bc_dirty);
		list_del(&bc->bc_list);
		hlist_del(&bc->bc_hash);

		nvfuse_free_aligned_buffer(bc->bc_buf);
		nvfuse_free_bc(sb, bc);

		bm->bm_hash_count[HASH_NUM]--;
		bm->bm_list_count[BUFFER_TYPE_UNUSED]--;
		bm->bm_cache_size--;

		if (--nr_buffers == 0)
			break;
	}

	return 0;
}


int nvfuse_add_buffer_cache(struct nvfuse_superblock *sb, int nr)
{
	struct nvfuse_buffer_manager *bm = sb->sb_bm;
	struct nvfuse_buffer_cache *bc;

	assert(nr > 0);

	if (nvfuse_process_model_is_dataplane() &&
	    bm->bm_cache_size / 256 >= NVFUSE_MAX_BUFFER_SIZE_DATA) {
		printf(" Current buffer size = %.3f \n", (double)bm->bm_cache_size / 256);
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
			printf(" %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
#ifdef SPDK_ENABLED
			printf(" Please, increase # of huge pages in scripts/setup.sh\n");
#endif
			return -1;
		}

		memset(bc->bc_buf, 0x00, CLUSTER_SIZE);

		list_add(&bc->bc_list, &bm->bm_list[BUFFER_TYPE_UNUSED]);
		hlist_add_head(&bc->bc_hash, &bm->bm_hash[HASH_NUM]);
		bm->bm_hash_count[HASH_NUM]++;
		bm->bm_list_count[BUFFER_TYPE_UNUSED]++;
		bm->bm_cache_size++;
	}

#if 0
	printf(" Buffer Size = %.3f MB\n", (double)bm->bm_cache_size / 256);
	printf(" buffer Unused = %.3f MB\n", (double)bm->bm_list_count[BUFFER_TYPE_UNUSED] / 256);
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

	printf(" mempool for bh head size = %d \n",
	       (int)(mempool_size * sizeof(struct nvfuse_buffer_head)));
	sb->bh_mempool = spdk_mempool_create(mempool_name, mempool_size,
					     sizeof(struct nvfuse_buffer_head), NVFUSE_BH_MEMPOOL_CACHE_SIZE, -1);
	if (sb->bh_mempool == NULL) {
		fprintf(stderr, " Error: allocation of bh mempool \n");
		exit(0);
	}

	sprintf(mempool_name, "nvfuse_bc");

	/* mempool is created by a primary process and shared to all processes. */
	if (spdk_process_is_primary() || nvfuse_process_model_is_standalone()) {
		mempool_size = NVFUSE_BC_MEMPOOL_TOTAL_SIZE;
		printf(" create mempool for bc head size = %d \n",
		       (int)(mempool_size * sizeof(struct nvfuse_buffer_cache)));
		sb->bc_mempool = (struct spdk_mempool *)spdk_mempool_create(mempool_name, mempool_size,
				 sizeof(struct nvfuse_buffer_cache), NVFUSE_BC_MEMPOOL_CACHE_SIZE, -1);
		if (sb->bc_mempool == NULL) {
			fprintf(stderr, " Error: allocation of bc mempool \n");
			exit(0);
		}
	} else {
		printf(" use shared mempool for bc head size = %d \n",
		       (int)(mempool_size * sizeof(struct nvfuse_buffer_cache)));
		sb->bc_mempool = (struct spdk_mempool *)rte_mempool_lookup(mempool_name);
		if (sb->bc_mempool == NULL) {
			fprintf(stderr, " Error: allocation of bc mempool \n");
			exit(0);
		}
	}

	bm = (struct nvfuse_buffer_manager *)spdk_malloc(sizeof(struct nvfuse_buffer_manager), 0, NULL);
	if (bm == NULL) {
		printf(" %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
		return -1;
	}
	memset(bm, 0x00, sizeof(struct nvfuse_buffer_manager));
	sb->sb_bm = bm;

	for (i = BUFFER_TYPE_UNUSED; i < BUFFER_TYPE_NUM; i++) {
		INIT_LIST_HEAD(&bm->bm_list[i]);
		bm->bm_list_count[i] = 0;
	}

	for (i = 0; i < HASH_NUM + 1; i++) {
		INIT_HLIST_HEAD(&bm->bm_hash[i]);
		bm->bm_hash_count[i] = 0;
	}

	if (nvfuse_process_model_is_standalone()) {
		s32 recommended_size;

		/* 0.1% buffer of device size is recommended for better performance. */
		recommended_size = sb->io_manager->total_blkcount / SECTORS_PER_CLUSTER / 1000;

		if (buffer_size) {
			buffer_size_in_4k = buffer_size * (NVFUSE_MEGA_BYTES / CLUSTER_SIZE);
		} else {
			buffer_size_in_4k = recommended_size;
		}

		if (buffer_size_in_4k < recommended_size) {
			printf(" Performance will degrade due to small buffer size (%.3fMB)\n",
			       (double)buffer_size_in_4k * CLUSTER_SIZE / NVFUSE_MEGA_BYTES);
		} else {
			printf(" Buffer cache size = %.3f MB\n",
			       (double)buffer_size_in_4k * CLUSTER_SIZE / NVFUSE_MEGA_BYTES);
		}

	} else {
		buffer_size_in_4k = buffer_size * (NVFUSE_MEGA_BYTES / CLUSTER_SIZE);
	}
	printf(" Set Default Buffer Cache = %dMB\n", buffer_size_in_4k / 256);
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
			printf(" Error: buffer cannot be allocated. \n");
			break;
		}
	}

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

			if (bc->bc_bh_count)
				nvfuse_remove_bhs_in_bc(sb, bc);

			assert(!bc->bc_bh_count);
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
	printf(" > buffer cache hit rate = %f \n",
	       (double)sb->sb_bm->bm_cache_hit / sb->sb_bm->bm_cache_ref);

	spdk_free(sb->sb_bm);
}

void nvfuse_mark_dirty_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh)
{
	assert(bh != NULL);

	bh->bh_bc->bc_dirty = 1;
	set_bit(&bh->bh_status, BUFFER_STATUS_DIRTY);
	if (bh->bh_ictx) {
		clear_bit(&bh->bh_ictx->ictx_status, BUFFER_STATUS_CLEAN);
		set_bit(&bh->bh_ictx->ictx_status, BUFFER_STATUS_DIRTY);
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
		//printf(" insert dirty Meta: ino = %d lbno = %d count = %d pno = %d \n", ictx->ictx_ino, bh->bh_bc->bc_lbno, ictx->ictx_meta_dirty_count, bh->bh_bc->bc_pno);
	} else {
		assert(ictx->ictx_ino != ROOT_INO);
		list_add(&bh->bh_dirty_list, &ictx->ictx_data_bh_head);
#ifdef USE_RBNODE
		nvfuse_rbnode_insert_bh(&ictx->ictx_data_bh_rbroot, bh);
#endif
		ictx->ictx_data_dirty_count++;
		//printf(" insert dirty Data: ino = %d lbno = %d count = %d pno = %d \n", ictx->ictx_ino, bh->bh_bc->bc_lbno, ictx->ictx_meta_dirty_count, bh->bh_bc->bc_pno);
	}

	assert(list_empty(&bh->bh_bc_list));

	if (list_empty(&bh->bh_bc_list)) {
		struct nvfuse_buffer_cache *bc = bh->bh_bc;

		list_add(&bh->bh_bc_list, &bc->bc_bh_head);
		bc->bc_bh_count++;
	} else {
		printf(" warning:");
	}

	return 0;
}

s32 nvfuse_forget_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh)
{
	struct nvfuse_buffer_cache *bc;

	bc = bh->bh_bc;
	bc->bc_ref--;
	nvfuse_remove_bhs_in_bc(sb, bc);

	bc->bc_dirty = 0;
	bc->bc_load = 0;

	if (bc->bc_ref) {
		printf("debug\n");
	}

	nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_UNUSED, INSERT_HEAD);

	nvfuse_free_buffer_head(sb, bh);

	return 0;
}

s32 nvfuse_release_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh, s32 tail, s32 dirty)
{
	struct nvfuse_buffer_cache *bc;

	if (bh == NULL) {
		return 0;
	}

	bc = bh->bh_bc;

	bc->bc_ref--;
	assert(bc->bc_ref >= 0);

	if (dirty || bc->bc_dirty) {
		bc->bc_dirty = 1;
		bc->bc_load = 1;
		nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_DIRTY, tail);
	} else {
		bc->bc_dirty = 0;
		if (bc->bc_ref == 0)
			nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_CLEAN, tail);
		else
			nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_REF, tail);
	}

	if (dirty)
		set_bit(&bh->bh_status, BUFFER_STATUS_DIRTY);

	/* insert dirty buffer head to inode context */
	//if (bh->bh_ictx && (dirty || bc->bc_dirty))
	if (bh->bh_ictx && test_bit(&bh->bh_status, BUFFER_STATUS_DIRTY)) {
		nvfuse_insert_dirty_bh_to_ictx(bh, bh->bh_ictx);
	} else {
		nvfuse_free_buffer_head(sb, bh);
	}

	return 0;
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
		bc->bc_bh_count--;

		/* FIXME: */
		if (ictx->ictx_bh == bh) {
			ictx->ictx_bh = NULL;
			bc->bc_ref--;
		}

		/* removal of buffer head */
		nvfuse_free_buffer_head(sb, bh);

		/* move inode context to clean list */
		if (ictx->ictx_meta_dirty_count == 0 &&
		    ictx->ictx_data_dirty_count == 0) {
			assert(ictx->ictx_ino);
#ifdef USE_RBNODE
			assert(RB_EMPTY_ROOT(&ictx->ictx_data_bh_rbroot));
			assert(RB_EMPTY_ROOT(&ictx->ictx_meta_bh_rbroot));
#endif

			clear_bit(&ictx->ictx_status, BUFFER_STATUS_DIRTY);
			set_bit(&ictx->ictx_status, BUFFER_STATUS_CLEAN);
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

	if (ictx->ictx_data_dirty_count == 0 &&
	    ictx->ictx_meta_dirty_count == 0)
		return NULL;


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

__inline s32 nvfuse_get_dirty_count(struct nvfuse_superblock *sb)
{
	return sb->sb_bm->bm_list_count[BUFFER_TYPE_DIRTY];
}

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
	struct list_head *remove_ptr;
	s32 type = 0;

	if (ictxc->ictxc_list_count[BUFFER_TYPE_UNUSED]) {
		type = BUFFER_TYPE_UNUSED;
	} else if (ictxc->ictxc_list_count[BUFFER_TYPE_CLEAN]) {
		type = BUFFER_TYPE_CLEAN;
	} else {
		type = BUFFER_TYPE_DIRTY;
		printf(" Warning: it runs out of clean buffers.\n");
		printf(" Warning: it needs to flush dirty pages to disks.\n");
		nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	}

	assert(ictxc->ictxc_list_count[type]);
	remove_ptr = (struct list_head *)(&ictxc->ictxc_list[type])->prev;
	do {
		ictx = list_entry(remove_ptr, struct nvfuse_inode_ctx, ictx_cache_list);
		if (ictx->ictx_ref == 0 &&
		    ictx->ictx_data_dirty_count == 0 &&
		    ictx->ictx_meta_dirty_count == 0)
			break;

		remove_ptr = remove_ptr->prev;
		if (remove_ptr == &ictxc->ictxc_list[type]) {
			/* TODO: error handling */
			printf(" no more buffer head.");
			while (1);
		}
		assert(remove_ptr != &ictxc->ictxc_list[type]);
	} while (1);

	/* remove list */
	list_del(&ictx->ictx_cache_list);
	/* remove hlist */
	hlist_del(&ictx->ictx_hash);
	ictxc->ictxc_list_count[type]--;
	if (type == BUFFER_TYPE_UNUSED)
		ictxc->ictxc_hash_count[HASH_NUM]--;
	else
		ictxc->ictxc_hash_count[ictx->ictx_ino % HASH_NUM]--;
	return ictx;
}

struct nvfuse_inode_ctx *nvfuse_alloc_ictx(struct nvfuse_superblock *sb)
{
	struct nvfuse_inode_ctx *ictx;

	ictx = nvfuse_replace_ictx(sb);
	nvfuse_init_ictx(ictx);

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
	ictx->ictx_ino = ino;
	ictx->ictx_type = type;

	if (type == BUFFER_TYPE_CLEAN && (ictx->ictx_data_dirty_count || ictx->ictx_meta_dirty_count)) {
		printf("debug");
		assert(0);
	}
}

void nvfuse_init_ictx(struct nvfuse_inode_ctx *ictx)
{
	ictx->ictx_ino = 0;
	ictx->ictx_inode = NULL;
	ictx->ictx_bh = NULL;

	INIT_LIST_HEAD(&ictx->ictx_meta_bh_head);
	INIT_LIST_HEAD(&ictx->ictx_data_bh_head);
#ifdef USE_RBNODE
	ictx->ictx_meta_bh_rbroot = RB_ROOT;
	ictx->ictx_data_bh_rbroot = RB_ROOT;
#endif

	ictx->ictx_meta_dirty_count = 0;
	ictx->ictx_data_dirty_count = 0;

	ictx->ictx_type = 0;
	ictx->ictx_status = 0;
	ictx->ictx_ref = 0;
}


struct nvfuse_inode_ctx *nvfuse_get_ictx(struct nvfuse_superblock *sb, inode_t ino)
{
	struct nvfuse_ictx_manager *ictxc = sb->sb_ictxc;
	struct nvfuse_inode_ctx *ictx;

	ictx = nvfuse_ictx_hash_lookup(sb->sb_ictxc, ino);
	if (ictx) { /* in case of cache hit */
		/* cache move to mru position */
		list_del(&ictx->ictx_cache_list);
		list_add(&ictx->ictx_cache_list, &ictxc->ictxc_list[ictx->ictx_type]);
		if (ictx->ictx_type == BUFFER_TYPE_CLEAN && (ictx->ictx_data_dirty_count ||
				ictx->ictx_meta_dirty_count)) {
			printf("debug");
			assert(0);
		}
	} else { /* in case of cache misses */
		ictx = nvfuse_replace_ictx(sb);
		if (ictx) {
			/* init ictx structure */
			INIT_LIST_HEAD(&ictx->ictx_data_bh_head);
			INIT_LIST_HEAD(&ictx->ictx_meta_bh_head);

#ifdef USE_RBNODE
			ictx->ictx_meta_bh_rbroot = RB_ROOT;
			ictx->ictx_data_bh_rbroot = RB_ROOT;
#endif

			ictx->ictx_meta_dirty_count = 0;
			ictx->ictx_data_dirty_count = 0;
			ictx->ictx_ino = ino;
			ictx->ictx_status = 0;
			ictx->ictx_ref = 0;
			nvfuse_insert_ictx(sb, ictx);
		}
	}

	return ictx;
}

void nvfuse_move_ictx_list(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
			   s32 desired_type)
{
	struct nvfuse_ictx_manager *ictxc = sb->sb_ictxc;

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
}

s32 nvfuse_release_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 dirty)
{
	if (ictx == NULL)
		return 0;

	ictx->ictx_ref--;

	assert(ictx->ictx_ref >= 0);
	/* dirty means that a given inode has dirty pages to be written to underlying storage */
	if (dirty || test_bit(&ictx->ictx_status, BUFFER_STATUS_DIRTY)) {
		if (!ictx->ictx_data_dirty_count && !ictx->ictx_meta_dirty_count) {
			printf(" Warning: ictx doesn't have dirty data. \n");
		}

		set_bit(&ictx->ictx_status, BUFFER_STATUS_DIRTY);
		nvfuse_move_ictx_list(sb, ictx, BUFFER_TYPE_DIRTY);
	} else {
		assert(!ictx->ictx_data_dirty_count && !ictx->ictx_meta_dirty_count);
		nvfuse_move_ictx_list(sb, ictx, BUFFER_TYPE_CLEAN);
	}

	return 0;
}
				
/* to be removed */

#if 0
s32 nvfuse_free_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx)
{
	if (ictx == NULL)
		return 0;

	ictx->ictx_ref--;
	assert(ictx->ictx_ref >= 0);
	assert(!ictx->ictx_data_dirty_count && !ictx->ictx_meta_dirty_count);
	nvfuse_move_ictx_list(sb, ictx, BUFFER_TYPE_UNUSED);

	return 0;
}
#endif

/* initialization of inode context cache manager */
int nvfuse_init_ictx_cache(struct nvfuse_superblock *sb)
{
	struct nvfuse_ictx_manager *ictxc;
	s32 i;

	ictxc = (struct nvfuse_ictx_manager *)spdk_malloc(sizeof(struct nvfuse_ictx_manager), 0, NULL);
	if (ictxc == NULL) {
		printf(" %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
		return -1;
	}
	memset(ictxc, 0x00, sizeof(struct nvfuse_ictx_manager));
	sb->sb_ictxc = ictxc;

	for (i = BUFFER_TYPE_UNUSED; i < BUFFER_TYPE_NUM; i++) {
		INIT_LIST_HEAD(&ictxc->ictxc_list[i]);
		ictxc->ictxc_list_count[i] = 0;
	}

	for (i = 0; i < HASH_NUM + 1; i++) {
		INIT_HLIST_HEAD(&ictxc->ictxc_hash[i]);
		ictxc->ictxc_hash_count[i] = 0;
	}

	ictxc->ictx_buf = spdk_malloc(sizeof(struct nvfuse_inode_ctx) * NVFUSE_ICTXC_SIZE, 0, NULL);

	printf(" ictx cache size = %d \n", (int)sizeof(struct nvfuse_inode_ctx) * NVFUSE_ICTXC_SIZE);

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
	spdk_free(sb->sb_ictxc->ictx_buf);
	assert(removed_count == NVFUSE_ICTXC_SIZE);
	spdk_free(sb->sb_ictxc);
}

