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

/*
*  Indirect Block Management Scheme ported from Ext2 File System of Linux Kerenl
*
*  linux/fs/ext2/inode.c
*
* Copyright (C) 1992, 1993, 1994, 1995
* Remy Card (card@masi.ibp.fr)
* Laboratoire MASI - Institut Blaise Pascal
* Universite Pierre et Marie Curie (Paris VI)
*
*  from
*
*  linux/fs/minix/inode.c
*
*  Copyright (C) 1991, 1992  Linus Torvalds
*
*  Goal-directed block allocation by Stephen Tweedie
* 	(sct@dcs.ed.ac.uk), 1993, 1998
*  Big-endian to little-endian byte-swapping/bitmaps by
*        David S. Miller (davem@caip.rutgers.edu), 1995
*  64-bit file support on 64-bit platforms by Jakub Jelinek
* 	(jj@sunsite.ms.mff.cuni.cz)
*
*  Assorted race fixes, rewrite of ext2_get_block() by Al Viro, 2000
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
//#define NDEBUG
#include <assert.h>
#include <errno.h>
#include "spdk/env.h"

#ifdef __linux__
#include <sys/uio.h>
#endif

#include "nvfuse_core.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_dirhash.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_indirect.h"

typedef struct {
	u32 *p;
	u32 key;
	struct nvfuse_buffer_head *bh;
} Indirect;

static inline void add_chain(Indirect *p, struct nvfuse_buffer_head *bh, u32 *v)
{
	p->key = *(p->p = v);
	p->bh = bh;
}

/**
*	ext2_block_to_path - parse the block number into array of offsets
*	@inode: inode in question (we are only interested in its superblock)
*	@i_block: block number to be parsed
*	@offsets: array to store the offsets in
*      @boundary: set this non-zero if the referred-to block is likely to be
*             followed (on disk) by an indirect block.
*	To store the locations of file's data ext2 uses a data structure common
*	for UNIX filesystems - tree of pointers anchored in the inode, with
*	data blocks at leaves and indirect blocks in intermediate nodes.
*	This function translates the block number into path in that tree -
*	return value is the path length and @offsets[n] is the offset of
*	pointer to (n+1)th node in the nth one. If @block is out of range
*	(negative or too large) warning is printed and zero returned.
*
*	Note: function doesn't find node addresses, so no IO is needed. All
*	we need to know is the capacity of indirect blocks (taken from the
*	inode->i_sb).
*/

/*
* Portability note: the last comparison (check that we fit into triple
* indirect block) is spelled differently, because otherwise on an
* architecture with 32-bit longs and 8Kb pages we might get into trouble
* if our filesystem had 8Kb blocks. We might use long long, but that would
* kill us on x86. Oh, well, at least the sign propagation does not matter -
* i_block would have to be negative in the very beginning, so we would not
* get there at all.
*/

int nvfuse_block_to_path(s32 block, u32 offsets[4], u32 *boundary)
{
	int ptrs = PTRS_PER_BLOCK;
	int ptrs_bits = PTRS_PER_BLOCK_BITS;
	const long direct_blocks = DIRECT_BLOCKS,
		   double_blocks = (1 << (ptrs_bits * 2));
	int n = 0;
	int final = 0;
	int org_block = block;

	if (block < 0) {
		printf(" Warning: block < 0(%d)\n", org_block);
	} else if (block < DIRECT_BLOCKS) {
		offsets[n++] = block;
		final = direct_blocks;
	} else if ((block -= DIRECT_BLOCKS) < PTRS_PER_BLOCK) {
		offsets[n++] = INDIRECT_BLOCKS;
		offsets[n++] = block;
		final = ptrs;
	} else if ((block -= PTRS_PER_BLOCK) < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
		offsets[n++] = DINDIRECT_BLOCKS;
		offsets[n++] = block >> ptrs_bits;
		offsets[n++] = block & (ptrs - 1);
		final = ptrs;
	} else if (((block -= double_blocks) >> (ptrs_bits * 2)) < ptrs) {
		offsets[n++] = TINDIRECT_BLOCKS;
		offsets[n++] = block >> (ptrs_bits * 2);
		offsets[n++] = (block >> ptrs_bits) & (ptrs - 1);
		offsets[n++] = block & (ptrs - 1);
		final = ptrs;
	} else {
		printf(" Warning: block is too big (%d)!\n", org_block);
	}

	if (boundary)
		*boundary = final - 1 - (block & (ptrs - 1));
	return n;
}

static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == *from->p)
		from++;
	return (from > to);
}

/**
*	ext2_get_branch - read the chain of indirect blocks leading to data
*	@inode: inode in question
*	@depth: depth of the chain (1 - direct pointer, etc.)
*	@offsets: offsets of pointers in inode/indirect blocks
*	@chain: place to store the result
*	@err: here we store the error value
*
*	Function fills the array of triples <key, p, bh> and returns %NULL
*	if everything went OK or the pointer to the last filled triple
*	(incomplete one) otherwise. Upon the return chain[i].key contains
*	the number of (i+1)-th block in the chain (as it is stored in memory,
*	i.e. little-endian 32-bit), chain[i].p contains the address of that
*	number (it points into struct inode for i==0 and into the bh->b_data
*	for i>0) and chain[i].bh points to the buffer_head of i-th indirect
*	block for i>0 and NULL for i==0. In other words, it holds the block
*	numbers of the chain, addresses they were taken from (and where we can
*	verify that chain did not change) and buffer_heads hosting these
*	numbers.
*
*	Function stops when it stumbles upon zero pointer (absent block)
*		(pointer to last triple returned, *@err == 0)
*	or when it gets an IO error reading an indirect block
*		(ditto, *@err == -EIO)
*	or when it notices that chain had been changed while it was reading
*		(ditto, *@err == -EAGAIN)
*	or when it reads all @depth-1 indirect blocks successfully and finds
*	the whole chain, all way to the data (returns %NULL, *err == 0).
*/
static Indirect *nvfuse_get_branch(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
				   struct nvfuse_inode *inode,
				   int depth,
				   int *offsets,
				   Indirect chain[4],
				   int *err)
{
	Indirect *p = chain;
	struct nvfuse_buffer_head *bh;

	*err = 0;
	/* i_data is not going away, no lock needed */
	add_chain(chain, NULL, inode->i_blocks + *offsets);
	if (!p->key)
		goto no_block;
	while (--depth) {
		bh = nvfuse_get_bh(sb, ictx, BLOCK_IO_INO, p->key, READ, NVFUSE_TYPE_META);
		if (!bh)
			goto failure;
		if (!verify_chain(chain, p))
			goto changed;
		add_chain(++p, bh, (u32 *)bh->bh_buf + *++offsets);
		if (!p->key)
			goto no_block;
	}
	return NULL;

changed:
	nvfuse_release_bh(sb, bh, 0, CLEAN);
	*err = -EAGAIN;
	goto no_block;
failure:
	*err = -EIO;
no_block:
	return p;
}

u32 nvfuse_alloc_free_block(struct nvfuse_superblock *sb, struct nvfuse_inode *inode,
			    u32 *alloc_blks, u32 num_blocks)
{
	s32 ret = 0;
	u32 bg_id;
	u32 next_id;
	u32 cnt = 0;
	u32 once = 1;

	//printf(" current free blocks = %ld \n", sb->asb.asb_free_blocks);

	if (nvfuse_process_model_is_dataplane() && !nvfuse_check_free_block(sb, num_blocks)) {
		s32 container_id;

		container_id = nvfuse_alloc_container_from_primary_process(sb->sb_nvh, CONTAINER_NEW_ALLOC);
		if (container_id > 0) {
			/* insert allocated container to process */
			nvfuse_add_bg(sb, container_id);
			assert(nvfuse_check_free_block(sb, num_blocks) == 1);
		} else {
			printf(" No free containers in the file system\n");
			assert(0);
		}
	}

	bg_id = inode->i_ino / sb->sb_no_of_inodes_per_bg;
	if (bg_id != sb->sb_last_allocated_bgid && inode->i_ino == sb->sb_last_allocated_bgid_by_ino) {
		bg_id = sb->sb_last_allocated_bgid;
	}

	next_id = bg_id;

	do {
		if (nvfuse_get_free_blocks(sb, bg_id)) {
			ret = nvfuse_alloc_dbitmap(sb, bg_id, alloc_blks + cnt, num_blocks);
			num_blocks -= ret;
			cnt += ret;

			/* retain hint information to rapidly find free blocks */
			sb->sb_last_allocated_bgid = bg_id;
			sb->sb_last_allocated_bgid_by_ino = inode->i_ino;

			if (!num_blocks) {
				break;
			}
		}

		//printf(" sid %d free blocks %d \n", bg_id, nvfuse_get_free_blocks(sb, bg_id));
		//printf("1. cur bg = %d %d\n", bg_id, nvfuse_get_curr_bg_id(sb, 0 /* data */));
#if 1
		if (once && nvfuse_process_model_is_dataplane()) {
			nvfuse_move_curr_bg_id(sb, bg_id, 0 /* data type */);
			once--;
		}
#endif
		//printf("2. cur bg = %d %d\n", bg_id, nvfuse_get_curr_bg_id(sb, 0 /* data */));
		if (nvfuse_process_model_is_dataplane())
			bg_id = nvfuse_get_next_bg_id(sb, 0 /* data type */);
		else
			bg_id = (bg_id + 1) % sb->sb_bg_num;
		//printf("3. alloc block: cur bg = %d, next_bg = %d \n", bg_id, next_id);
	} while (bg_id != next_id);

	/* FIXME: how to handle this exception case! */
	if (!cnt) {
		printf(" Warning: it runs out of free blocks.\n");
		nvfuse_print_bg_list(sb);
		assert(0);
	}

	return cnt;
}

void nvfuse_return_free_blocks(struct nvfuse_superblock *sb, u32 *blks, u32 num)
{
	u32 *end = blks + num;

	while (blks < end) {
		nvfuse_free_blocks(sb, *blks, 1);
		blks++;
	}
}

u32 nvfuse_alloc_free_blocks(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, u32 *blocks,
			     u32 num_indirect_blocks, u32 num_blocks, u32 *direct_map, s32 *error)
{
	u32 new_blocks[2] = { 0, 0 };
	u32 cnt = 0;
	u32 total_blocks;

	total_blocks = num_indirect_blocks + 1;

	if (total_blocks) {
		new_blocks[0] = nvfuse_alloc_free_block(sb, inode, blocks, total_blocks);
		if (new_blocks[0] != total_blocks) {
			printf(" Warning: it runs out of free blocks.\n");
			nvfuse_print_bg_list(sb);
			assert(0);
			if (error) {
				*error = -1;
				goto RELEASE_FREE;
			}
		}
		cnt += new_blocks[0];
	}

	if (*error)
		return 0;

	total_blocks =  num_blocks - 1;
	if (total_blocks) {
		new_blocks[1] = nvfuse_alloc_free_block(sb, inode, direct_map, total_blocks);
		if (new_blocks[1] != total_blocks) {
			printf(" Warning: it runs out of free blocks. (requested = %d, allocated = %d)\n",
			       total_blocks, new_blocks[1]);
			printf(" current free blocks = %ld \n", sb->asb.asb_free_blocks);
			nvfuse_print_bg_list(sb);
			assert(0);
			if (error) {
				*error = -1;
				goto RELEASE_FREE;
			}
		}
		cnt += new_blocks[1];
	}

	return cnt;

RELEASE_FREE:
	;

	if (new_blocks[1]) {
		nvfuse_return_free_blocks(sb, direct_map, new_blocks[1]);
	}

	if (new_blocks[0]) {
		nvfuse_return_free_blocks(sb, blocks, new_blocks[0]);
	}

	return 0;
}

/**
*	ext2_blks_to_allocate: Look up the block map and count the number
*	of direct blocks need to be allocated for the given branch.
*
* 	@branch: chain of indirect blocks
*	@k: number of blocks need for indirect blocks
*	@blks: number of data blocks to be mapped.
*	@blocks_to_boundary:  the offset in the indirect block
*
*	return the total number of blocks to be allocate, including the
*	direct and indirect blocks.
*/

static int
nvfuse_blks_to_allocate(Indirect *branch, int k, unsigned long blks,
			int blocks_to_boundary)
{
	unsigned long count = 0;

	/*
	* Simple case, [t,d]Indirect block(s) has not allocated yet
	* then it's clear blocks on that path have not allocated
	*/
	if (k > 0) {
		/* right now don't hanel cross boundary allocation */
		if (blks < blocks_to_boundary + 1)
			count += blks;
		else
			count += blocks_to_boundary + 1;
		return count;
	}

	count++;
	while (count < blks && count <= blocks_to_boundary
	       && (*(branch[0].p + count)) == 0) {
		count++;
	}
	return count;
}

/**
*	ext2_alloc_branch - allocate and set up a chain of blocks.
*	@inode: owner
*	@num: depth of the chain (number of blocks to allocate)
*	@offsets: offsets (in the blocks) to store the pointers to next.
*	@branch: place to store the chain in.
*
*	This function allocates @num blocks, zeroes out all but the last one,
*	links them into chain and (if we are synchronous) writes them to disk.
*	In other words, it prepares a branch that can be spliced onto the
*	inode. It stores the information about that chain in the branch[], in
*	the same format as ext2_get_branch() would do. We are calling it after
*	we had read the existing part of chain and partial points to the last
*	triple of that (one with zero ->key). Upon the exit we have the same
*	picture as after the successful ext2_get_block(), except that in one
*	place chain is disconnected - *branch->p is still zero (we did not
*	set the last link), but branch->key contains the number that should
*	be placed into *branch->p to fill that gap.
*
*	If allocation fails we free all blocks we've allocated (and forget
*	their buffer_heads) and return the error value the from failed
*	ext2_alloc_block() (normally -ENOSPC). Otherwise we set the chain
*	as described above and return 0.
*/

static int nvfuse_alloc_branch(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
			       struct nvfuse_inode *inode,
			       int indirect_blks, int *blks, u32 *direct_map,
			       int *offsets, Indirect *branch)
{
	int blocksize = CLUSTER_SIZE;
	int i, n = 0;
	int err = 0;
	struct nvfuse_buffer_head *bh;
	int num;
	u32 new_blocks[4] = { 0, };
	u32 current_block;

	num = nvfuse_alloc_free_blocks(sb, inode, new_blocks, indirect_blks, *blks, direct_map, &err);
	if (err) {
		return err;
	}

	if (indirect_blks)
		num -= indirect_blks;

	branch[0].key = new_blocks[0];
	/*
	* metadata blocks and data blocks are allocated.
	*/
	for (n = 1; n <= indirect_blks; n++) {
		/*
		* Get buffer_head for parent block, zero it out
		* and set the pointer to new one, then send
		* parent to disk.
		*/
		bh = nvfuse_get_bh(sb, ictx, BLOCK_IO_INO, new_blocks[n - 1], WRITE, NVFUSE_TYPE_META);
		if (!bh) {
			err = -ENOMEM;
			goto failed;
		}
		branch[n].bh = bh;
		//lock_buffer(bh);
		memset(bh->bh_buf, 0, blocksize);
		branch[n].p = (u32 *)bh->bh_buf + offsets[n];

		assert(!offsets[n]);

		branch[n].key = new_blocks[n];
		*branch[n].p = branch[n].key;
		if (n == indirect_blks) {
			current_block = new_blocks[n];
			/*
			* End of chain, update the last new metablock of
			* the chain to point to the new allocated
			* data blocks numbers
			*/
			*(branch[n].p + 0) = current_block;
			for (i = 1; i < num; i++) {
				*(branch[n].p + i) = *direct_map;
				direct_map++;
			}
		}

		nvfuse_mark_dirty_bh(sb, bh);
		//nvfuse_release_bh(sb, bh, 0, DIRTY);

		//set_buffer_uptodate(bh);
		//unlock_buffer(bh);
		//mark_buffer_dirty_inode(bh, inode);
		/* We used to sync bh here if IS_SYNC(inode).
		* But we now rely upon generic_write_sync()
		* and b_inode_buffers.  But not for directories.
		*/
		//if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
		//	sync_dirty_buffer(bh);
	}

	*blks = num;
	return err;

failed:
	printf(" need to check the following lines.");
	assert(0);
	for (i = 1; i < n; i++)
		nvfuse_release_bh(sb, branch[i].bh, 0, CLEAN);
	for (i = 0; i < indirect_blks; i++)
		nvfuse_free_blocks(sb, new_blocks[i], 1);
	nvfuse_free_blocks(sb, new_blocks[i], num);
	return err;
}

/**
* ext2_splice_branch - splice the allocated branch onto inode.
* @inode: owner
* @where: location of missing link
* @num:   number of indirect blocks we are adding
* @blks:  number of direct blocks we are adding
*
* This function fills the missing link and does all housekeeping needed in
* inode (->i_blocks, etc.). In case of success we end up with the full
* chain to new block and return 0.
*/
static void nvfuse_splice_branch(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
				 Indirect *where, u32 *direct_map, int num, int blks)
{
	/* XXX LOCKING probably should have i_meta_lock ?*/
	/* That's it */

	*where->p = where->key;

	/*
	* Update the host buffer_head or inode to point to more just allocated
	* direct blocks blocks
	*/
	if (num == 0 && blks > 1) {
		u32 i;
		for (i = 1; i < blks; i++) {
			*(where->p + i) = *direct_map;
			direct_map++;
		}
	}

	/* We are done with atomic stuff, now do the rest of housekeeping */

	/* had we spliced it onto indirect block? */
	if (where->bh) {
		/* need to be marked as dirty */
		nvfuse_mark_dirty_bh(sb, where->bh);
	}

	//inode->i_ctime = CURRENT_TIME_SEC;
	//mark_inode_dirty(inode);
	nvfuse_mark_inode_dirty(ictx);
}

/*
* Allocation strategy is simple: if we have to allocate something, we will
* have to go the whole way to leaf. So let's do it before attaching anything
* to tree, set linkage between the newborn blocks, write them if sync is
* required, recheck the path, free and repeat if check fails, otherwise
* set the last missing link (that will protect us from any truncate-generated
* removals - all blocks on the path are immune now) and possibly force the
* write on the parent block.
* That has a nice additional property: no special recovery from the failed
* allocations is needed - we simply release blocks and do not touch anything
* reachable from inode.
*
* `handle' can be NULL if create == 0.
*
* return > 0, # of blocks mapped or allocated.
* return = 0, if plain lookup failed.
* return < 0, error case.
*/
s32 nvfuse_get_block(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 lblock,
		     u32 maxblocks, u32 *num_alloc_blocks, u32 *pblock, u32 create)
{
	s32 offsets[INDIRECT_BLOCKS_LEVEL];
	Indirect chain[INDIRECT_BLOCKS_LEVEL];
	Indirect *partial;
	int err = 0;
	u32 depth;
	u32 indirect_blks;
	u32 first_block = 0;
	int count = 0;
	int blocks_to_boundary = 0;

	if (pblock)
		*pblock = 0;

	if (num_alloc_blocks)
		*num_alloc_blocks = 0;

	depth = nvfuse_block_to_path(lblock, (u32 *)offsets, (u32 *)&blocks_to_boundary);
	if (depth == 0)
		return -1;
	partial = nvfuse_get_branch(sb, ictx, ictx->ictx_inode, depth, (s32 *)offsets, chain, &err);
	if (!partial) {
		first_block = chain[depth - 1].key;
		//clear_buffer_new(bh_result); /* What's this do? */
		count++;
		/*map more blocks*/
		while (count < maxblocks && count <= blocks_to_boundary) {
			u32 blk;

			if (!verify_chain(chain, chain + depth - 1)) {
				/*
				* Indirect block might be removed by
				* truncate while we were reading it.
				* Handling of that case: forget what we've
				* got now, go to reread.
				*/
				err = -EAGAIN;
				count = 0;
				break;
			}
			blk = *(chain[depth - 1].p + count);
			if (blk == first_block + count)
				count++;
			else
				break;
		}
		if (err != -EAGAIN)
			goto got_it;

	}

	if (!create || err == -EIO) {
		goto cleanup;
	}

	/* the number of blocks need to allocate for [d,t]indirect blocks */
	indirect_blks = (chain + depth) - partial - 1;

	count = nvfuse_blks_to_allocate(partial, indirect_blks, maxblocks, blocks_to_boundary);
	{
		u32 *direct_map;

		direct_map = spdk_dma_zmalloc(sizeof(u32) * count, 0, NULL);
		assert(direct_map != NULL);

		err = nvfuse_alloc_branch(sb, ictx, ictx->ictx_inode, indirect_blks, &count, direct_map,
					  offsets + (partial - chain), partial);
		if (err) {
			goto cleanup;
		}

		/* attach indirect block to inode */
		nvfuse_splice_branch(sb, ictx, partial, direct_map, indirect_blks, count);

		spdk_dma_free(direct_map);
	}

got_it:
	if (num_alloc_blocks)
		*num_alloc_blocks = count;

	if (pblock)
		*pblock = chain[depth - 1].key;
	partial = chain + depth - 1;    /* the whole chain */

cleanup:
	while (partial > chain) {
		nvfuse_release_bh(sb, partial->bh, 0, CLEAN);
		partial--;
	}

	if (err)
		return err;

	return 0;
}

/*
* Probably it should be a library function... search for first non-zero word
* or memcmp with zero_page, whatever is better for particular architecture.
* Linus?
*/
static inline int all_zeroes(u32 *p, u32 *q)
{
	while (p < q)
		if (*p++)
			return 0;
	return 1;
}


/**
*	ext2_free_data - free a list of data blocks
*	@inode:	inode we are dealing with
*	@p:	array of block numbers
*	@q:	points immediately past the end of array
*
*	We are freeing all blocks referred from that array (numbers are
*	stored as little-endian 32-bit) and updating @inode->i_blocks
*	appropriately.
*/
static inline void nvfuse_free_data(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
				    u32 *p, u32 *q)
{
	unsigned long block_to_free = 0, count = 0;
	unsigned long nr;

	for (; p < q; p++) {
		nr = *p;
		if (nr) {
			*p = 0;
			/* accumulate blocks to free if they're contiguous */
			if (count == 0)
				goto free_this;
			else if (block_to_free == nr - count)
				count++;
			else {
				nvfuse_free_blocks(sb, block_to_free, count);
				nvfuse_mark_inode_dirty(ictx);
free_this:
				block_to_free = nr;
				count = 1;
			}
		}
	}
	if (count > 0) {
		nvfuse_free_blocks(sb, block_to_free, count);
		nvfuse_mark_inode_dirty(ictx);
	}

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
}

/**
*	ext2_find_shared - find the indirect blocks for partial truncation.
*	@inode:	  inode in question
*	@depth:	  depth of the affected branch
*	@offsets: offsets of pointers in that branch (see ext2_block_to_path)
*	@chain:	  place to store the pointers to partial indirect blocks
*	@top:	  place to the (detached) top of branch
*
*	This is a helper function used by ext2_truncate().
*
*	When we do truncate() we may have to clean the ends of several indirect
*	blocks but leave the blocks themselves alive. Block is partially
*	truncated if some data below the new i_size is referred from it (and
*	it is on the path to the first completely truncated data block, indeed).
*	We have to free the top of that path along with everything to the right
*	of the path. Since no allocation past the truncation point is possible
*	until ext2_truncate() finishes, we may safely do the latter, but top
*	of branch may require special attention - pageout below the truncation
*	point might try to populate it.
*
*	We atomically detach the top of branch from the tree, store the block
*	number of its root in *@top, pointers to buffer_heads of partially
*	truncated blocks - in @chain[].bh and pointers to their last elements
*	that should not be removed - in @chain[].p. Return value is the pointer
*	to last filled element of @chain.
*
*	The work left to caller to do the actual freeing of subtrees:
*		a) free the subtree starting from *@top
*		b) free the subtrees whose roots are stored in
*			(@chain[i].p+1 .. end of @chain[i].bh->b_data)
*		c) free the subtrees growing from the inode past the @chain[0].p
*			(no partially truncated stuff there).
*/

static Indirect *nvfuse_find_shared(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
				    int depth,
				    int offsets[4],
				    Indirect chain[4],
				    u32 *top)
{
	Indirect *partial, *p;
	int k, err;

	*top = 0;
	for (k = depth; k > 1 && !offsets[k - 1]; k--)
		;
	partial = nvfuse_get_branch(sb, ictx, ictx->ictx_inode, k, offsets, chain, &err);
	if (!partial)
		partial = chain + k - 1;
	/*
	* If the branch acquired continuation since we've looked at it -
	* fine, it should all survive and (new) top doesn't belong to us.
	*/
	//write_lock(&EXT2_I(inode)->i_meta_lock);
	if (!partial->key && *partial->p) {
		//write_unlock(&EXT2_I(inode)->i_meta_lock);
		goto no_top;
	}
	for (p = partial; p > chain && all_zeroes((u32 *)p->bh->bh_buf, p->p); p--)
		;
	/*
	* OK, we've found the last block that must survive. The rest of our
	* branch should be detached before unlocking. However, if that rest
	* of branch is all ours and does not grow immediately from the inode
	* it's easier to cheat and just decrement partial->p.
	*/
	if (p == chain + k - 1 && p > chain) {
		p->p--;
	} else {
		*top = *p->p;
		*p->p = 0;
	}
	//write_unlock(&EXT2_I(inode)->i_meta_lock);

	while (partial > p) {
		nvfuse_release_bh(sb, partial->bh, 0, 0);
		partial--;
	}
no_top:
	return partial;
}


/**
*	ext2_free_branches - free an array of branches
*	@inode:	inode we are dealing with
*	@p:	array of block numbers
*	@q:	pointer immediately past the end of array
*	@depth:	depth of the branches to free
*
*	We are freeing all blocks referred from these branches (numbers are
*	stored as little-endian 32-bit) and updating @inode->i_blocks
*	appropriately.
*/
static void nvfuse_free_branches(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
				 u32 *p, u32 *q, int depth)
{
	struct nvfuse_buffer_head *bh;
	unsigned long nr;

	if (depth--) {
		int addr_per_block = PTRS_PER_BLOCK;
		for (; p < q; p++) {
			nr = *p;
			if (!nr)
				continue;
			*p = 0;

			bh = nvfuse_get_bh(sb, ictx, BLOCK_IO_INO, nr, READ, NVFUSE_TYPE_META);
			/*
			* A read failure? Report error and clear slot
			* (should be rare).
			*/
			if (!bh) {
				printf("ext2_free_branches Read failure, inode=%ld, block=%ld",
				       (unsigned long)ictx->ictx_inode->i_ino, (unsigned long)nr);
				continue;
			}
			nvfuse_free_branches(sb, ictx,
					     (u32 *)bh->bh_buf,
					     (u32 *)bh->bh_buf + addr_per_block,
					     depth);
			/* FIXME: ??? needed to be revised */
			nvfuse_release_bh(sb, bh, 0, DIRTY);
			//nvfuse_forget_bh(sb, bh);
			nvfuse_free_blocks(sb, nr, 1);
			nvfuse_mark_inode_dirty(ictx);
		}
	} else
		nvfuse_free_data(sb, ictx, p, q);
}


/* dax_sem must be held when calling this function */
static void __nvfuse_truncate_blocks(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
				     u64 offset)
{
	u32 *i_data = ictx->ictx_inode->i_blocks;
	//struct ext2_inode_info *ei = EXT2_I(inode);
	int addr_per_block = PTRS_PER_BLOCK;
	int offsets[4];
	Indirect chain[4];
	Indirect *partial;
	u32 nr = 0;
	int n;
	long iblock;
	unsigned blocksize;
	blocksize = CLUSTER_SIZE;
	iblock = NVFUSE_SIZE_TO_BLK(offset + blocksize - 1);

	n = nvfuse_block_to_path(iblock, (u32 *)offsets, NULL);
	if (n == 0)
		return;

	/*
	* From here we block out all ext2_get_block() callers who want to
	* modify the block allocation tree.
	*/
	//mutex_lock(&ei->truncate_mutex);

	if (n == 1) {
		nvfuse_free_data(sb, ictx, i_data + offsets[0],
				 i_data + DIRECT_BLOCKS);
		goto do_indirects;
	}

	partial = nvfuse_find_shared(sb, ictx, n, offsets, chain, &nr);
	/* Kill the top of shared branch (already detached) */
	if (nr) {
		if (partial == chain)
			nvfuse_mark_inode_dirty(ictx);
		else {
			nvfuse_release_bh(sb, partial->bh, 0, DIRTY);
		}
		nvfuse_free_branches(sb, ictx, &nr, &nr + 1, (chain + n - 1) - partial);
	}
	/* Clear the ends of indirect blocks on the shared branch */
	while (partial > chain) {
		nvfuse_free_branches(sb, ictx,
				     partial->p + 1,
				     (u32 *)partial->bh->bh_buf + addr_per_block,
				     (chain + n - 1) - partial);

		nvfuse_release_bh(sb, partial->bh, 0, DIRTY);
		partial--;
	}
do_indirects:
	/* Kill the remaining (whole) subtrees */
	switch (offsets[0]) {
	default:
		nr = i_data[INDIRECT_BLOCKS];
		if (nr) {
			i_data[INDIRECT_BLOCKS] = 0;
			nvfuse_mark_inode_dirty(ictx);
			nvfuse_free_branches(sb, ictx, &nr, &nr + 1, 1);
		}
	case INDIRECT_BLOCKS:
		nr = i_data[DINDIRECT_BLOCKS];
		if (nr) {
			i_data[DINDIRECT_BLOCKS] = 0;
			nvfuse_mark_inode_dirty(ictx);
			nvfuse_free_branches(sb, ictx, &nr, &nr + 1, 2);
		}
	case DINDIRECT_BLOCKS:
		nr = i_data[TINDIRECT_BLOCKS];
		if (nr) {
			i_data[TINDIRECT_BLOCKS] = 0;
			nvfuse_mark_inode_dirty(ictx);
			nvfuse_free_branches(sb, ictx, &nr, &nr + 1, 3);
		}
	case TINDIRECT_BLOCKS:
		;
	}

	//ext2_discard_reservation(inode);

	//mutex_unlock(&ei->truncate_mutex);
}

void nvfuse_truncate_blocks(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, u64 offset)
{
	/*
	* XXX: it seems like a bug here that we don't allow
	* IS_APPEND inode to have blocks-past-i_size trimmed off.
	* review and fix this.
	*
	* Also would be nice to be able to handle IO errors and such,
	* but that's probably too much to ask.
	*/
	/* if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
		S_ISLNK(inode->i_mode)))
		return;
	if (ext2_inode_is_fast_symlink(inode))
		return;
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return;*/

	//dax_sem_down_write(EXT2_I(inode));
	__nvfuse_truncate_blocks(sb, ictx, offset);
	//dax_sem_up_write(EXT2_I(inode));
}
