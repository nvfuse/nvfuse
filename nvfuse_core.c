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
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#ifdef __linux__
#include <sys/uio.h>
#endif 

#include "nvfuse_buffer_cache.h"
#include "nvfuse_core.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_indirect.h"
#include "nvfuse_bp_tree.h"
#include "nvfuse_config.h"
#include "nvfuse_malloc.h"
#include "nvfuse_api.h"
#include "nvfuse_dirhash.h"

struct nvfuse_superblock * nvfuse_read_super(struct nvfuse_handle *nvh)
{		
	return &nvh->nvh_sb;
}

void nvfuse_release_super(struct nvfuse_superblock *sb)
{	
	return;
}

s32 nvfuse_dir_is_invalid(struct nvfuse_dir_entry *dir) 
{
	if (dir->d_flag == DIR_EMPTY || dir->d_flag == DIR_DELETED)
		return 1;

	return 0;
}

void nvfuse_print_inode(struct nvfuse_inode *inode, s8 *str)
{
	if(inode->i_type == NVFUSE_TYPE_DIRECTORY){
		printf("%-13s/ [%ld] ino : %3d\n", str, (long)inode->i_size, inode->i_ino);
	}else if(inode->i_type ==NVFUSE_TYPE_FILE){
		printf("%-14s [%ld] ino : %3d\n", str, (long)inode->i_size, inode->i_ino);
	}
}

struct nvfuse_segment_summary *nvfuse_get_segment_summary(struct nvfuse_superblock *sb, u32 seg_id){
	assert(seg_id == sb->sb_ss[seg_id].ss_id);
	return sb->sb_ss + seg_id;
}

struct nvfuse_inode_ctx *nvfuse_read_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, inode_t ino)
{
	struct nvfuse_inode_ctx *alloc_ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_buffer_head *bh;
	lbno_t block;
	lbno_t offset;
	
	if (ino < ROOT_INO)
		return NULL;

	if (ictx == NULL)
	{
		alloc_ictx = nvfuse_get_ictx(sb, ino);
		if (alloc_ictx == NULL)
			return NULL;
	}
	else 
	{
		alloc_ictx = ictx;
	}
	
	block = ino / IFILE_ENTRY_NUM;
	offset = ino % IFILE_ENTRY_NUM;

	bh = nvfuse_get_bh(sb, alloc_ictx, IFILE_INO, block, READ, NVFUSE_TYPE_META);
	inode = (struct nvfuse_inode *)bh->bh_buf;
	inode += offset;
	assert(ino == inode->i_ino);

	alloc_ictx->ictx_ino = ino;
	alloc_ictx->ictx_inode = inode;
	alloc_ictx->ictx_bh = bh;	
	alloc_ictx->ictx_ref++;
	
	assert(inode->i_ino);
	
	return alloc_ictx;
}

void nvfuse_mark_inode_dirty(struct nvfuse_inode_ctx *ictx)
{
	set_bit(&ictx->ictx_status, BUFFER_STATUS_DIRTY);
	clear_bit(&ictx->ictx_status, BUFFER_STATUS_CLEAN);
}

void nvfuse_release_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 dirty) 
{
	struct nvfuse_inode *inode;
	struct nvfuse_inode *ip;
	struct nvfuse_buffer_head *bh;
	u32 offset, block;

	if (ictx == NULL)
		return;

	inode = ictx->ictx_inode;
	bh = ictx->ictx_bh;
	if (bh)
	{
		nvfuse_set_bh_status(bh, BUFFER_STATUS_META);
		nvfuse_release_bh(sb, bh, 0/*head*/, dirty);
	}
	
	ictx->ictx_bh = NULL;

	//assert(ictx->ictx_ref == 1);
		
	nvfuse_release_ictx(sb, ictx, dirty);
}

s32 nvfuse_relocate_delete_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx) 
{
	struct nvfuse_inode *inode;
	inode_t ino;
	u32 seg_id;
	ino = ictx->ictx_ino;
	inode = ictx->ictx_inode;	
	inode->i_deleted = 1;
	inode->i_ino = 0;
	inode->i_size = 0;

	nvfuse_release_inode(sb, ictx, DIRTY);
	nvfuse_inc_free_inodes(sb, ino);
	
	seg_id = ino / sb->sb_no_of_inodes_per_seg;
	nvfuse_release_ibitmap(sb, seg_id, ino);
	return 0;
}

u32 nvfuse_scan_free_ibitmap(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, u32 seg_id, u32 hint_free_inode)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;	
	struct nvfuse_buffer_head *bh;
	void *buf;	
	u32 count = 0;
	u32 free_inode = hint_free_inode;
	u32 found = 0;

	ss_bh = nvfuse_get_bh(sb, ictx, SS_INO, seg_id, READ, NVFUSE_TYPE_META);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	bh = nvfuse_get_bh(sb, ictx, IBITMAP_INO, seg_id, READ, NVFUSE_TYPE_META);
	buf = bh->bh_buf;

	while (ss->ss_free_inodes && count < sb->sb_no_of_inodes_per_seg)
	{
		if (!ext2fs_test_bit(free_inode, buf))
		{
			//printf(" seg = %d free block %d found \n", seg_id, free_inode);
			found = 1;
			break;
		}
		free_inode = (free_inode + 1) % sb->sb_no_of_inodes_per_seg;
		count++;
	}
	
	if (free_inode < sb->sb_no_of_inodes_per_seg)
	{
		ext2fs_set_bit(free_inode, buf);
		free_inode += (seg_id * ss->ss_max_inodes);
	}
	else
	{
		free_inode = 0;
	}
		
	nvfuse_release_bh(sb, ss_bh, 0, CLEAN);
	if (found)
		nvfuse_release_bh(sb, bh, 0, DIRTY);
	else
		nvfuse_release_bh(sb, bh, 0, CLEAN);

	return free_inode;
}

u32 nvfuse_find_free_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, u32 last_ino)
{
	u32 new_ino = 0;
	u32 hint_ino;
	s32 ret = 0;
	u32 seg_id;
	u32 next_id;

	seg_id = last_ino / sb->sb_no_of_inodes_per_seg;
	next_id = seg_id % sb->sb_segment_num;
	seg_id = (seg_id - 1 + sb->sb_segment_num) % sb->sb_segment_num;

	hint_ino = last_ino % sb->sb_no_of_inodes_per_seg;

	while (next_id != seg_id) {
		new_ino = nvfuse_scan_free_ibitmap(sb, ictx, next_id, hint_ino);
		if (new_ino)
			break;
		next_id = (next_id + 1) % sb->sb_segment_num;
		hint_ino = 0;
	}

	return new_ino;
}


void nvfuse_release_ibitmap(struct nvfuse_superblock *sb, u32 seg_id, u32 ino)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;
	struct nvfuse_buffer_head *bh;
	void *buf;

	ss_bh = nvfuse_get_bh(sb, NULL, SS_INO, seg_id, READ, NVFUSE_TYPE_META);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	bh = nvfuse_get_bh(sb, NULL, IBITMAP_INO, seg_id, READ, NVFUSE_TYPE_META);
	buf = bh->bh_buf;

	if (ext2fs_test_bit(ino % ss->ss_max_inodes, buf))
	{
		ext2fs_clear_bit(ino % ss->ss_max_inodes, buf);		
	}
	else
	{
		printf(" Warning: ino was already released \n");
	}
			
	nvfuse_release_bh(sb, ss_bh, 0, CLEAN);
	nvfuse_release_bh(sb, bh, 0, DIRTY);
}

void nvfuse_inc_free_inodes(struct nvfuse_superblock *sb, inode_t ino)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;
	u32 seg_id; 

	seg_id = ino / sb->sb_no_of_inodes_per_seg;
	ss_bh = nvfuse_get_bh(sb, NULL, SS_INO, seg_id, READ, NVFUSE_TYPE_META);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	ss->ss_free_inodes++;
	sb->sb_free_inodes++;
	assert(ss->ss_free_inodes <= ss->ss_max_inodes);
	nvfuse_release_bh(sb, ss_bh, 0, DIRTY);
	
}

void nvfuse_dec_free_inodes(struct nvfuse_superblock *sb, inode_t ino)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;
	u32 seg_id;
	
	seg_id = ino / sb->sb_no_of_inodes_per_seg;
	ss_bh = nvfuse_get_bh(sb, NULL, SS_INO, seg_id, READ, NVFUSE_TYPE_META);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	ss->ss_free_inodes--;
	sb->sb_free_inodes--;
	assert(ss->ss_free_inodes >= 0);
	nvfuse_release_bh(sb, ss_bh, 0, DIRTY);
}

void nvfuse_inc_free_blocks(struct nvfuse_superblock *sb, u32 blockno)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;
	u32 seg_id;

	seg_id = blockno / sb->sb_no_of_blocks_per_seg;
	ss_bh = nvfuse_get_bh(sb, NULL, SS_INO, seg_id, READ, NVFUSE_TYPE_META);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	ss->ss_free_blocks++;
	sb->sb_free_blocks++;
	sb->sb_no_of_used_blocks--;

	assert(ss->ss_free_blocks <= ss->ss_max_blocks);
	assert(sb->sb_free_blocks <= sb->sb_no_of_blocks);
	assert(sb->sb_no_of_used_blocks >= 0);

	nvfuse_release_bh(sb, ss_bh, 0, DIRTY);
}

void nvfuse_dec_free_blocks(struct nvfuse_superblock *sb, u32 blockno)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;
	u32 seg_id;

	seg_id = blockno / sb->sb_no_of_blocks_per_seg;
	ss_bh = nvfuse_get_bh(sb, NULL, SS_INO, seg_id, READ, NVFUSE_TYPE_META);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	ss->ss_free_blocks--;
	sb->sb_free_blocks--;
	sb->sb_no_of_used_blocks++;
	assert(ss->ss_free_blocks >= 0);
	assert(sb->sb_free_blocks >= 0);
	assert(sb->sb_no_of_used_blocks <= sb->sb_no_of_blocks);

	nvfuse_release_bh(sb, ss_bh, 0, DIRTY);
}

u32 nvfuse_get_free_blocks(struct nvfuse_superblock *sb, u32 seg_id)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;
	u32 free_blocks;

	ss_bh = nvfuse_get_bh(sb, NULL, SS_INO, seg_id, READ, NVFUSE_TYPE_META);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	free_blocks = ss->ss_free_blocks;

	assert(ss->ss_free_blocks >= 0);
	assert(sb->sb_free_blocks >= 0);
	nvfuse_release_bh(sb, ss_bh, 0, DIRTY);

	return free_blocks;
}

inode_t nvfuse_alloc_new_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx)
{
	struct nvfuse_buffer_head *bh;	
	struct nvfuse_inode *ip;
	
	lbno_t search_block = 0, search_entry = 0;
	inode_t alloc_ino = 0;
	inode_t hint_ino = 0;
	inode_t last_allocated_ino = 0;
	u32 block, ifile_num;	
	s32 i, j;

	last_allocated_ino = sb->sb_last_allocated_ino;
	hint_ino = nvfuse_find_free_inode(sb, ictx, last_allocated_ino);
	if (hint_ino)
	{
		search_block = hint_ino / IFILE_ENTRY_NUM;
		search_entry = hint_ino % IFILE_ENTRY_NUM;
	}
	else
	{
		printf(" no more inodes in the file system.");		
		return 0;
	}
	
RETRY:;
	
	bh = nvfuse_get_bh(sb, ictx, IFILE_INO, search_block, READ, NVFUSE_TYPE_META);
	ip = (struct nvfuse_inode *)bh->bh_buf;
#ifdef NVFUSE_USE_MKFS_INODE_ZEROING
	for (j = 0; j < IFILE_ENTRY_NUM; j++) 
	{
		if (ip[search_entry].i_ino == 0 && (search_entry + search_block * IFILE_ENTRY_NUM) >= NUM_RESV_INO)
		{
			alloc_ino = search_entry + search_block * IFILE_ENTRY_NUM;		
			goto RES;
		}			
		search_entry = (search_entry + 1) % IFILE_ENTRY_NUM;
	}		

	/* FIXME: need to put error handling code  */
	printf(" Warning: it runs out of free inodes = %d \n", sb->sb_free_inodes);
	printf(".");
	while (1);

RES:;

#else
	alloc_ino = search_entry + search_block * IFILE_ENTRY_NUM;	
#endif
		
	nvfuse_dec_free_inodes(sb, alloc_ino);

	ip += search_entry;
	
	/* initialization of inode entry */
	memset(ip, 0x00, INODE_ENTRY_SIZE);

	ip->i_ino = alloc_ino;
	ip->i_deleted = 0; 
	ip->i_version++;		
	
	nvfuse_release_bh(sb, bh, 0, DIRTY);

	/* keep hit information to rapidly find a free inode */
	sb->sb_last_allocated_ino = alloc_ino + 1;

	return alloc_ino;
}

void nvfuse_free_blocks(struct nvfuse_superblock *sb, u32 block_to_delete, u32 count)
{
	u32 end_blk = block_to_delete + count;
	u32 start_blk = block_to_delete;
	
	while (start_blk < end_blk) {
		u32 seg_id = start_blk / sb->sb_no_of_blocks_per_seg;
		u32 offset = start_blk % sb->sb_no_of_blocks_per_seg;
		nvfuse_free_dbitmap(sb, seg_id, offset);
		start_blk ++;
	}
}

void nvfuse_free_inode_size(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s64 size)
{
	struct nvfuse_inode *inode;
	struct nvfuse_buffer_cache *bc;
	lbno_t offset;
	u32 num_block, trun_num_block;
	u32 deleted_bno;	
	s32 res;
	u64 key;

	inode = ictx->ictx_inode;

	num_block = inode->i_size >> CLUSTER_SIZE_BITS;
	trun_num_block = size >> CLUSTER_SIZE_BITS;
	if(inode->i_size & (CLUSTER_SIZE-1))
		num_block++;

	/* debug code 
	*if(trun_num_block)
	*	printf(" trun num block = %d\n", trun_num_block);
	*/

	if(!num_block || num_block <= trun_num_block)
		return;

	for(offset = num_block-1; offset >= trun_num_block; offset--)
	{
		nvfuse_make_pbno_key(inode->i_ino, offset, &key, NVFUSE_BP_TYPE_DATA);
		bc = (struct nvfuse_buffer_cache *)nvfuse_hash_lookup(sb->sb_bm, key);
		if(bc)
		{
			nvfuse_remove_bh_in_bc(sb, bc);

			/* FIXME: reinitialization is necessary */
			bc->bc_load = 0;			
			bc->bc_pno = 0;	
			bc->bc_dirty = 0;

			nvfuse_move_buffer_type(sb, bc, BUFFER_TYPE_UNUSED, INSERT_HEAD);
		}

		if(offset == 0)
			break;	
	}

	/* truncate blocks */
	nvfuse_truncate_blocks(sb, ictx, size);	
}


///*
//* For the benefit of those who are trying to port Linux to another
//* architecture, here are some C-language equivalents.  You should
//* recode these in the native assmebly language, if at all possible.
//*
//* C language equivalents written by Theodore Ts'o, 9/26/92.
//* Modified by Pete A. Zaitcev 7/14/95 to be portable to big endian
//* systems, as well as non-32 bit systems.
//*/
//
s32 ext2fs_set_bit(u32 nr,void * addr)
{
	s32		mask, retval;
	u8	*ADDR = (u8 *) addr;

	ADDR += nr >> 3;
	mask = 1 << (nr & 0x07);
	retval = mask & *ADDR;
	*ADDR |= mask;
	return retval;
}

s32 ext2fs_clear_bit(u32 nr, void * addr)
{
	s32		mask, retval;
	u8	*ADDR = (u8 *) addr;

	ADDR += nr >> 3;
	mask = 1 << (nr & 0x07);
	retval = mask & *ADDR;
	*ADDR &= ~mask;
	return retval;
}

s32 ext2fs_test_bit(u32 nr, const void * addr)
{
	s32			mask;
	const u8	*ADDR = (const u8 *) addr;

	ADDR += nr >> 3;
	mask = 1 << (nr & 0x07);
	return (mask & *ADDR);
}

s32 nvfuse_set_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename, u32 offset)
{
	bkey_t *key;
	u32 dir_hash[2];
	u32 collision = ~0; 	
	u32 cur_offset;
	
	master_node_t *master;

	master= bp_init_master();
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;	
	bp_read_master(master);

	collision >>= NVFUSE_BP_COLLISION_BITS;
	offset &= collision;

	key = B_KEY_ALLOC();

	nvfuse_dir_hash(filename, dir_hash, dir_hash + 1);	
	*key = (u64)dir_hash[0] | ((u64)dir_hash[1]) << 32;
	if (B_INSERT(master, key, &offset, &cur_offset, 0) < 0) {
		u32 c = cur_offset >> (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);		
		c++;

		//printf(" file name collision = %x, %d\n", dir_hash, c);

		c <<= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
		/* if collision occurs, offset is set to 0 */
		cur_offset = 0;
		cur_offset = c | cur_offset;
		//collision
		B_UPDATE(master, key, &cur_offset);
	}
	bp_write_master(master);
	bp_deinit_master(master);

	B_KEY_FREE(key);

	return 0;
}

s32 nvfuse_get_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename, bitem_t *offset){
	bkey_t *key;
	u32 dir_hash[2];	
	u32 collision = ~0; 	
	u32 c;	
	int res = 0;
	
	master_node_t *master;

	master = bp_init_master();
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;
	bp_read_master(master);

	if (!strcmp(filename, ".") || !strcmp(filename,"..")) {
		*offset = 0;
		return 0;
	}

	key = B_KEY_ALLOC();

	collision >>= NVFUSE_BP_COLLISION_BITS;
	nvfuse_dir_hash(filename, dir_hash, dir_hash + 1);
	*key = (u64)dir_hash[0] | ((u64)dir_hash[1]) << 32;
	if (bp_find_key(master, key, offset) < 0) {
		res = -1;
		goto RES;
	}
	
	c = *offset;
	c >>= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
	if(c)
		*offset = 0;
	else
		*offset &= collision;
RES:;

	B_KEY_FREE(key);

	bp_deinit_master(master);
	return res;
}


s32 nvfuse_update_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename, bitem_t *offset) {
	bkey_t *key;
	u32 dir_hash[2];
	u32 collision = ~0;
	u32 c;
	int res = 0;

	master_node_t *master;

	master = bp_init_master();
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;
	bp_read_master(master);

	if (!strcmp(filename, ".") || !strcmp(filename, "..")) {
		*offset = 0;
		return 0;
	}

	key = B_KEY_ALLOC();

	collision >>= NVFUSE_BP_COLLISION_BITS;
	nvfuse_dir_hash(filename, dir_hash, dir_hash + 1);
	*key = (u64)dir_hash[0] | ((u64)dir_hash[1]) << 32;
	if (bp_find_key(master, key, offset) < 0) {
		res = -1;
		goto RES;
	}

	c = *offset;
	c >>= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
	if (c)
		*offset = 0;
	else
		*offset &= collision;
RES:;

	B_KEY_FREE(key);

	bp_deinit_master(master);
	return res;
}

s32 nvfuse_del_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename){
	u32 dir_hash[2];
	bkey_t *key;
	bitem_t offset = 0;
	u32 collision = ~0; 	
	u32 c;
	master_node_t *master = NULL;

	master = bp_init_master();
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;
	bp_read_master(master);

	collision >>= NVFUSE_BP_COLLISION_BITS;	
	
	key = B_KEY_ALLOC();
	
	nvfuse_dir_hash(filename, dir_hash, dir_hash + 1);
	*key = (u64)dir_hash[0] | ((u64)dir_hash[1]) << 32;

	if(bp_find_key(master, key, &offset) < 0){
		printf(" find key %lu \n", (unsigned long)*key);
		return -1;
	}
		
	c = offset;
	c >>= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
	if(c == 0)
		B_REMOVE(master, key);	
	else{
		c--;
		c <<= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
		offset &= collision;
		offset = c | offset;
		//collision
		B_UPDATE(master, key, &offset);
	}
	
	B_KEY_FREE(key);

	bp_write_master(master);
	bp_deinit_master(master);
	return 0;
}

s32 nvfuse_make_jobs(struct io_job **jobs, int numjobs)
{
	*jobs = (struct io_job *)malloc(sizeof(struct io_job) * numjobs);
	if (!jobs) 
	{
		printf("  Malloc Error: allocate jobs \n");
		*jobs = NULL;
		return -1;
	}

	//printf(" make jobs has completed successfully\n");
	return 0;
}

void io_cancel_incomplete_ios(struct nvfuse_superblock *sb, struct io_job *jobq, int job_cnt){
    struct io_job *job;
    int i;

    for(i = 0;i < job_cnt;i++){
        job = jobq + i;

        if(job && job->complete)
            continue;

        nvfuse_aio_cancel(job, sb->io_manager);
    }
}

s32 nvfuse_wait_aio_completion(struct nvfuse_superblock *sb, struct io_job *jobq, int job_cnt)
{
	struct io_job *job;
	unsigned long blkno;
	unsigned int bcount;
	int cc = 0; // completion count

	nvfuse_aio_resetnextcjob(sb->io_manager);
	cc = nvfuse_aio_complete(sb->io_manager);
	sb->io_manager->queue_cur_count -= cc;

	//printf(" tid = %d cc = %d \n", th_p->tid, cc);
	while (cc--) {
		job = nvfuse_aio_getnextcjob(sb->io_manager);

		if(job->ret!=job->bytes){
			printf(" IO error \n");
		}

		job->complete = 1;
	}

	// TODO: io cancel 
	io_cancel_incomplete_ios(sb, jobq, job_cnt);

	return 0;
}

s32 nvfuse_sync_dirty_data(struct nvfuse_superblock *sb, struct list_head *head, s32 num_blocks)
{	
	struct list_head *ptr, *temp;
	struct nvfuse_buffer_head *bh;
	struct nvfuse_buffer_cache *bc;	
	s32 res = 0;

#ifdef USE_AIO_WRITE
	struct io_job *jobs;
	struct iocb **iocb;
	s32 count = 0;	

	res = nvfuse_make_jobs(&jobs, num_blocks);
	if (res < 0) {
		return res;
	}

	iocb = (struct iocb **)malloc(sizeof(struct iocb *) * num_blocks);
	if (!iocb) {
		printf(" Malloc error: struct iocb\n");
		return -1;
	}

	list_for_each_safe(ptr, temp, head) {
		bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);
		assert(bc->bc_dirty);

		(*(jobs + count)).offset = (long)bc->bc_pno * CLUSTER_SIZE;
		(*(jobs + count)).bytes = (size_t)CLUSTER_SIZE;
		(*(jobs + count)).ret = 0;
		(*(jobs + count)).req_type = WRITE;
		(*(jobs + count)).buf = bc->bc_buf;
		(*(jobs + count)).complete = 0;
		iocb[count] = &((*(jobs + count)).iocb);
		count++;

	}

	count = 0;
	while(count < num_blocks)
	{
		nvfuse_aio_prep(jobs + count, sb->io_manager);
		count++;
	}

	nvfuse_aio_submit(iocb, num_blocks, sb->io_manager);
	sb->io_manager->queue_cur_count = num_blocks;

	nvfuse_wait_aio_completion(sb, jobs, num_blocks);

	free(iocb);
	free(jobs);
#else
	list_for_each_safe(ptr, temp, head) {
		bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);
		assert(bc->bc_dirty);
		nvfuse_write_cluster(bc->bc_buf, bc->bc_pno, sb->io_manager);
	}
#endif

	list_for_each_safe(ptr, temp, head) {
		bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);
		
		nvfuse_remove_bh_in_bc(sb, bc);

		assert(bc->bc_dirty);
		bc->bc_dirty = 0;
		if (bc->bc_ref) {
			printf("debug\n");
		}
		nvfuse_move_buffer_type(sb, bc, BUFFER_TYPE_CLEAN, INSERT_HEAD);
	}
	
	return 0;
}

void nvfuse_printf_statistics(struct nvfuse_superblock *sb, FILE *fp, s8 *dev_name){
	struct timeval total_tv;
	struct timeval total_tv2;
	struct timeval other_tv;	
	s32 i;
	
	fprintf(fp, "-------------------------------------\n");
	fprintf(fp,"\n Read Write Statistics ... \n");
		
	fprintf(fp," Utilization = %0.2f\n", (float)sb->sb_no_of_used_blocks/(float)sb->sb_no_of_blocks);
	fprintf(fp," dev name = %s\n", dev_name);
	fprintf(fp," Read Blocks = %lu\n", (long)sb->sb_read_blocks); 
	fprintf(fp," Write Blocks = %lu\n", (long)sb->sb_write_blocks);
	fprintf(fp," Read IOs = %lu\n", (long)sb->sb_read_ios);
	fprintf(fp," Write IOs = %lu\n", (long)sb->sb_write_ios);
	fprintf(fp," BP Write Blocks = %lu\n", (long)sb->sb_bp_write_blocks);
	fprintf(fp,"\n");

	fprintf(fp,"\n");

	memcpy(&total_tv, &sb->sb_time_total, sizeof(struct timeval));

	total_tv2.tv_sec = 0;
	total_tv2.tv_usec = 0;
	
	timeval_subtract(&other_tv, &total_tv, &total_tv2);

	fprintf(fp," Total Execution Time = %06ld.%06ld\n", 
			sb->sb_time_total.tv_sec, sb->sb_time_total.tv_usec);
	fprintf(fp," Total Other Time = %06ld.%06ld\n", 
			other_tv.tv_sec, other_tv.tv_usec);	
	fprintf(fp,"\n");
		
	fprintf(fp," \n\n");

}

void nvfuse_write_statistics(struct nvfuse_superblock *sb){	
	s8 out_str[128];
	s8 dev_str[128];
	s32 i;
	FILE *fp;

	if (sb->io_manager->dev_path != NULL && sb->io_manager->dev_path[0] == '/') {
		strcpy(dev_str, sb->io_manager->dev_path);

		for(i = 0;i < strlen(dev_str);i++){
			if(dev_str[i] == '/')
				dev_str[i] = '_';
		}	
	} else {
		sprintf(dev_str, "NONAME");
		sprintf(out_str, "nvfuse_stat_file.txt");
	}
	
	fp = fopen(out_str,"a+");
	nvfuse_printf_statistics(sb, fp, dev_str);
	fclose(fp);
}

s32 nvfuse_mount(struct nvfuse_handle *nvh) 
{
	struct nvfuse_superblock *sb;
	struct nvfuse_buffer_list *bh;
	struct nvfuse_superblock *nvfuse_sb_disk;
	struct nvfuse_segment_buffer *seg_buf;
	
	struct timeval start_tv, end_tv, result_tv;
	void *buf;
	s32 i, j, res = 0;
	
	nvfuse_lock_init();
	
	sb = nvfuse_read_super(nvh);
	
	memset(sb, 0x00, sizeof(struct nvfuse_superblock));
	
	sb->io_manager = &nvh->nvh_iom;
	
	res = nvfuse_init_buffer_cache(sb);
	if (res < 0)
	{
		printf(" Error: initialization of buffer cache \n");
		return -1;
	}

	res = nvfuse_init_ictx_cache(sb);
	if (res < 0)
	{
		printf(" Error: initialization of inode context cache \n");
		return -1;
	}
	if (nvh->nvh_mounted)
		return -1;

	sb->sb_file_table = (struct nvfuse_file_table *)nvfuse_malloc(sizeof(struct nvfuse_file_table) * MAX_OPEN_FILE);
	if (sb->sb_file_table == NULL) {
		printf(" nvfuse_malloc error \n");
		return -1;
	}
	memset(sb->sb_file_table, 0x00, sizeof(struct nvfuse_file_table) * MAX_OPEN_FILE);
	for (i = 0; i < MAX_OPEN_FILE; i++) {
		struct nvfuse_file_table *ft = &sb->sb_file_table[i];
		pthread_mutex_init(&ft->ft_lock, NULL);
	}
		
	pthread_mutex_init(&sb->sb_iolock, NULL);
	pthread_mutex_init(&sb->sb_file_table_lock, NULL);
	pthread_mutex_init(&sb->sb_prefetch_lock, NULL);
	pthread_cond_init(&sb->sb_prefetch_cond, NULL);	
	pthread_mutex_init(&sb->sb_request_lock, NULL);

	gettimeofday(&start_tv, NULL);

	res = nvfuse_scan_superblock(sb);
	if (res < 0) {
		printf(" invalid signature !!\n");
		return res;
	}

	gettimeofday(&end_tv, NULL);

	timeval_subtract(&result_tv, &end_tv, &start_tv);
	printf("\n scan time %.3d second\n", (s32)((float)result_tv.tv_sec + (float)result_tv.tv_usec / (float)1000000));
	gettimeofday(&sb->sb_last_update, NULL);
		
	nvfuse_set_cwd_ino(nvh, sb->sb_root_ino);
	nvfuse_set_root_ino(nvh, sb->sb_root_ino);

	gettimeofday(&sb->sb_sync_time, NULL);
		
	if (!sb->sb_umount) {
		sb->sb_cur_segment = 0;
		sb->sb_next_segment = 0;
		nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	}
	else 
	{
		sb->sb_umount = 0;
	}

	sb->sb_ss = (struct nvfuse_segment_summary *)nvfuse_malloc(sizeof(struct nvfuse_segment_summary) * sb->sb_segment_num);
	if (sb->sb_ss == NULL) 
	{
		printf("nvfuse_malloc error = %d\n", __LINE__);
		return -1;
	}
	memset(sb->sb_ss, 0x00, sizeof(struct nvfuse_segment_summary) * sb->sb_segment_num);

	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL)
	{
		printf(" malloc error \n");
		return NVFUSE_ERROR;
	}

	// load segment summary in memory
	for (i = 0; i < sb->sb_segment_num; i++)
	{
		u32 cno = NVFUSE_SUMMARY_OFFSET + i * sb->sb_no_of_blocks_per_seg;
		nvfuse_read_cluster(buf, cno, sb->io_manager);
		memcpy(sb->sb_ss + i, buf, sizeof(struct nvfuse_segment_summary));
		//printf("seg %d ibitmap start = %d \n", i, g_nvfuse_sb->sb_ss[i].ss_ibitmap_start);
	}
	nvfuse_free_aligned_buffer(buf);

	/* create b+tree index for root directory at first mount after formattming */
	if (sb->sb_mount_cnt == 0)
	{
		struct nvfuse_inode_ctx *root_ictx;
		struct nvfuse_inode *root_inode;

		/* read root inode */
		root_ictx = nvfuse_read_inode(sb, NULL, sb->sb_root_ino);

		/* create bptree related nodes */
		nvfuse_create_bptree(sb, root_ictx->ictx_inode);

		/* mark dirty and copy */
		nvfuse_release_inode(sb, root_ictx, DIRTY);

		/* sync dirty data to storage medium */
		nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	}

	nvh->nvh_mounted = 1;

	sb->sb_dirty_sync_policy = NVFUSE_META_DIRTY_POLICY;
	//sb->sb_dirty_sync_policy = NVFUSE_META_DIRTY_SYNC_DELAYED;
	switch (sb->sb_dirty_sync_policy)
	{
	    case DIRTY_FLUSH_DELAY:
		printf(" DIRTY_FLUSH_POLICY: DELAY \n");
		break;
	    case DIRTY_FLUSH_FORCE:
		printf(" DIRTY_FLUSH_POLICY: FORCE \n");
		break;
	    default:
		printf(" DIRTY_FLUSH_POLICY: UNKNOWN\n");
		break;
	}

	gettimeofday(&sb->sb_time_start, NULL);

	printf(" NVFUSE has been successfully mounted. \n");

	return NVFUSE_SUCCESS;
}

s32 nvfuse_umount(struct nvfuse_handle *nvh)
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s32 i;
	s8 *buf;
	
	if(!nvh->nvh_mounted)
		return -1;

	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL)
	{
	    printf(" Error: alloc aligned buffer \n");
	    return -1;
	}

	gettimeofday(&sb->sb_time_end, NULL);
	timeval_subtract(&sb->sb_time_total, &sb->sb_time_end, &sb->sb_time_start);

	nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);

	nvfuse_copy_mem_sb_to_disk_sb((struct nvfuse_superblock *)buf, sb);
	nvfuse_write_cluster(buf, INIT_NVFUSE_SUPERBLOCK_NO, sb->io_manager);

	nvfuse_free_buffer_cache(sb);	
	nvfuse_free_ictx_cache(sb);

	pthread_mutex_destroy(&sb->sb_iolock);
	pthread_mutex_destroy(&sb->sb_request_lock);
	
	pthread_mutex_destroy(&sb->sb_file_table_lock);
	pthread_mutex_destroy(&sb->sb_prefetch_lock);
	pthread_cond_destroy(&sb->sb_prefetch_cond);

	for(i = 0;i < MAX_OPEN_FILE;i++){
		struct nvfuse_file_table *ft = sb->sb_file_table + i;
		pthread_mutex_destroy(&ft->ft_lock);
	}
		
	free(sb->sb_ss_nfinfo);
	free(sb->sb_ss);
	free(sb->sb_sb);
	free(sb->sb_bm);
	free(sb->sb_file_table);	
	
	nvfuse_lock_exit();

	nvh->nvh_mounted = 0;

	nvfuse_free_aligned_buffer(buf);

	return 0;
}


void nvfuse_copy_mem_sb_to_disk_sb(struct nvfuse_superblock *disk_sb, struct nvfuse_superblock *memory_sb)
{
	memcpy(disk_sb, memory_sb, sizeof(struct nvfuse_superblock_common));
}


void nvfuse_copy_disk_sb_to_sb(struct nvfuse_superblock *memory, struct nvfuse_superblock *disk)
{
	memcpy(memory, disk, sizeof(struct nvfuse_superblock_common));
}

s32 nvfuse_is_sb(s8 *buf){
	struct nvfuse_superblock *sb = (struct nvfuse_superblock *)buf;
	
	if(sb->sb_signature == NVFUSE_SB_SIGNATURE)
		return 1;
	else
		return 0;
}

s32 nvfuse_is_ss(s8 *buf){
	struct nvfuse_segment_summary *ss = (struct nvfuse_segment_summary *)buf;

	if(ss->ss_magic == NVFUSE_SS_SIGNATURE)
		return 1;
	else
		return 0;

}

#if NVFUSE_OS == NVFUSE_OS_LINUX
u32 get_part_size(s32 fd)
{
	u32 size, blksize;

#ifndef BLKSSZGET
	blksize = 512;
#else
	ioctl(fd, BLKSSZGET, &blksize);
#endif

	ioctl(fd, BLKGETSIZE, &size);

	return size*blksize;
}
u32 get_sector_size(s32 fd)
{
	u32 sector_size;

#ifndef BLKSSZGET
	sector_size = 512;
#else
	ioctl(fd, BLKSSZGET, &sector_size);
#endif
	return sector_size;
}

u64 get_no_of_sectors(s32 fd)
{
	u64 no_of_sectors;
#if USE_MTDIO == 1
	//struct mtd_info_user meminfo;
#endif 
#if USE_UNIXIO == 1
	ioctl(fd, BLKGETSIZE, &no_of_sectors);
#endif

#if USE_MTDIO == 1
	//if (ioctl(fd, MEMGETINFO, &meminfo) != 0) {
	//	perror("ioctl(MEMGETINFO)");
	//	//close(io_manager->dev);
		//BUG();
	//}
	//no_of_sectors = meminfo.size / 512;
#endif

	printf(" no of sectors = %lu\n", no_of_sectors);
	return no_of_sectors;
}
#endif

s32 nvfuse_scan_superblock(struct nvfuse_superblock *cur_sb)
{
	s32 res = -1, i;
	s8 *buf;
	u64 num_clu, num_sectors, num_seg;
	pbno_t cno;	
	struct nvfuse_superblock *read_sb;
	
#if NVFUSE_OS == NVFUSE_OS_WINDOWS
	num_sectors = NO_OF_SECTORS;
	num_clu = (u32)NVFUSE_NUM_CLU;
#else
#	ifdef __USE_FUSE__
	num_sectors = get_no_of_sectors(cur_sb->io_manager->dev);
	num_clu = (u32)num_sectors / (u32)SECTORS_PER_CLUSTER;
#	else
#		if USE_RAMDISK == 1 || USE_FILEDISK == 1
		num_sectors = NO_OF_SECTORS;
		num_clu = (u32)NVFUSE_NUM_CLU;
#		elif USE_UNIX_IO == 1
		num_sectors = get_no_of_sectors(cur_sb->io_manager->dev);
		num_clu = num_sectors / (u32)SECTORS_PER_CLUSTER;
#		elif USE_SPDK == 1
		num_sectors = cur_sb->io_manager->total_blkcount;
		num_clu = num_sectors/SECTORS_PER_CLUSTER;
#		endif
#	endif
	num_sectors = cur_sb->io_manager->total_blkcount;
	num_clu = num_sectors/SECTORS_PER_CLUSTER;
	/* FIXME: total_blkcount must be set to when io_manager is initialized. */
#endif

	printf(" sectors = %lu, blocks = %lu\n", (unsigned long)num_sectors, (unsigned long)num_clu);

	num_seg = NVFUSE_SEG_NUM(num_clu, NVFUSE_SEGMENT_SIZE_BITS-CLUSTER_SIZE_BITS);
	num_clu = num_seg << (NVFUSE_SEGMENT_SIZE_BITS-CLUSTER_SIZE_BITS);// (NVFUSE_SEGMENT_SIZE/NVFUSE_CLUSTER_SIZE);

	buf = (s8 *)nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if(buf == NULL)	{
		printf(" nvfuse_malloc error \n");
	}
		
	memset(buf, 0x00, CLUSTER_SIZE);
	cno = INIT_NVFUSE_SUPERBLOCK_NO;

	nvfuse_read_cluster(buf, cno, cur_sb->io_manager);
	read_sb = (struct nvfuse_superblock *)buf;
	
	if(read_sb->sb_signature == NVFUSE_SB_SIGNATURE){
		nvfuse_copy_disk_sb_to_sb(cur_sb, read_sb);
		res = 0;
	}else{
		printf(" super block signature is mismatched. \n");
		res = -1;
	}

	printf(" root ino = %d \n", cur_sb->sb_root_ino);
	printf(" no of sectors = %ld \n", (unsigned long)cur_sb->sb_no_of_sectors);
	printf(" no of blocks = %ld \n", (unsigned long)cur_sb->sb_no_of_blocks);
	printf(" no of used blocks = %ld \n", (unsigned long)cur_sb->sb_no_of_used_blocks);
	printf(" no of inodes per seg = %d \n", cur_sb->sb_no_of_inodes_per_seg);
	printf(" no of blocks per seg = %d \n", cur_sb->sb_no_of_blocks_per_seg);
	printf(" no of free inodes = %d \n", cur_sb->sb_free_inodes);
	printf(" no of free blocks = %ld \n", (unsigned long)cur_sb->sb_free_blocks);

	nvfuse_free_aligned_buffer(buf);
	return res;
}

u32 nvfuse_create_bptree(struct nvfuse_superblock *sb, struct nvfuse_inode *inode)
{
	master_node_t *master;
	int ret;

	/* make b+tree master and root nodes */
	master = bp_init_master();
	ret = bp_alloc_master(sb, master);
	if (ret < 0)
	{
		return -1;
	}

	bp_init_root(master);
	bp_write_master(master);

	/* update bptree inode */
	inode->i_bpino = master->m_ino;
	/* deinit b+tree memory structure */
	bp_deinit_master(master);

	return 0;
}

s32 nvfuse_dir(struct nvfuse_handle *nvh)
{
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_dir_entry *dir;	
	struct nvfuse_superblock *sb;
	s64 read_bytes;
	s64 dir_size;	

	sb = nvfuse_read_super(nvh);

	dir_ictx = nvfuse_read_inode(sb, NULL, nvfuse_get_cwd_ino(nvh));
	dir_inode = dir_ictx->ictx_inode;

	dir_size = dir_inode->i_size;

	for (read_bytes = 0; read_bytes < dir_size ; read_bytes+=DIR_ENTRY_SIZE)
	{
		if (!(read_bytes & (CLUSTER_SIZE-1)))
		{
			if (dir_bh != NULL)
			{
				nvfuse_release_bh(sb, dir_bh, 0, 0);
				dir_bh = NULL;
			}
			dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, (u32)(read_bytes>>CLUSTER_SIZE_BITS), READ, NVFUSE_TYPE_META);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED)
		{
			ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
			inode = ictx->ictx_inode;

			nvfuse_print_inode(inode, dir->d_filename);

			nvfuse_release_inode(sb, ictx, CLEAN);
		}
		dir++;
	}

	nvfuse_release_bh(sb, dir_bh, 0, 0);
	nvfuse_release_inode(sb, dir_ictx, CLEAN);	
	nvfuse_release_super(sb);

	printf("\nfree blocks	  = %ld, num  blocks  = %ld\n", (unsigned long)sb->sb_free_blocks, (unsigned long)sb->sb_no_of_blocks);
	printf("Disk Util     = %2.2f %%\n", ((double)(sb->sb_no_of_blocks - sb->sb_free_blocks) / (double)sb->sb_no_of_blocks) * 100);
	
	return NVFUSE_SUCCESS;
}


s32 nvfuse_allocate_open_file_table(struct nvfuse_superblock *sb)
{
	s32 i = 0, fid = -1;

	pthread_mutex_lock(&sb->sb_file_table_lock);
	for (i = START_OPEN_FILE; i < MAX_OPEN_FILE; i++)
	{
		if (sb->sb_file_table[i].used == FALSE)
		{
			fid = i;
			break;
		}
	}
	pthread_mutex_unlock(&sb->sb_file_table_lock);

	return fid;
}

s32 nvfuse_truncate(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, nvfuse_off_t trunc_size)
{
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_dir_entry *dir;
	struct nvfuse_buffer_head *dir_bh;
	
	s64 read_bytes = 0;
	lbno_t lblock = 0;
	u32 start = 0;
	u32 offset = 0;
	s64 dir_size = 0;
	u32 found_entry;
	
	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;
	dir_size = dir_inode->i_size;

#if NVFUSE_USE_DIR_INDEXING == 1
	if (nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset) < 0) 
	{
		printf(" fixme: filename (%s) is not in the index.\n", filename);
		offset = 0;
	}
#endif

	start = offset * DIR_ENTRY_SIZE;
	if ((start & (CLUSTER_SIZE - 1))) 
	{
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, (u32)(start / CLUSTER_SIZE), READ, NVFUSE_TYPE_META);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir += (offset % DIR_ENTRY_NUM);
	}

	for (read_bytes = start; read_bytes < dir_size; read_bytes += DIR_ENTRY_SIZE) 
	{
		if (!(read_bytes & (CLUSTER_SIZE - 1))) 
		{
			if (dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0/*tail*/, 0/*dirty*/);
			lblock = read_bytes >> CLUSTER_SIZE_BITS;
			dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, (u32)(read_bytes >> CLUSTER_SIZE_BITS), READ, NVFUSE_TYPE_META);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED) 
		{
			if (!strcmp(dir->d_filename, filename)) 
			{
				ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
				inode = ictx->ictx_inode;
				found_entry = read_bytes / DIR_ENTRY_SIZE;
				break;
			}
		}
		dir++;
	}

	if (inode == NULL || inode->i_ino == 0) 
	{
		printf(" file (%s) is not found this directory\n", filename);
		nvfuse_release_bh(sb, dir_bh, 0/*tail*/, CLEAN);
		return NVFUSE_ERROR;
	}

	if (inode->i_type == NVFUSE_TYPE_DIRECTORY) 
	{
		return error_msg(" rmfile() is supported for a file.");
	}

	nvfuse_free_inode_size(sb, ictx, trunc_size);
	inode->i_size = trunc_size;
	assert(inode->i_size < MAX_FILE_SIZE);
	nvfuse_release_inode(sb, ictx, DIRTY);
	
	nvfuse_release_bh(sb, dir_bh, 0/*tail*/, CLEAN);
	nvfuse_release_inode(sb, dir_ictx, CLEAN);
	
	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_truncate_ino(struct nvfuse_superblock *sb, inode_t ino, s64 trunc_size){	
	struct nvfuse_inode_ctx *ictx;	
	
	ictx = nvfuse_read_inode(sb, NULL, ino);	
	nvfuse_free_inode_size(sb, ictx, trunc_size);
	nvfuse_release_inode(sb, ictx, DIRTY);	

	return NVFUSE_SUCCESS;
}

s32 nvfuse_chmod(struct nvfuse_handle *nvh, inode_t par_ino, s8 *filename, mode_t mode){
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_dir_entry *dir;	
	struct nvfuse_buffer_head *dir_bh = NULL;	
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	lbno_t lblock = 0;
	u32 start = 0;
	s64 read_bytes = 0;
	u32 offset = 0;
	s32 mask;
	s64 dir_size = 0;

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;
	
#if NVFUSE_USE_DIR_INDEXING == 1
	if (nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset) < 0) {
		printf(" The smae file eixts = %s \n", filename);
	}
#endif

	dir_size = dir_inode->i_size;
	start = offset * DIR_ENTRY_SIZE;

	if (start&(CLUSTER_SIZE-1))
	{
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, (u32)(start/CLUSTER_SIZE), READ, NVFUSE_TYPE_META);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir+= (offset % DIR_ENTRY_NUM);
	}
	
	for (read_bytes = start; read_bytes < dir_size; read_bytes+=DIR_ENTRY_SIZE)
	{
		if(!(read_bytes & (CLUSTER_SIZE-1)))
		{
			if(dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0, 0);

			lblock = read_bytes>>CLUSTER_SIZE_BITS;
			dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, (u32)(read_bytes>>CLUSTER_SIZE_BITS), READ, NVFUSE_TYPE_META);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED)
		{
			if(!strcmp(dir->d_filename,filename)){
				ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
				inode = ictx->ictx_inode;
				break;
			}
		}
		dir++;
	}

	if(inode->i_ino == 0)
		return NVFUSE_ERROR;
	
	mask = S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
	inode->i_mode = (inode->i_mode & ~mask) | (mode & mask);

	nvfuse_release_inode(sb, ictx, DIRTY);

	nvfuse_release_bh(sb, dir_bh, 0, CLEAN);
	nvfuse_release_inode(sb, dir_ictx, CLEAN);
	
	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_is_directio(struct nvfuse_superblock *sb, s32 fid)
{	
	struct nvfuse_file_table *ft;

	ft = sb->sb_file_table + fid;
		
	if (ft->flags & O_DIRECT)
		return 1;	

	return 0;
}

s32 nvfuse_path_open(struct nvfuse_handle *nvh, s8 *path, s8 *filename, struct nvfuse_dir_entry *get){
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s8 *token;
	s8 b[256];
	u32 local_dir_ino;
	s32 i;
	s32 count=0;
	
	strcpy(b,path);
	i = strlen(b);

	if(path[0] == '/')
		local_dir_ino = nvfuse_get_root_ino(nvh);
	else
		local_dir_ino = nvfuse_get_cwd_ino(nvh);

	token = strtok(b,"/");

	if(token != NULL)
	{
		if(nvfuse_lookup(sb, NULL, &dir_entry, token, local_dir_ino)< 0)
		{
			return NVFUSE_ERROR;
		}

		memcpy(get, &dir_entry, DIR_ENTRY_SIZE);

		while(token = strtok(NULL,"/"))
		{
			local_dir_ino = dir_entry.d_ino;
			if(nvfuse_lookup(sb, NULL, &dir_entry, token, local_dir_ino) < 0)
				return NVFUSE_ERROR;
			memcpy(get, &dir_entry, DIR_ENTRY_SIZE);
		}
	}
	else if(token == NULL)
	{
		get->d_ino = local_dir_ino;
	}

	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_path_open2(struct nvfuse_handle *nvh, s8 *path, s8 *filename, struct nvfuse_dir_entry *get){
	struct nvfuse_superblock *sb; 
	struct nvfuse_inode inode;
	struct nvfuse_dir_entry dir_entry;
	s8 *token;
	s8 b[256];
	u32 local_dir_ino;
	s32 i;
	s32 count=0;
	s32 res = 0;

	sb = nvfuse_read_super(nvh);

	strcpy(b,path);
	i = strlen(b);

	if(b[i-1] == '/'){
		res = NVFUSE_ERROR;
		goto RES;
	}

	while(b[--i] != '/' && i >1);
	if(b[i] == '/')
		b[i] = '\0';


	if(path[0] == '/')
		local_dir_ino = nvfuse_get_root_ino(nvh);
	else
		local_dir_ino = nvfuse_get_cwd_ino(nvh);

	for(count = 0, i = 0;i < strlen(path);i++)
		if(path[i] == '/')
			count++;

	if(count == 0){
		get->d_ino = local_dir_ino;
		goto RES;
	}

	token = strtok(b,"/");
	if(token == NULL)
	{
		get->d_ino = local_dir_ino;
	}
	else if(token != NULL)
	{
		if(nvfuse_lookup(sb, NULL, &dir_entry, token, local_dir_ino)< 0)
		{
			//not found
			res = NVFUSE_ERROR;
			goto RES;
		}

		memcpy(get, &dir_entry, DIR_ENTRY_SIZE);

		while(token = strtok(NULL,"/"))
		{
			local_dir_ino = dir_entry.d_ino;

			if(nvfuse_lookup(sb, NULL, &dir_entry, token, local_dir_ino) < 0)
				return NVFUSE_ERROR;
			memcpy(get, &dir_entry, DIR_ENTRY_SIZE);
		}
	}
	
RES:;

	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_lseek(struct nvfuse_handle *nvh, s32 fd, u32 offset, s32 position)
{
	struct nvfuse_file_table *of;
	struct nvfuse_superblock *sb; 

	sb = nvfuse_read_super(nvh);

	of = sb->sb_file_table + fd;

	if (position == SEEK_SET)             /* SEEK_SET */
		of->rwoffset = offset;
	else if (position == SEEK_CUR)        /* SEEK_CUR */
		of->rwoffset += offset;
	else if (position == SEEK_END)        /* SEEK_END */
		of->rwoffset = of->size - offset;

	nvfuse_release_super(sb);

	return(NVFUSE_SUCCESS);
}


s32 nvfuse_seek(struct nvfuse_superblock *sb, struct nvfuse_file_table *of, s64 offset, s32 position)
{	
	if (position == SEEK_SET)             /* SEEK_SET */
		of->rwoffset = offset;
	else if (position == SEEK_CUR)        /* SEEK_CUR */
		of->rwoffset += offset;
	else if (position == SEEK_END)        /* SEEK_END */
		of->rwoffset = of->size - offset;

	return(NVFUSE_SUCCESS);
}



u32 nvfuse_alloc_dbitmap(struct nvfuse_superblock *sb, u32 seg_id, u32 *alloc_blks, u32 num_blocks)
{
	struct nvfuse_segment_summary *ss;
	struct nvfuse_buffer_head *ss_bh, *bh;
	u32 free_block = 0;
	u32 cnt = 0;
	void *buf;
	u32 flag = 0;
	u32 alloc_cnt = 0;

	ss_bh = nvfuse_get_bh(sb, NULL, SS_INO, seg_id, READ, NVFUSE_TYPE_META);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;

	bh = nvfuse_get_bh(sb, NULL, DBITMAP_INO, seg_id, READ, NVFUSE_TYPE_META);
	buf = bh->bh_buf;
	
	free_block = ss->ss_next_block % sb->sb_no_of_blocks_per_seg;

	while (cnt++ < sb->sb_no_of_blocks_per_seg)
	{
		if (!free_block) {
			free_block = ss->ss_dtable_start % sb->sb_no_of_blocks_per_seg;
			cnt += (ss->ss_dtable_start % sb->sb_no_of_blocks_per_seg - 1);
		}

		if (!ext2fs_test_bit(free_block, buf))
		{
			//printf(" free block %d found \n", free_block);
			ss->ss_next_block = free_block; // keep track of hit information to quickly lookup free blocks.
			ext2fs_set_bit(free_block, buf); // right approach?
			flag = 1;
						
			*alloc_blks = ss->ss_seg_start + free_block;
			alloc_blks++;
			num_blocks--;
			alloc_cnt++;
			if (num_blocks == 0)
				break;
		}
		free_block = (free_block + 1) % sb->sb_no_of_blocks_per_seg;
	}
	
	if (flag) {
		int i;
		nvfuse_release_bh(sb, bh, 0, DIRTY);
		nvfuse_release_bh(sb, ss_bh, 0, CLEAN);
		for (i = 0; i < alloc_cnt; i++)
			nvfuse_dec_free_blocks(sb, ss->ss_seg_start + free_block);

		return alloc_cnt;
	}

	nvfuse_release_bh(sb, bh, 0, CLEAN);
	nvfuse_release_bh(sb, ss_bh, 0, CLEAN);
	return 0;
}

u32 nvfuse_free_dbitmap(struct nvfuse_superblock *sb, u32 seg_id, nvfuse_loff_t offset)
{
	struct nvfuse_segment_summary *ss;
	struct nvfuse_buffer_head *ss_bh, *bh;		
	void *buf;
	int flag = 0;

	ss_bh = nvfuse_get_bh(sb, NULL, SS_INO, seg_id, READ, NVFUSE_TYPE_META);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	bh = nvfuse_get_bh(sb, NULL, DBITMAP_INO, seg_id, READ, NVFUSE_TYPE_META);
	buf = bh->bh_buf;
		
	if (ext2fs_test_bit(offset, buf))
	{
		ext2fs_clear_bit(offset, buf);

		/* keep track of hit information to quickly lookup free blocks. */
		ss->ss_next_block = offset; 
		flag = 1;
	}
	else
	{
		printf(" ERROR: block was already cleared. ");
		assert(0);
	}
		
	nvfuse_release_bh(sb, bh, 0, DIRTY);
	nvfuse_release_bh(sb, ss_bh, 0, CLEAN);
	
	if(flag)
		nvfuse_inc_free_blocks(sb, ss->ss_seg_start + offset);

	return 0;
}

s32 nvfuse_link(struct nvfuse_superblock *sb, u32 newino, s8 *new_filename, s32 ino){	
	struct nvfuse_dir_entry *dir;
	struct nvfuse_dir_entry dir_entry;

	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode;

	struct nvfuse_buffer_head *dir_bh = NULL,*dir_bh2;		

	s32 j = 0, i = 0;
	s32 new_entry=0, flag = 0, new_alloc = 0;
	s32 search_lblock = 0, search_entry = 0;
	s32 offset;	
	s32 dir_num;
	s32 num_block;
	u32 dir_hash;
	
	if(strlen(new_filename) < 1 || strlen(new_filename) >= FNAME_SIZE)
		return error_msg("mkdir [dir name]\n");

	if (!nvfuse_lookup(sb, NULL, NULL,new_filename, newino))
	{
		printf(" file exists = %s\n", new_filename);
		return -1;		
	}

	dir_ictx = nvfuse_read_inode(sb, NULL, newino);
	dir_inode = dir_ictx->ictx_inode;

	search_lblock = dir_inode->i_ptr / DIR_ENTRY_NUM;
	search_entry = dir_inode->i_ptr % DIR_ENTRY_NUM;

	if (dir_inode->i_links_count == MAX_FILES_PER_DIR)
	{
		printf(" The number of files exceeds %d\n", MAX_FILES_PER_DIR);
		return -1;
	}

retry:

	dir_num = (dir_inode->i_size/DIR_ENTRY_SIZE);
	//if(dir_num == dir_inode.i_count){
	if (dir_num == dir_inode->i_links_count)
	{
		search_entry = -1;
		num_block = 0;
	}
	else
	{
		if(search_entry == DIR_ENTRY_NUM-1)
			search_entry = 0;
		num_block = dir_num / DIR_ENTRY_NUM;
	}
	
	for (i = 0;i < num_block;i++)
	{
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, search_lblock, READ, NVFUSE_TYPE_META);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		
		for (new_entry = 0; new_entry < DIR_ENTRY_NUM; new_entry++)
		{
			search_entry++;
			if(search_entry == DIR_ENTRY_NUM)
				search_entry = 0;
			if (nvfuse_dir_is_invalid(dir + search_entry)) {
				flag = 1;
				dir_inode->i_ptr = search_lblock * DIR_ENTRY_NUM + search_entry;				 
				dir_inode->i_links_count++;
				goto find;
			}
		}
		
		search_entry = 0;
		search_lblock++;
		if(search_lblock == dir_inode->i_size>>CLUSTER_SIZE_BITS)
			search_lblock = 0;
	}
	
	dir_num = (dir_inode->i_size/DIR_ENTRY_SIZE);
	num_block =  dir_num / DIR_ENTRY_NUM;
	search_lblock = num_block;
	
	if(!flag) // allocate new direcct block 
	{		
		new_alloc = 1;		
		nvfuse_release_bh(sb, dir_bh, 0, 0);
		dir_inode->i_size+=CLUSTER_SIZE;
		goto retry;
	}

find:	
	
	ictx = nvfuse_read_inode(sb, NULL, ino);
	inode = ictx->ictx_inode;
	inode->i_links_count++;

	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;	
	dir[search_entry].d_flag = DIR_USED;
	dir[search_entry].d_ino = ino;
	dir[search_entry].d_version = inode->i_version;
	strcpy(dir[search_entry].d_filename,new_filename);

#if NVFUSE_USE_DIR_INDEXING == 1	
	nvfuse_set_dir_indexing(sb, dir_inode, new_filename, dir_inode->i_ptr);
#endif 
		
	nvfuse_release_bh(sb, dir_bh, 0, DIRTY);

	nvfuse_release_inode(sb, dir_ictx, DIRTY);
	nvfuse_release_inode(sb, ictx, DIRTY);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
	
	return NVFUSE_SUCCESS;
}


s32 nvfuse_rm_direntry(struct nvfuse_superblock *sb, inode_t par_ino, s8 *name, u32 *ino){
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode;	

	struct nvfuse_dir_entry *dir;	
	struct nvfuse_buffer_head *dir_bh = NULL;	
	lbno_t lblock = 0;
	s64 read_bytes = 0;
	u32 start = 0;
	u32 offset = 0;
	s64 dir_size = 0;

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;
	dir_size = dir_inode->i_size;

#if NVFUSE_USE_DIR_INDEXING == 1
	if (nvfuse_get_dir_indexing(sb, dir_inode, name, &offset) < 0) {
		return -1;
	}
#endif

	start = offset* DIR_ENTRY_SIZE;
	if (start & (CLUSTER_SIZE-1))
	{
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, (u32)(start/CLUSTER_SIZE), READ, NVFUSE_TYPE_META);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir+= (offset % DIR_ENTRY_NUM);
	}

	for (read_bytes=start; read_bytes < dir_size; read_bytes+=DIR_ENTRY_SIZE)
	{
		if (!(read_bytes& (CLUSTER_SIZE-1)))
		{
			if(dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0, 0);

			lblock = read_bytes>>CLUSTER_SIZE_BITS;
			dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, (u32)(read_bytes>>CLUSTER_SIZE_BITS), READ, NVFUSE_TYPE_META);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED)
		{
			if (!strcmp(dir->d_filename,name))
			{
				ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
				inode = ictx->ictx_inode;

				if(ino)
					*ino = dir->d_ino;
				break;
			}
		}
		dir++;
	}

	if(inode->i_ino == 0)
		return NVFUSE_ERROR;
	
	/* link count decrement */
	inode->i_links_count--;

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_del_dir_indexing(sb, dir_inode, name);
#endif 
		
	dir->d_flag = DIR_DELETED;	

	nvfuse_release_bh(sb, dir_bh, 0, DIRTY);
	nvfuse_release_inode(sb, dir_ictx, DIRTY);
		
	nvfuse_release_inode(sb, ictx, DIRTY);
	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
	
	return 0;
}


u32 nvfuse_get_pbn(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t offset)
{
	struct nvfuse_inode *inode;	
	bitem_t value = 0;
	int ret;

	if (ino < ROOT_INO) 
	{
		printf(" Received invalid ino = %d", ino);
		return 0;
	}

	switch (ino) {
		case BLOCK_IO_INO: // direct translation lblk to pblk
			return offset;
		case IFILE_INO:
		{
			u32 seg_id = offset / (sb->sb_no_of_inodes_per_seg / INODE_ENTRY_NUM);
			struct nvfuse_segment_summary *ss = nvfuse_get_segment_summary(sb, seg_id);
			value = ss->ss_itable_start + (offset % ss->ss_itable_size);		
			return value;
		}
		case DBITMAP_INO:
		{
			u32 seg_id = offset;
			struct nvfuse_segment_summary *ss = nvfuse_get_segment_summary(sb, seg_id);
			value = ss->ss_dbitmap_start;
			return value;
		}
		case IBITMAP_INO:
		{
			u32 seg_id = offset;
			struct nvfuse_segment_summary *ss = nvfuse_get_segment_summary(sb, seg_id);
			value = ss->ss_ibitmap_start;
			return value;
		}		
		case SS_INO:
		{
			value = offset * NVFUSE_CLU_P_SEG(sb);
			value += NVFUSE_SUMMARY_OFFSET;
			return value;
		}
		default:;
	}
	
	assert(ictx->ictx_ino == ino);		

	ret = nvfuse_get_block(sb, ictx, offset, 1/* num block */, NULL, &value, 0);
	if (ret) 
	{
		printf(" Warning: block is not allocated.");
	}	
	
	return value;
}

s32 error_msg(s8 *str)
{
	printf("ERROR_MSG : %s \n", str);
	return NVFUSE_ERROR;
}

s32 fat_dirname(const s8 *path, s8 *dest)
{
	s8 *slash;
	strcpy(dest, path);
	slash = strrchr(dest, 0x2F); // 0x2F = "/"
	if (slash == &(dest[0])) { dest[1] = 0; return 0; } // root dir
	*slash  = 0;
	return 0;
}

s32 fat_filename(const s8 *path, s8 *dest)
{
	s8 *slash;
	slash = strrchr(path, 0x2F); // 0x2F = "/"
	if(slash == NULL){
		strcpy(dest, path);
		return 0;
	}
	slash++;
	strcpy(dest, slash);
	return 0;
}

void nvfuse_dir_hash(s8 *filename, u32 *hash1, u32 *hash2)
{
	ext2fs_dirhash(1, filename, strlen(filename), 0, hash1, hash2);
}

int nvfuse_read_block(char *buf, unsigned long block, struct nvfuse_io_manager *io_manager)
{
	return nvfuse_read_cluster(buf, block, io_manager);
}

void nvfuse_check_flush_dirty(struct nvfuse_superblock *sb, s32 force)
{
	struct list_head *dirty_head, *flushing_head;
	struct list_head *temp, *ptr;
	struct nvfuse_buffer_cache *bc;
	s32 dirty_count = 0;
	s32 flushing_count = 0;
	s32 res;

	dirty_count = nvfuse_get_dirty_count(sb);
	/* check dirty flush with force option */
	if (force != DIRTY_FLUSH_FORCE && dirty_count < NVFUSE_SYNC_DIRTY_COUNT)
		goto RES;

	/* no more dirty data */
	if (dirty_count == 0)
		goto RES;

	while ((dirty_count = nvfuse_get_dirty_count(sb)) != 0)
	{
		dirty_head = &sb->sb_bm->bm_list[BUFFER_TYPE_DIRTY];
		flushing_head = &sb->sb_bm->bm_list[BUFFER_TYPE_FLUSHING];
		flushing_count = 0;

		list_for_each_safe(ptr, temp, dirty_head) 
		{
			bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);
			assert(bc->bc_dirty);
			list_move(&bc->bc_list, flushing_head);
			flushing_count++;
			if (flushing_count >= AIO_MAX_QDEPTH)
				break;
		}

		res = nvfuse_sync_dirty_data(sb, flushing_head, flushing_count);
		if (res)
			break;
	}

	if (sb->sb_ictxc->ictxc_list_count[BUFFER_TYPE_DIRTY])
		printf(" Warning: inode dirty count = %d \n", sb->sb_ictxc->ictxc_list_count[BUFFER_TYPE_DIRTY]);
RES:;	

	return;
}

pthread_mutex_t mutex_lock;

void nvfuse_lock_init(){	
#if 1
	pthread_mutex_init(&mutex_lock, NULL);
#else
	pthread_spin_init(&mutex_lock, NULL);
#endif
}
void nvfuse_lock_exit(){
#if 1
	pthread_mutex_destroy(&mutex_lock);	
#else
	pthread_spin_destroy(&mutex_lock);
#endif
}
