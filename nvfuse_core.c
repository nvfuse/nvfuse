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

#include "nvfuse_core.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_indirect.h"
#include "nvfuse_bp_tree.h"
#include "nvfuse_config.h"
#include "nvfuse_malloc.h"
#include "nvfuse_api.h"
#include "nvfuse_dirhash.h"


static struct nvfuse_superblock *g_nvfuse_sb;
struct nvfuse_io_manager *nvfuse_io_manager;

struct timeval gstart_tv, gend_tv, gresult_tv;
s32 gtime_use = 0;

u32 CWD;
u32 ROOT_DIR;
u32 ROOT_DIR_INO;
u32 CUR_DIR_INO;
s32 NVFUSE_MOUNTED = 0;

void nvfuse_start_time(){
	assert(gtime_use == 0);
	gettimeofday(&gstart_tv, NULL);
	gtime_use = 1;
}

void nvfuse_end_time(struct timeval *p_tv){
	assert(gtime_use == 1);
	gettimeofday(&gend_tv, NULL);
	timeval_subtract(&gresult_tv, &gend_tv, &gstart_tv);
	timeval_add(p_tv, &gresult_tv);
	gtime_use = 0;
}

struct nvfuse_superblock * nvfuse_read_super(s32 rwlock, s32 is_request)
{
	struct nvfuse_superblock *sb;	
	sb = g_nvfuse_sb;
	return sb;
}

void nvfuse_release_super(struct nvfuse_superblock *sb, s32 is_update){
	if(is_update)
		gettimeofday(&g_nvfuse_sb->sb_last_update, NULL);		

	sb->sb_rwcount--;	
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

struct nvfuse_inode *nvfuse_read_inode(struct nvfuse_superblock *sb, inode_t ino, s32 read){
	struct segment_summary *ss;
	struct nvfuse_inode *inode;
	struct nvfuse_buffer_head *bh;	
	lbno_t block;
	lbno_t offset;	
	u32 seg_id;
	s32 res;

	//printf(" %s ino = %d \n", __FUNCTION__, ino);

	if(ino < ROOT_INO)
		return (struct nvfuse_inode *)NULL;

	block = ino / IFILE_ENTRY_NUM;
	offset = ino % IFILE_ENTRY_NUM;

	bh = nvfuse_get_bh(sb, IFILE_INO, block, READ);
	inode = (struct nvfuse_inode *)bh->bh_buf;
	inode += offset;	
	assert(ino == inode->i_ino);
	
	nvfuse_release_bh(sb, bh, 0, CLEAN);
RES:;

	if(inode->i_ino == 0){		
		assert(0);
		return (struct nvfuse_inode *)NULL;
	}	
	
	return inode;
}

u32 nvfuse_scan_free_ibitmap(struct nvfuse_superblock *sb, u32 seg_id, u32 hint_free_inode)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;	
	struct nvfuse_buffer_head *bh;
	void *buf;	
	u32 count = 0;
	u32 free_inode = hint_free_inode;
	u32 found = 0;

	ss_bh = nvfuse_get_bh(sb, SS_INO, seg_id, READ);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	bh = nvfuse_get_bh(sb, IBITMAP_INO, seg_id, READ);
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

u32 nvfuse_find_free_inode(struct nvfuse_superblock *sb, u32 ino)
{
	u32 new_ino = 0;
	u32 hint_ino;
	s32 ret = 0;
	u32 seg_id;
	u32 next_id;

	seg_id = ino / sb->sb_no_of_inodes_per_seg;
	next_id = seg_id % sb->sb_segment_num;
	seg_id = (seg_id - 1 + sb->sb_segment_num) % sb->sb_segment_num;

	hint_ino = ino % sb->sb_no_of_inodes_per_seg;

	while (next_id != seg_id) {
		new_ino = nvfuse_scan_free_ibitmap(sb, next_id, hint_ino);
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

	ss_bh = nvfuse_get_bh(sb, SS_INO, seg_id, READ);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	bh = nvfuse_get_bh(sb, IBITMAP_INO, seg_id, READ);
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
	ss_bh = nvfuse_get_bh(sb, SS_INO, seg_id, READ);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	ss->ss_free_inodes++;
	sb->sb_free_inodes++;
	assert(ss->ss_free_inodes <= ss->ss_max_inodes);
	nvfuse_release_bh(sb, ss_bh, 0, DIRTY);

	nvfuse_release_ibitmap(sb, seg_id, ino);
}

void nvfuse_dec_free_inodes(struct nvfuse_superblock *sb, inode_t ino)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;
	u32 seg_id;
	
	seg_id = ino / sb->sb_no_of_inodes_per_seg;
	ss_bh = nvfuse_get_bh(sb, SS_INO, seg_id, READ);
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
	ss_bh = nvfuse_get_bh(sb, SS_INO, seg_id, READ);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	ss->ss_free_blocks++;
	sb->sb_free_blocks++;

	assert(ss->ss_free_blocks <= ss->ss_max_blocks);
	assert(sb->sb_free_blocks <= sb->sb_no_of_blocks);
	nvfuse_release_bh(sb, ss_bh, 0, CLEAN);
}

void nvfuse_dec_free_blocks(struct nvfuse_superblock *sb, u32 blockno)
{
	struct nvfuse_segment_summary *ss = NULL;
	struct nvfuse_buffer_head *ss_bh;
	u32 seg_id;

	seg_id = blockno / sb->sb_no_of_blocks_per_seg;
	ss_bh = nvfuse_get_bh(sb, SS_INO, seg_id, READ);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	ss->ss_free_blocks--;
	sb->sb_free_blocks--;
	assert(ss->ss_free_blocks >= 0);
	assert(sb->sb_free_blocks >= 0);
	nvfuse_release_bh(sb, ss_bh, 0, CLEAN);
}

struct nvfuse_inode *nvfuse_alloc_new_inode(struct nvfuse_superblock *sb, inode_t par_ino){
	struct nvfuse_buffer_head *bh = NULL;	
	struct nvfuse_inode *ip = NULL;
	lbno_t search_block = 0, search_entry = 0;
	inode_t alloc_ino = 0;
	inode_t hint_ino = 0;
	inode_t last_allocated_ino = 0;
	u32 block, ifile_num;	
	s32 i, j;

	last_allocated_ino = sb->sb_last_allocated_ino;
	hint_ino = nvfuse_find_free_inode(sb, last_allocated_ino);
	if (hint_ino)
	{
		search_block = hint_ino / IFILE_ENTRY_NUM;
		search_entry = hint_ino % IFILE_ENTRY_NUM;
	}
	else
	{
		printf(" no more inodes in the file system.");
		hint_ino = nvfuse_find_free_inode(sb, par_ino);
		return NULL;
	}
	
RETRY:;
	
	bh = nvfuse_get_bh(sb, IFILE_INO, search_block, READ);
	ip = (struct nvfuse_inode *)bh->bh_buf;

	for(j = 0;j < IFILE_ENTRY_NUM;j++){
		if(ip[search_entry].i_ino == 0 && (search_entry + search_block * IFILE_ENTRY_NUM) >= NUM_RESV_INO){
			alloc_ino = search_entry + search_block * IFILE_ENTRY_NUM;		
			goto RES;
		}			
		search_entry = (search_entry + 1) % IFILE_ENTRY_NUM;
	}		

	printf(" Warning: it runs out of free inodes = %d \n", sb->sb_free_inodes);
	printf(".");
	while (1);

RES:;
	
	nvfuse_dec_free_inodes(sb, alloc_ino);

	ip += search_entry;
	ip->i_ino = alloc_ino;
	ip->i_deleted = 0; 
	ip->i_version++;		
	
	nvfuse_release_bh(sb, bh, 0, DIRTY);

	/* keep hit information to rapidly find a free inode */
	sb->sb_last_allocated_ino = alloc_ino + 1;

	return ip;
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

void nvfuse_free_inode_size(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, u64 size)
{
	struct nvfuse_buffer_head *bh;
	lbno_t offset;
	u32 num_block, trun_num_block;
	u32 deleted_bno;	
	s32 res;
	bkey_t key;

	num_block = inode->i_size >> CLUSTER_SIZE_BITS;
	trun_num_block = size >> CLUSTER_SIZE_BITS;
	if(inode->i_size & (CLUSTER_SIZE-1))
		num_block++;

	if(trun_num_block)		
		printf(" trun num block = %d\n", trun_num_block);

	if(!num_block || num_block <= trun_num_block)
		return;

	for(offset = num_block-1;offset >= trun_num_block;offset--){		
		nvfuse_make_key(inode->i_ino, offset, &key, NVFUSE_BP_TYPE_DATA);
		bh = (struct nvfuse_buffer_head *)nvfuse_hash_lookup(sb->sb_bm, key);
		if(bh){
			bh->bh_load = 0;			
			bh->bh_pno = 0;	
			bh->bh_dirty = 0;			
			nvfuse_move_buffer_type(sb, bh, BUFFER_TYPE_UNUSED, INSERT_HEAD);
		}

		if(offset == 0)
			break;	
	}

	/* truncate blocks */
	nvfuse_truncate_blocks(sb, inode, size);	
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

s32 nvfuse_set_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename, u32 offset){
	bkey_t key = 0;	
	u32 dir_hash;
	u32 collision = ~0; 	
	u32 cur_offset;
	
	master_node_t *master;

	master= bp_init_master();
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;	
	bp_read_master(master);

	collision >>= NVFUSE_BP_COLLISION_BITS;
	offset &= collision;

	nvfuse_dir_hash(filename, &dir_hash);	
	nvfuse_make_key(inode->i_ino, dir_hash, &key, NVFUSE_BP_TYPE_DIR);
	if (B_INSERT(master, &key, &offset, &cur_offset, 0) < 0) {
		u32 c = cur_offset >> (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);		
		c++;

		//printf(" file name collision = %x, %d\n", dir_hash, c);

		c <<= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
		/* if collision occurs, offset is set to 0 */
		cur_offset = 0;
		cur_offset = c | cur_offset;
		//collision
		B_UPDATE(master, &key, &cur_offset);
	}
	bp_write_master(master);
	bp_deinit_master(master);
	return 0;
}

s32 nvfuse_get_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename, bitem_t *offset){
	bkey_t key = 0;
	u32 dir_hash;		
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

	collision >>= NVFUSE_BP_COLLISION_BITS;
	nvfuse_dir_hash(filename, &dir_hash);	
	nvfuse_make_key(inode->i_ino, dir_hash, &key, NVFUSE_BP_TYPE_DIR);
	if (bp_find_key(master, &key, offset) < 0) {
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

	bp_deinit_master(master);
	return res;
}


s32 nvfuse_update_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename, bitem_t *offset) {
	bkey_t key = 0;
	u32 dir_hash;
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

	collision >>= NVFUSE_BP_COLLISION_BITS;
	nvfuse_dir_hash(filename, &dir_hash);
	nvfuse_make_key(inode->i_ino, dir_hash, &key, NVFUSE_BP_TYPE_DIR);
	if (bp_find_key(master, &key, offset) < 0) {
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

	bp_deinit_master(master);
	return res;
}

s32 nvfuse_del_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename){
	u32 dir_hash;	
	bkey_t key = 0;	
	bitem_t offset = 0;
	u32 collision = ~0; 	
	u32 c;
	master_node_t *master = NULL;

	master = bp_init_master();
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;
	bp_read_master(master);

	collision >>= NVFUSE_BP_COLLISION_BITS;
	nvfuse_dir_hash(filename, &dir_hash);	
	nvfuse_make_key(inode->i_ino, dir_hash, &key, NVFUSE_BP_TYPE_DIR);	
	if(bp_find_key(master, &key, &offset) < 0){
		printf(" del error = %x\n", dir_hash);
		return -1;
	}
		
	c = offset;
	c >>= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
	if(c == 0)
		B_REMOVE(master, &key);	
	else{
		c--;
		c <<= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
		offset &= collision;
		offset = c | offset;
		//collision
		B_UPDATE(master, &key, &offset);
	}

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

void io_cancel_incomplete_ios(struct io_job *jobq, int job_cnt){
    struct io_job *job;
    int i;

    for(i = 0;i < job_cnt;i++){
        job = jobq + i;

        if(job && job->complete)
            continue;

        nvfuse_aio_cancel(job, nvfuse_io_manager);
    }
}

s32 nvfuse_wait_aio_completion(struct io_job *jobq, int job_cnt)
{
	struct io_job *job;
	unsigned long blkno;
	unsigned int bcount;
	int cc = 0; // completion count

	nvfuse_aio_resetnextcjob(nvfuse_io_manager);
	cc = nvfuse_aio_complete(nvfuse_io_manager);
	nvfuse_io_manager->queue_cur_count -= cc;

	//printf(" tid = %d cc = %d \n", th_p->tid, cc);
	while (cc--) {
		job = nvfuse_aio_getnextcjob(nvfuse_io_manager);

		if(job->ret!=job->bytes){
			printf(" IO error \n");
		}

		job->complete = 1;
	}

	// TODO: io cancel 
	io_cancel_incomplete_ios(jobq, job_cnt);

	return 0;
}


s32 nvfuse_sync_dirty_data(struct nvfuse_superblock *sb){
	struct list_head *head = &sb->sb_bm->bm_list[BUFFER_TYPE_DIRTY];
	struct list_head *ptr, *temp;
	struct nvfuse_buffer_head *bh;	
	s32 res = 0;

#ifdef USE_AIO_WRITE
	struct io_job *jobs;
	struct iocb **iocb;
	s32 count = 0;
	s32 num_dirty = sb->sb_bm->bm_list_count[BUFFER_TYPE_DIRTY];
	res = nvfuse_make_jobs(&jobs, num_dirty);
	if (res < 0) {
		return res;
	}

	iocb = (struct iocb **)malloc(sizeof(struct iocb *) * num_dirty);
	if (!iocb) {
		printf(" Malloc error: struct iocb\n");
		return -1;
	}

	list_for_each_safe(ptr, temp, head) {
		bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_list);
		assert(bh->bh_dirty);

		(*(jobs + count)).offset = (long)bh->bh_pno * CLUSTER_SIZE;
		(*(jobs + count)).bytes = (size_t)CLUSTER_SIZE;
		(*(jobs + count)).ret = 0;
		(*(jobs + count)).req_type = WRITE;
		(*(jobs + count)).buf = bh->bh_buf;
		(*(jobs + count)).complete = 0;
		iocb[count] = &((*(jobs + count)).iocb);
		count++;

	}

	count = 0;
	while(count < num_dirty)
	{
		nvfuse_aio_prep(jobs + count, nvfuse_io_manager);
		count ++;
	}

	nvfuse_aio_submit(iocb, num_dirty, nvfuse_io_manager);
	nvfuse_io_manager->queue_cur_count = num_dirty;

	nvfuse_wait_aio_completion(jobs, num_dirty);

	free(jobs);
#else
	list_for_each_safe(ptr, temp, head) {
		bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_list);
		assert(bh->bh_dirty);
		nvfuse_write_cluster(bh->bh_buf, bh->bh_pno, nvfuse_io_manager);
	}
#endif

	list_for_each_safe(ptr, temp, head) {
		bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_list);
		assert(bh->bh_dirty);
		bh->bh_dirty = 0;
		nvfuse_move_buffer_type(sb, bh, BUFFER_TYPE_CLEAN, INSERT_HEAD);
	}
	
	return 0;
}

s32 nvfuse_sync(struct nvfuse_superblock *sb){	
	
	nvfuse_sync_dirty_data(sb);

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
	timeval_add(&total_tv2, &sb->sb_time_segwrite);
	timeval_add(&total_tv2, &sb->sb_time_ssr);
	timeval_add(&total_tv2, &sb->sb_time_cleaning);
	timeval_add(&total_tv2, &sb->sb_time_checkpoint);
	timeval_subtract(&other_tv, &total_tv, &total_tv2);

	fprintf(fp," Total Execution Time = %06ld.%06ld\n", 
			sb->sb_time_total.tv_sec, sb->sb_time_total.tv_usec);
	fprintf(fp," Total Other Time = %06ld.%06ld\n", 
			other_tv.tv_sec, other_tv.tv_usec);
	fprintf(fp," Total Segwrite Time = %06ld.%06ld\n", 
			sb->sb_time_segwrite.tv_sec, sb->sb_time_segwrite.tv_usec);
	fprintf(fp," Total SSRwrite Time = %06ld.%06ld\n", 
			sb->sb_time_ssr.tv_sec, sb->sb_time_ssr.tv_usec);
	fprintf(fp," Total Cleaning Time = %06ld.%06ld\n", 
			sb->sb_time_cleaning.tv_sec, sb->sb_time_cleaning.tv_usec);
	fprintf(fp," Total Checkpoint Time = %06ld.%06ld\n", 
			sb->sb_time_checkpoint.tv_sec, sb->sb_time_checkpoint.tv_usec);
	fprintf(fp,"\n");
		
	fprintf(fp," \n\n");

}

void nvfuse_write_statistics(struct nvfuse_superblock *sb){	
	s8 out_str[128];
	s8 dev_str[128];
	s32 i;
	FILE *fp;

	if (nvfuse_io_manager->dev_path != NULL && nvfuse_io_manager->dev_path[0] == '/') {		
		strcpy(dev_str, nvfuse_io_manager->dev_path);

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

s32 nvfuse_mount() {
	struct nvfuse_superblock *sb;
	struct nvfuse_buffer_list *bh;
	struct nvfuse_superblock *nvfuse_sb_disk;
	struct nvfuse_segment_buffer *seg_buf;
	struct nvfuse_buffer_manager *bm;
	struct timeval start_tv, end_tv, result_tv;
	void *buf;
	s32 i, j, res = 0;
	
	nvfuse_lock_init();

	g_nvfuse_sb = (struct nvfuse_superblock *)nvfuse_malloc(sizeof(struct nvfuse_superblock));
	if (g_nvfuse_sb == NULL) {
		printf(" nvfuse_malloc error \n");
		return -1;
	}
	memset(g_nvfuse_sb, 0x00, sizeof(struct nvfuse_superblock));

	bm = (struct nvfuse_buffer_manager *)nvfuse_malloc(sizeof(struct nvfuse_buffer_manager));
	if (bm == NULL) {
		printf(" nvfuse_malloc error \n");
		return -1;
	}
	memset(bm, 0x00, sizeof(struct nvfuse_buffer_manager));
	g_nvfuse_sb->sb_bm = bm;

	res = nvfuse_init_buffer_cache(g_nvfuse_sb);
	if (res < 0)
	{
		printf(" Error: initialization of buffer cache \n");
		return -1;
	}

	if (NVFUSE_MOUNTED)
		return -1;

	g_nvfuse_sb->sb_file_table = (struct nvfuse_file_table *)nvfuse_malloc(sizeof(struct nvfuse_file_table) * MAX_OPEN_FILE);
	if (g_nvfuse_sb->sb_file_table == NULL) {
		printf(" nvfuse_malloc error \n");
		return -1;
	}
	memset(g_nvfuse_sb->sb_file_table, 0x00, sizeof(struct nvfuse_file_table) * MAX_OPEN_FILE);
	for (i = 0; i < MAX_OPEN_FILE; i++) {
		struct nvfuse_file_table *ft = &g_nvfuse_sb->sb_file_table[i];
		pthread_mutex_init(&ft->ft_lock, NULL);
	}

	sb = g_nvfuse_sb;
	pthread_mutex_init(&sb->sb_iolock, NULL);
	pthread_mutex_init(&g_nvfuse_sb->sb_file_table_lock, NULL);
	pthread_mutex_init(&g_nvfuse_sb->sb_prefetch_lock, NULL);
	pthread_cond_init(&g_nvfuse_sb->sb_prefetch_cond, NULL);
	pthread_rwlock_init(&sb->sb_rwlock, NULL);	
	pthread_mutex_init(&sb->sb_request_lock, NULL);

	gettimeofday(&start_tv, NULL);

	res = nvfuse_scan_superblock();
	if (res < 0) {
		printf(" invalid signature !!\n");
		return res;
	}

	gettimeofday(&end_tv, NULL);

	timeval_subtract(&result_tv, &end_tv, &start_tv);
	printf("\n scan time %.3d second\n", (s32)((float)result_tv.tv_sec + (float)result_tv.tv_usec / (float)1000000));
	gettimeofday(&g_nvfuse_sb->sb_last_update, NULL);

	ROOT_DIR_INO = CUR_DIR_INO = g_nvfuse_sb->sb_root_ino;

	gettimeofday(&g_nvfuse_sb->sb_sync_time, NULL);
		
	if (!g_nvfuse_sb->sb_umount) {
		g_nvfuse_sb->sb_cur_segment = 0;
		g_nvfuse_sb->sb_next_segment = 0;
		nvfuse_sync(g_nvfuse_sb);
	}
	else {
		g_nvfuse_sb->sb_umount = 0;
	}

	g_nvfuse_sb->sb_ss = (struct nvfuse_segment_summary *)nvfuse_malloc(sizeof(struct nvfuse_segment_summary) * g_nvfuse_sb->sb_segment_num);
	if (g_nvfuse_sb->sb_ss == NULL) {
		printf("nvfuse_malloc error = %d\n", __LINE__);
		return -1;
	}
	memset(g_nvfuse_sb->sb_ss, 0x00, sizeof(struct nvfuse_segment_summary) * g_nvfuse_sb->sb_segment_num);

	buf = nvfuse_malloc(CLUSTER_SIZE);
	if (buf == NULL)
	{
		printf(" malloc error \n");
		return NVFUSE_ERROR;
	}

	// load segment summary in memory
	for (i = 0; i < g_nvfuse_sb->sb_segment_num; i++)
	{
		u32 cno = NVFUSE_SUMMARY_OFFSET + i * g_nvfuse_sb->sb_no_of_blocks_per_seg;
		nvfuse_read_cluster(buf, cno, nvfuse_io_manager);
		memcpy(g_nvfuse_sb->sb_ss + i, buf, sizeof(struct nvfuse_segment_summary));
		//printf("seg %d ibitmap start = %d \n", i, g_nvfuse_sb->sb_ss[i].ss_ibitmap_start);
	}
	nvfuse_free(buf);
	/* create b+tree index for root directory at first mount after formattming */
	if (g_nvfuse_sb->sb_mount_cnt == 0)
	{
		struct nvfuse_inode *root_inode;

		/* read root inode */
		root_inode = nvfuse_read_inode(g_nvfuse_sb, g_nvfuse_sb->sb_root_ino, READ);

		/* create bptree related nodes */
		nvfuse_create_bptree(sb, root_inode);

		/* mark dirty and copy */
		nvfuse_relocate_write_inode(g_nvfuse_sb, root_inode, root_inode->i_ino, DIRTY);

		/* sync dirty data to storage medium */
		nvfuse_check_flush_segment(g_nvfuse_sb);
	}

	NVFUSE_MOUNTED = 1;

	gettimeofday(&sb->sb_time_start, NULL);

	printf(" NVFUSEE has been successfully mounted. \n");

	return NVFUSE_SUCCESS;
}

s32 nvfuse_umount()
{
	struct nvfuse_superblock *sb = nvfuse_read_super(READ, 0);
	s32 i;
	s8 buf[CLUSTER_SIZE];
	
	if(!NVFUSE_MOUNTED)
		return -1;

	gettimeofday(&sb->sb_time_end, NULL);
	timeval_subtract(&sb->sb_time_total, &sb->sb_time_end, &sb->sb_time_start);

	nvfuse_trace_close();

	nvfuse_sync(sb);

	nvfuse_copy_mem_sb_to_disk_sb((struct nvfuse_superblock *)buf, g_nvfuse_sb);
	nvfuse_write_cluster(buf, INIT_NVFUSE_SUPERBLOCK_NO, nvfuse_io_manager);

	nvfuse_free_buffer_cache(sb);

	pthread_rwlock_destroy(&sb->sb_rwlock);
	pthread_mutex_destroy(&sb->sb_iolock);
	pthread_mutex_destroy(&sb->sb_request_lock);
	
	pthread_mutex_destroy(&sb->sb_file_table_lock);
	pthread_mutex_destroy(&sb->sb_prefetch_lock);
	pthread_cond_destroy(&sb->sb_prefetch_cond);

	for(i = 0;i < MAX_OPEN_FILE;i++){
		struct nvfuse_file_table *ft = &g_nvfuse_sb->sb_file_table[i];
		pthread_mutex_destroy(&ft->ft_lock);
	}
		
	free(sb->sb_ss_nfinfo);
	free(sb->sb_ss);
	free(sb->sb_sb);
	free(sb->sb_bm);
	free(sb->sb_file_table);
	free(sb);
	
	nvfuse_lock_exit();

	NVFUSE_MOUNTED = 0;

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

s32 nvfuse_scan_superblock()
{
	s32 res = -1, i;
	s8 *buf;
	u64 num_clu, num_sectors, num_seg;
	pbno_t cno;	
	struct nvfuse_superblock *sb;
	
#if NVFUSE_OS == NVFUSE_OS_WINDOWS
	num_sectors = NO_OF_SECTORS;
	num_clu = (u32)NVFUSE_NUM_CLU;
#else
#	ifdef __USE_FUSE__
	num_sectors = get_no_of_sectors(nvfuse_io_manager->dev);
	num_clu = (u32)num_sectors / (u32)SECTORS_PER_CLUSTER;
#	else
#		if USE_RAMDISK == 1 || USE_FILEDISK == 1
		num_sectors = NO_OF_SECTORS;
		num_clu = (u32)NVFUSE_NUM_CLU;
#		elif USE_UNIX_IO == 1
		num_sectors = get_no_of_sectors(nvfuse_io_manager->dev);
		num_clu = num_sectors / (u32)SECTORS_PER_CLUSTER;
#		elif USE_SPDK == 1
		num_sectors = nvfuse_io_manager->total_blkcount;
		num_clu = num_sectors/SECTORS_PER_CLUSTER;
#		endif
#	endif
	num_sectors = nvfuse_io_manager->total_blkcount;
	num_clu = num_sectors/SECTORS_PER_CLUSTER;
	/* FIXME: total_blkcount must be set to when io_manager is initialized. */
#endif

	printf(" sectors = %lu, blocks = %lu\n", (unsigned long)num_sectors, (unsigned long)num_clu);

	num_seg = NVFUSE_SEG_NUM(num_clu, NVFUSE_SEGMENT_SIZE_BITS-CLUSTER_SIZE_BITS);
	num_clu = num_seg << (NVFUSE_SEGMENT_SIZE_BITS-CLUSTER_SIZE_BITS);// (NVFUSE_SEGMENT_SIZE/NVFUSE_CLUSTER_SIZE);

	buf = (s8 *)nvfuse_malloc(CLUSTER_SIZE);
	if(buf == NULL)	{
		printf(" nvfuse_malloc error \n");
	}
		
	memset(buf, 0x00, CLUSTER_SIZE);
	cno = INIT_NVFUSE_SUPERBLOCK_NO;

	nvfuse_read_cluster(buf, cno, nvfuse_io_manager);
	sb = (struct nvfuse_superblock *)buf;
	
	if(sb->sb_signature == NVFUSE_SB_SIGNATURE){
		nvfuse_copy_disk_sb_to_sb(g_nvfuse_sb, sb);
		res = 0;
	}else{
		printf(" super block signature is mismatched. \n");
		res = -1;
	}

	printf(" root ino = %d \n", sb->sb_root_ino);
	printf(" no of sectors = %ld \n", (unsigned long)sb->sb_no_of_sectors);
	printf(" no of blocks = %ld \n", (unsigned long)sb->sb_no_of_blocks);
	printf(" no of used blocks = %ld \n", (unsigned long)sb->sb_no_of_used_blocks);
	printf(" no of inodes per seg = %d \n", sb->sb_no_of_inodes_per_seg);
	printf(" no of blocks per seg = %d \n", sb->sb_no_of_blocks_per_seg);
	printf(" no of free inodes = %d \n", sb->sb_free_inodes);
	printf(" no of free blocks = %ld \n", (unsigned long)sb->sb_free_blocks);
	nvfuse_free(buf);
	return res;
}

u32 nvfuse_create_bptree(struct nvfuse_superblock *sb, struct nvfuse_inode *inode)
{
	master_node_t *master;

	/* make b+tree master and root nodes */
	master = bp_init_master();
	bp_alloc_master(sb, master);
	bp_init_root(master);
	bp_write_master(master);

	/* update bptree inode */
	inode->i_bpino = master->m_ino;
	/* deinit b+tree memory structure */
	bp_deinit_master(master);

	return 0;
}

s32 nvfuse_dir(){
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_dir_entry *dir;	
	struct nvfuse_superblock *sb = nvfuse_read_super(READ, 1);
	s32 read_bytes;
	s32 dir_size;	

	dir_inode = nvfuse_read_inode(sb, CUR_DIR_INO, READ);

	dir_size = dir_inode->i_size;
	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, CLEAN);

	for(read_bytes=0;read_bytes < dir_size;read_bytes+=DIR_ENTRY_SIZE){

		if(!(read_bytes& (CLUSTER_SIZE-1))){
			if(dir_bh != NULL){
				nvfuse_release_bh(sb, dir_bh, 0, 0);
				dir_bh = NULL;
			}
			dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, read_bytes >>CLUSTER_SIZE_BITS, READ);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if(dir->d_flag == DIR_USED){
			printf(" read inode ... \n");
			inode = nvfuse_read_inode(sb, dir->d_ino, READ);
			nvfuse_print_inode(inode, dir->d_filename);
			nvfuse_relocate_write_inode(sb, inode, inode->i_ino,CLEAN);
		}
		dir++;

	}

	nvfuse_release_bh(sb, dir_bh, 0, 0);
	nvfuse_release_super(sb, 0);

	//nvfuse_relocate_write_inode(sb, inode, inode->i_ino,0);
	printf("\n");
#if 1
	printf("free blocks	  = %ld, num  blocks  = %ld\n", (unsigned long)sb->sb_free_blocks, (unsigned long)sb->sb_no_of_blocks);
	printf("Disk Util     = %2.2f %%\n", ((double)(sb->sb_no_of_blocks - sb->sb_free_blocks) / (double)sb->sb_no_of_blocks) * 100);
#endif

	//nvfuse_check_buf();

	return NVFUSE_SUCCESS;
}


s32 nvfuse_allocate_open_file_table(struct nvfuse_superblock *sb)
{
	s32 i = 0, fid = -1;

	pthread_mutex_lock(&sb->sb_file_table_lock);
	for (i = 0; i < MAX_OPEN_FILE; i++){
		if(sb->sb_file_table[i].used == FALSE){
			fid = i;
			break;
		}
	}
	pthread_mutex_unlock(&sb->sb_file_table_lock);

	return fid;
}

s32 nvfuse_truncate(inode_t par_ino, s8 *filename, nvfuse_off_t trunc_size)
{
	struct nvfuse_inode *dir_inode, *inode = NULL;
	struct nvfuse_dir_entry *dir;
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_superblock *sb = nvfuse_read_super(READ, 1);
	u32 read_bytes = 0;
	lbno_t lblock = 0;
	u32 start = 0;
	u32 offset = 0;
	u64 dir_size = 0;
	u32 found_entry;

	dir_inode = nvfuse_read_inode(sb, par_ino, WRITE);
	dir_size = dir_inode->i_size;

	if (!strcmp(filename, "9999")) {
		dir_size = dir_inode->i_size;
	}
#if NVFUSE_USE_DIR_INDEXING == 1
	if (nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset) < 0) {
		printf(" fixme: filename (%s) is not in the index.\n", filename);
		offset = 0;
	}
#endif

	start = offset * DIR_ENTRY_SIZE;
	if ((start & (CLUSTER_SIZE - 1))) {
		dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, start / CLUSTER_SIZE, READ);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir += (offset % DIR_ENTRY_NUM);
	}

	for (read_bytes = start; read_bytes < dir_size; read_bytes += DIR_ENTRY_SIZE) {
		if (!(read_bytes & (CLUSTER_SIZE - 1))) {
			if (dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0/*tail*/, 0/*dirty*/);
			lblock = read_bytes >> CLUSTER_SIZE_BITS;
			dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, read_bytes >> CLUSTER_SIZE_BITS, READ);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED) {
			if (!strcmp(dir->d_filename, filename)) {
				inode = nvfuse_read_inode(sb, dir->d_ino, WRITE);
				found_entry = read_bytes / DIR_ENTRY_SIZE;
				break;
			}
		}
		dir++;
	}

	if (inode == NULL || inode->i_ino == 0) {
		printf(" file (%s) is not found this directory\n", filename);
		nvfuse_release_bh(sb, dir_bh, 0/*tail*/, CLEAN);
		return NVFUSE_ERROR;
	}

	if (inode->i_type == NVFUSE_TYPE_DIRECTORY) {
		return error_msg(" rmfile() is supported for a file.");
	}

	nvfuse_free_inode_size(sb, inode, trunc_size);
	inode->i_size = trunc_size;
	nvfuse_relocate_write_inode(sb, inode, inode->i_ino, 1/*dirty*/);

	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, CLEAN/*dirty*/);
	nvfuse_release_bh(sb, dir_bh, 0/*tail*/, CLEAN);

	nvfuse_check_flush_segment(sb);
	nvfuse_release_super(sb, 1/*last update time*/);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_truncate_ino(struct nvfuse_superblock *sb, inode_t ino, u64 trunc_size){	
	struct nvfuse_inode *inode;
	
	inode = nvfuse_read_inode(sb, ino, WRITE);
	nvfuse_free_inode_size(sb, inode, trunc_size);
	nvfuse_relocate_write_inode(sb, inode, inode->i_ino,1);	

	return NVFUSE_SUCCESS;
}

s32 nvfuse_chmod(inode_t par_ino, s8 *filename, mode_t mode){
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_dir_entry *dir;	
	struct nvfuse_buffer_head *dir_bh = NULL;	
	struct nvfuse_superblock *sb = nvfuse_read_super(READ, 1);
	lbno_t lblock = 0;
	u32 start = 0;
	u32 read_bytes = 0;
	u32 offset = 0;
	s32 mask;
	u64 dir_size = 0;

	//memset(&inode, 0x00, INODE_ENTRY_SIZE);
	//memset(&dir_inode, 0x00, INODE_ENTRY_SIZE);

	dir_inode = nvfuse_read_inode(sb, par_ino, WRITE);
	
#if NVFUSE_USE_DIR_INDEXING == 1
	if(nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset) < 0)
		printf(" debug \n");
#endif

	dir_size = dir_inode->i_size;
	start = offset * DIR_ENTRY_SIZE;

	if((start&(CLUSTER_SIZE-1))){
		dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, start/CLUSTER_SIZE, READ);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir+= (offset % DIR_ENTRY_NUM);
	}


	for(read_bytes=start;read_bytes < dir_size;read_bytes+=DIR_ENTRY_SIZE){

		if(!(read_bytes& (CLUSTER_SIZE-1))){
			if(dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0, 0);

			lblock = read_bytes>>CLUSTER_SIZE_BITS;
			dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, read_bytes>>CLUSTER_SIZE_BITS, READ);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if(dir->d_flag == DIR_USED){
			if(!strcmp(dir->d_filename,filename)){
				inode = nvfuse_read_inode(sb, dir->d_ino, WRITE);
				break;
			}
		}
		dir++;
	}

	if(inode->i_ino == 0)
		return NVFUSE_ERROR;
	
	mask = S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
	inode->i_mode = (inode->i_mode & ~mask) | (mode & mask);

	nvfuse_relocate_write_inode(sb, inode, inode->i_ino,1);
	nvfuse_release_bh(sb, dir_bh, 0, CLEAN);
	
	nvfuse_check_flush_segment(sb);

	nvfuse_release_super(sb, 1);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_path_open(s8 *path, s8 *filename, struct nvfuse_dir_entry *get){
	struct nvfuse_inode *inode;
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_superblock *sb = nvfuse_read_super(READ, 1);
	s8 *token;
	s8 b[256];
	u32 local_dir_ino;
	s32 i;
	s32 count=0;
	

	strcpy(b,path);
	i = strlen(b);

	if(path[0] == '/')
		local_dir_ino = ROOT_INO;
	else
		local_dir_ino = CUR_DIR_INO;

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
			inode = nvfuse_read_inode(sb, get->d_ino, READ);
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

	nvfuse_release_super(sb, 0);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_path_open2(s8 *path, s8 *filename, struct nvfuse_dir_entry *get){
	struct nvfuse_superblock *sb = nvfuse_read_super(READ, 1);
	struct nvfuse_inode inode;
	struct nvfuse_dir_entry dir_entry;
	s8 *token;
	s8 b[256];
	u32 local_dir_ino;
	s32 i;
	s32 count=0;
	s32 res = 0;

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
		local_dir_ino = ROOT_INO;
	else
		local_dir_ino = CUR_DIR_INO;

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

	nvfuse_release_super(sb, 1);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_lseek(s32 fd, u32 offset, s32 position)
{
	struct nvfuse_file_table *of;

	of = &g_nvfuse_sb->sb_file_table[fd];

	if (position == SEEK_SET)             /* SEEK_SET */
		of->rwoffset = offset;
	else if (position == SEEK_CUR)        /* SEEK_CUR */
		of->rwoffset += offset;
	else if (position == SEEK_END)        /* SEEK_END */
		of->rwoffset = of->size - offset;

	return(NVFUSE_SUCCESS);
}



s32 nvfuse_seek(struct nvfuse_superblock *sb, struct nvfuse_file_table *of, u32 offset, s32 position)
{
	//of = &sb->sb_file_table[fd];

	if (position == SEEK_SET)             /* SEEK_SET */
		of->rwoffset = offset;
	else if (position == SEEK_CUR)        /* SEEK_CUR */
		of->rwoffset += offset;
	else if (position == SEEK_END)        /* SEEK_END */
		of->rwoffset = of->size - offset;

	return(NVFUSE_SUCCESS);
}


void nvfuse_relocate_write_inode(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, inode_t ino, s32 dirty){
	struct nvfuse_inode ifile_inode;
	struct nvfuse_inode *ip;
	struct nvfuse_buffer_head *bh;	
	u32 offset, block;		

	if(inode == NULL)
		return;
	if(ino == 0)
		return;
	if(!dirty)
		goto RES;

	block = ino / IFILE_ENTRY_NUM;
	offset = ino % IFILE_ENTRY_NUM;

	//ifile_inode.i_ino = IFILE_INO;
	bh = nvfuse_get_bh(sb, IFILE_INO, block, READ);
	ip = (struct nvfuse_inode *)bh->bh_buf;
	ip += offset;
	memcpy(ip, inode, INODE_ENTRY_SIZE);	
		
	nvfuse_release_bh(sb, bh, 0/*head*/, dirty);
	
RES:;	
}


s32 nvfuse_relocate_delete_inode(struct nvfuse_superblock *sb,struct nvfuse_inode *inode){
	inode_t ino = inode->i_ino;	
	inode->i_deleted = 1;
	inode->i_ino = 0;	
	inode->i_size = 0;

	//printf(" delete ino = %d ", ino);
	nvfuse_relocate_write_inode(sb, inode, ino, 1);	
	nvfuse_inc_free_inodes(sb, ino);

	return 0;
}

u32 nvfuse_alloc_dbitmap(struct nvfuse_superblock *sb, u32 seg_id)
{
	struct nvfuse_segment_summary *ss;
	struct nvfuse_buffer_head *ss_bh, *bh;
	u32 free_block = 0;
	u32 cnt = 0;
	void *buf;
	u32 flag = 0;

	ss_bh = nvfuse_get_bh(sb, SS_INO, seg_id, READ);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;

	bh = nvfuse_get_bh(sb, DBITMAP_INO, seg_id, READ);
	buf = bh->bh_buf;
	
	free_block = ss->ss_next_block % sb->sb_no_of_blocks_per_seg;

	while (cnt++ < sb->sb_no_of_blocks_per_seg)
	{
		if (!ext2fs_test_bit(free_block, buf))
		{
			//printf(" free block %d found \n", free_block);
			ss->ss_next_block = free_block; // keep track of hit information to quickly lookup free blocks.
			ext2fs_set_bit(free_block, buf); // right approach?
			flag = 1;
			break;
		}
		free_block = (free_block + 1) % sb->sb_no_of_blocks_per_seg;
	}
	
	
	if (flag) {
		nvfuse_release_bh(sb, bh, 0, DIRTY);
		nvfuse_release_bh(sb, ss_bh, 0, CLEAN);
		nvfuse_dec_free_blocks(sb, ss->ss_seg_start + free_block);
		return ss->ss_seg_start + free_block;
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

	ss_bh = nvfuse_get_bh(sb, SS_INO, seg_id, READ);
	ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;
	assert(ss->ss_id == seg_id);

	bh = nvfuse_get_bh(sb, DBITMAP_INO, seg_id, READ);
	buf = bh->bh_buf;

	//free_block = ss->ss_next_block % sb->sb_no_of_blocks_per_seg;
	//printf(" Clear dbitmap seg id = %d, offset = %d\n", seg_id, offset);
	if (ext2fs_test_bit(offset, buf))
	{
		ext2fs_clear_bit(offset, buf);
		ss->ss_next_block = offset; // keep track of hit information to quickly lookup free blocks.				
		flag = 1;
	}
	else
	{
		printf(" block is not in use (e.g., free block).");		
	}
		
	nvfuse_release_bh(sb, bh, 0, DIRTY);
	nvfuse_release_bh(sb, ss_bh, 0, CLEAN);
	
	if(flag)
		nvfuse_inc_free_blocks(sb, ss->ss_seg_start + offset);

	return 0;
}

s32 nvfuse_link(u32 oldino, s8 *old_filename, u32 newino, s8 *new_filename, s32 ino){
	struct nvfuse_dir_entry *dir;
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_buffer_head *dir_bh = NULL,*dir_bh2;	
	struct nvfuse_superblock *sb = nvfuse_read_super(READ, 1);
	s32 j = 0, i = 0;
	s32 new_entry=0, flag = 0, new_alloc = 0;
	s32 search_lblock = 0, search_entry = 0;
	s32 offset;	
	s32 dir_num;
	s32 num_block;
	u32 dir_hash;
	
	if(strlen(new_filename) < 1 || strlen(new_filename) >= FNAME_SIZE)
		return error_msg("mkdir [dir name]\n");

	if(!nvfuse_lookup(sb, NULL,NULL,new_filename, newino)){		
		printf(" file exists = %s\n", new_filename);
		return -1;		
	}

	dir_inode = nvfuse_read_inode(sb, newino, WRITE);

	search_lblock = dir_inode->i_ptr / DIR_ENTRY_NUM;
	search_entry = dir_inode->i_ptr % DIR_ENTRY_NUM;

retry:

	dir_num = (dir_inode->i_size/DIR_ENTRY_SIZE);
	//if(dir_num == dir_inode.i_count){
	if(dir_num == dir_inode->i_links_count){
		search_entry = -1;
		num_block = 0;
	}else{
		if(search_entry == DIR_ENTRY_NUM-1)
			search_entry = 0;
		num_block = dir_num / DIR_ENTRY_NUM;
	}
	
	for(i = 0;i < num_block;i++){
		dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, search_lblock, READ);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		
		for(new_entry = 0;new_entry < DIR_ENTRY_NUM;new_entry++){
			search_entry++;
			if(search_entry == DIR_ENTRY_NUM)
				search_entry = 0;
			if(dir[search_entry].d_flag == DIR_EMPTY){				
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
	
	inode = nvfuse_read_inode(sb, ino,  WRITE);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;	
	dir[search_entry].d_flag = DIR_USED;
	dir[search_entry].d_ino = ino;
	dir[search_entry].d_version = inode->i_version;
	strcpy(dir[search_entry].d_filename,new_filename);

#if NVFUSE_USE_DIR_INDEXING == 1	
	nvfuse_set_dir_indexing(sb, dir_inode, new_filename, dir_inode->i_ptr);
#endif 
	
	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino,1);
	nvfuse_release_bh(sb, dir_bh, 0, 1);
	nvfuse_relocate_write_inode(sb, inode, inode->i_ino,1);

	nvfuse_check_flush_segment(sb);
	nvfuse_release_super(sb, 1);
	
	return NVFUSE_SUCCESS;
}


s32 nvfuse_rm_direntry(inode_t par_ino, s8 *name, u32 *ino){
	struct nvfuse_inode *dir_inode, *inode;	
	struct nvfuse_dir_entry *dir;	
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_superblock *sb = nvfuse_read_super(READ, 1);
	lbno_t lblock = 0;
	u32 read_bytes = 0;
	u32 start = 0;
	u32 offset = 0;
	u64 dir_size = 0;

	
	dir_inode = nvfuse_read_inode(sb, par_ino, WRITE);
	dir_size = dir_inode->i_size;

#if NVFUSE_USE_DIR_INDEXING == 1
	if(nvfuse_get_dir_indexing(sb, dir_inode, name, &offset) < 0)
		printf(" debug \n");
#endif

	start = offset* DIR_ENTRY_SIZE;
	if((start&(CLUSTER_SIZE-1))){
		dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, start/CLUSTER_SIZE, READ);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir+= (offset % DIR_ENTRY_NUM);
	}

	for(read_bytes=start;read_bytes < dir_size;read_bytes+=DIR_ENTRY_SIZE){

		if(!(read_bytes& (CLUSTER_SIZE-1))){
			if(dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0, 0);

			lblock = read_bytes>>CLUSTER_SIZE_BITS;
			dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, read_bytes>>CLUSTER_SIZE_BITS, READ);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if(dir->d_flag == DIR_USED){
			if(!strcmp(dir->d_filename,name)){
				inode = nvfuse_read_inode(sb, dir->d_ino, WRITE);
				if(ino)
					*ino = dir->d_ino;
				break;
			}
		}
		dir++;
	}

	if(inode->i_ino == 0)
		return NVFUSE_ERROR;


#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_del_dir_indexing(sb, dir_inode, name);
#endif 

	dir->d_flag = DIR_DELETED;	
	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, 1);
	nvfuse_release_bh(sb, dir_bh, 0, 1);
	
	nvfuse_relocate_write_inode(sb, inode, inode->i_ino,1);	
	nvfuse_check_flush_segment(sb);
	nvfuse_release_super(sb, 1);
	
	//nvfuse_sync(0, 0);
	//nvfuse_check_ondemand_cleaning();

	return 0;
}


u32 nvfuse_get_pbn(struct nvfuse_superblock *sb, inode_t ino, lbno_t offset){
	struct nvfuse_inode *inode;
	bkey_t key = 0;
	bitem_t value = 0;
	int ret;

	if (ino < ROOT_INO) {
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
			//printf(" itable start = %d \n", ss->ss_itable_start);
			//printf("loffset = %d, poffset = %d \n", offset, value);
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
	
	inode = nvfuse_read_inode(sb, ino, READ);
	ret = nvfuse_get_block(sb, inode, offset, &value, 0);
	if (ret) {
		printf(" Warning: block is not allocated.");
	}	
	
	//printf(" ino = %d offset = %d value = %d \n", ino, offset, value);	
	
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

__inline static u32 *nvfuse_dir_hash(s8 *filename, u32 *hash){
	ext2fs_dirhash(1,filename,strlen(filename), 0, hash, 0);
	return hash;
}

int nvfuse_read_block(char *buf, unsigned long block, struct nvfuse_io_manager *io_manager){
	return nvfuse_read_cluster(buf, block, io_manager);
}

FILE *trace_fp = NULL;
u64 trace_no = 0;

int nvfuse_trace_open(s8 *trace_file){
	s8 str[128];
	s32 util;
	
	if(strlen(trace_file) <= 1)
		return 0;
	
	trace_fp = fopen(trace_file, "w");
	if(trace_fp == NULL){
		printf(" cannot open file %s\n", trace_file);
	}

	printf(" Open trace file %s\n", trace_file);
	
	return 0;
}


int nvfuse_trace_write(u32 block, u32 num,u32 is_read){

	if(trace_fp==NULL)
		return -1;
	
	if(is_read)
		fprintf(trace_fp, "%lu %u %u %c\n", (long)trace_no++, block, num, 'R');
	else
		fprintf(trace_fp, "%lu %u %u %c\n", (long)trace_no++, block, num, 'W');

	return 0;
}

int nvfuse_trace_close(){
	
	if(trace_fp){
		fclose(trace_fp);
		trace_fp = NULL;
	}

	return 0;
}

static s32 segflush_start = 1;

void nvfuse_syncer_start()
{
	segflush_start = 1;
}

void nvfuse_syncer_stop()
{
	segflush_start = 0;
}

int nvfuse_segflush_init(struct nvfuse_superblock *sb)
{
	nvfuse_syncer_start();

	
	return 0;
}

void nvfuse_check_flush_segment(struct nvfuse_superblock *sb){
	s32 dirty_count = 0;		
		
	dirty_count = nvfuse_get_dirty_count(sb);
	
	if(dirty_count == 0)
		goto RES;
	
	nvfuse_sync(sb);
	
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
