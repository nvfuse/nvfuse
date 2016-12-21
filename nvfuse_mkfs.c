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
#include <math.h>

#include "nvfuse_core.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_bp_tree.h"
#include "nvfuse_malloc.h"
#include "nvfuse_dirhash.h"
#include "nvfuse_gettimeofday.h"

#if NVFUSE_OS == NVFUSE_OS_LINUX

#ifndef __NOUSE_FUSE__

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
	struct mtd_info_user meminfo;
#endif 
#if USE_UNIXIO == 1
	ioctl(fd, BLKGETSIZE, &no_of_sectors);
#endif

#if USE_MTDIO == 1
	if (ioctl(fd, MEMGETINFO, &meminfo) != 0) {
		perror("ioctl(MEMGETINFO)");
		//close(io_manager->dev);
		//BUG();
	}
	no_of_sectors = meminfo.size / 512;
#endif

	printf(" no of sectors = %llu\n", no_of_sectors);
	return no_of_sectors;
}

#endif //__NOUSE_FUSE__
#endif // NVFUSE_OS == NVFUSE_OS_LINUX


static s32 nvfuse_alloc_root_inode_direct(struct nvfuse_io_manager *io_manager, struct nvfuse_superblock *sb_disk, u32 seg_id, u32 seg_size)
{
	struct nvfuse_segment_summary *ss;
	struct nvfuse_inode *inode;
	struct nvfuse_dir_entry *d_entry;
	void *ss_buf;
	void *buf;
	u32 ino = 0;
	u32 blkno = 0;

	ss_buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (ss_buf == NULL)
	{
		printf(" Malloc error \n");
		return -1;
	}

	nvfuse_read_cluster(ss_buf, seg_id * seg_size + NVFUSE_SUMMARY_OFFSET, io_manager);
	ss = (struct nvfuse_segment_summary *)ss_buf;

	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL)
	{
		printf(" Malloc error \n");
		return -1;
	}

	// reserved and root inode bitmap allocation
	nvfuse_read_cluster(buf, ss->ss_ibitmap_start, io_manager);
	for (ino = 0; ino < NUM_RESV_INO; ino++)
	{
		ext2fs_set_bit(ino, buf);
		ss->ss_free_inodes--;
		sb_disk->sb_free_inodes--;
	}
	nvfuse_write_cluster(buf, ss->ss_ibitmap_start, io_manager);


	// data block for root directory allocation
	nvfuse_read_cluster(buf, ss->ss_dbitmap_start, io_manager);
	//printf(" data block for root dir = %d \n", (int)ss->ss_dtable_start);

	ext2fs_set_bit(ss->ss_dtable_start % seg_size, buf);
	ss->ss_free_blocks--;
	sb_disk->sb_free_blocks--;
	nvfuse_write_cluster(buf, ss->ss_dbitmap_start, io_manager);

	// root inode allocation
	nvfuse_read_cluster(buf, ss->ss_itable_start, io_manager);
	memset(buf, 0x0, CLUSTER_SIZE);
	inode = (struct nvfuse_inode *)buf;
	for (ino = 0; ino < NUM_RESV_INO; ino++)
	{
		inode[ino].i_ino = ino;
		if(ino != ROOT_INO)
			continue;

		//root inode
		inode[ino].i_ino = ROOT_INO;
		inode[ino].i_type = NVFUSE_TYPE_DIRECTORY;
		inode[ino].i_size = DIR_ENTRY_SIZE * DIR_ENTRY_NUM;		
		inode[ino].i_version = 1;
		inode[ino].i_ptr = 1;
		inode[ino].i_gid = 0;
		inode[ino].i_uid = 0;
		inode[ino].i_mode = 0600 | S_IFDIR;
		inode[ino].i_atime = time(NULL);
		inode[ino].i_ctime = time(NULL);
		inode[ino].i_mtime = time(NULL);
		inode[ino].i_links_count = 2;
		inode[ino].i_blocks[0] = ss->ss_dtable_start;
	}
	nvfuse_write_cluster(buf, ss->ss_itable_start, io_manager);

	// root data block allocation
	nvfuse_read_cluster(buf, ss->ss_dtable_start, io_manager);

	memset(buf, 0x0, CLUSTER_SIZE);
	d_entry = (struct nvfuse_dir_entry *)buf;

	//root directory
	strcpy(d_entry[0].d_filename,".");
	d_entry[0].d_ino = ROOT_INO;
	d_entry[0].d_flag = DIR_USED;

	strcpy(d_entry[1].d_filename,"..");
	d_entry[1].d_ino = ROOT_INO;
	d_entry[1].d_flag = DIR_USED;

	nvfuse_write_cluster(buf, ss->ss_dtable_start, io_manager);
	nvfuse_write_cluster(ss_buf, seg_id * seg_size + NVFUSE_SUMMARY_OFFSET, io_manager);
	nvfuse_free_aligned_buffer(buf);
	nvfuse_free_aligned_buffer(ss_buf);

	return 0;
}

void nvfuse_make_segment_summary(struct nvfuse_segment_summary *ss, u32 seg_id, u32 seg_start, u32 seg_size)
{
	u32 bits_per_bitmap;

	ss->ss_id		= seg_id;
	ss->ss_seg_start	= 0;
	ss->ss_summary_start	= NVFUSE_SUMMARY_OFFSET;
	ss->ss_ibitmap_start	= NVFUSE_IBITMAP_OFFSET;
	ss->ss_ibitmap_size	= NVFUSE_IBITMAP_SIZE;

	bits_per_bitmap		= NVFUSE_IBITMAP_SIZE * CLUSTER_SIZE * 8;
	ss->ss_max_inodes	= bits_per_bitmap / 2;
	ss->ss_max_blocks	= bits_per_bitmap;

	ss->ss_dbitmap_start	= NVFUSE_DBITMAP_OFFSET;
	ss->ss_dbitmap_size	= NVFUSE_DBITMAP_SIZE;
	ss->ss_itable_start	= ss->ss_dbitmap_start + ss->ss_dbitmap_size;
	ss->ss_itable_size	= ss->ss_max_inodes * INODE_ENTRY_SIZE / CLUSTER_SIZE;
	ss->ss_dtable_start	= ss->ss_itable_start + ss->ss_itable_size;
	ss->ss_dtable_size	= seg_size - ss->ss_dtable_start;

	ss->ss_free_inodes = ss->ss_max_inodes;
	ss->ss_free_blocks = ss->ss_max_blocks - ss->ss_dtable_start; // reserve metadata blocks including sb, inode, and bitmaps.

	ss->ss_seg_start	+= seg_start;
	ss->ss_summary_start	+= seg_start;
	ss->ss_ibitmap_start	+= seg_start;
	ss->ss_dbitmap_start	+= seg_start;
	ss->ss_itable_start	+= seg_start;
	ss->ss_dtable_start	+= seg_start;
}

static s32 nvfuse_format_write_segment_summary(struct nvfuse_handle *nvh, struct nvfuse_superblock *sb_disk, u32 num_segs, u32 seg_size)
{
	struct nvfuse_segment_summary *ss;
	void *ss_buf;
	void *buf;
	u32 seg_id;
	u32 seg_start;
	u32 bits_per_bitmap;
	u32 clu;
	struct nvfuse_io_manager *io_manager = &nvh->nvh_iom;

	ss_buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (ss_buf == NULL)
	{
		printf(" malloc error \n");
		return -1;
	}
	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL)
	{
		printf(" malloc error \n");
		return -1;
	}

	for (seg_id = 0; seg_id < num_segs; seg_id++)
	{
		seg_start = seg_id * seg_size;
		//printf(" seg_stat = %u \n", seg_start);

		/* Initialize and Write Segment Summary */
		memset(ss_buf, 0x00, CLUSTER_SIZE);
		ss = (struct nvfuse_segment_summary *)ss_buf;

		/* make segment sumamry */
		nvfuse_make_segment_summary(ss, seg_id, seg_start, seg_size);

		sb_disk->sb_free_inodes += ss->ss_free_inodes;
		sb_disk->sb_free_blocks += ss->ss_free_blocks;

		nvfuse_write_cluster(ss, ss->ss_summary_start, io_manager);

#if 1
		if(seg_id != 0)
			continue;

		printf(" \n");
		printf(" inode size = %u bytes \n", INODE_ENTRY_SIZE);
		printf(" ss_summary_start = %u\n", ss->ss_summary_start);
		printf(" ss_ibitmap_start = %u\n", ss->ss_ibitmap_start);
		printf(" ss_ibitmap_size = %u blocks \n", ss->ss_ibitmap_size);
		printf(" ss_dbitmap_start = %u\n", ss->ss_dbitmap_start);
		printf(" ss_dbitmap_size = %u blocks \n", ss->ss_dbitmap_size);
		printf(" itable start = %u \n", ss->ss_itable_start);
		printf(" itable size = %u blocks \n", ss->ss_itable_size);
		printf(" dtable start = %u \n", ss->ss_dtable_start);
		printf(" dtable size = %u blocks\n", ss->ss_dtable_size);
		printf(" ss end = %u \n", ss->ss_dtable_start + ss->ss_dtable_size);
		printf("\n");
#endif
	}

	nvfuse_free_aligned_buffer(ss_buf);
	nvfuse_free_aligned_buffer(buf);

	return 0;

}

static s32 nvfuse_format_metadata_zeroing(struct nvfuse_handle *nvh, struct nvfuse_superblock *sb_disk, u32 num_segs, u32 seg_size)
{
	struct nvfuse_segment_summary *ss;
	void *ss_buf;
	void *buf;

	void *zeroing_buf;
	u32 zeroing_blocks;

	u32 seg_id;
	u32 seg_start;
	u32 bits_per_bitmap;
	u32 clu;
	struct nvfuse_io_manager *io_manager = &nvh->nvh_iom;

	ss_buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (ss_buf == NULL)
	{
		printf(" malloc error \n");
		return -1;
	}

	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (ss_buf == NULL)
	{
		printf(" malloc error \n");
		return -1;
	}

	for (seg_id = 0; seg_id < num_segs; seg_id++)
	{
		seg_start = seg_id * seg_size;

		/* Initialize and Write Segment Summary */
		memset(ss_buf, 0x00, CLUSTER_SIZE);
		ss = (struct nvfuse_segment_summary *)ss_buf;

		/* make segment sumamry */
		nvfuse_make_segment_summary(ss, seg_id, seg_start, seg_size);

		/* Initialize ibitmap table */
		memset(buf, 0x00, CLUSTER_SIZE);
		nvfuse_write_cluster(buf, ss->ss_ibitmap_start, io_manager);

		/* reserve clusters ranging from ss to itable */
		memset(buf, 0x00, CLUSTER_SIZE);
		for (clu = ss->ss_seg_start; clu < ss->ss_itable_start + ss->ss_itable_size; clu++)
		{
			ext2fs_set_bit(clu % seg_size, buf);
		}
		nvfuse_write_cluster(buf, ss->ss_dbitmap_start, io_manager);

		/* inode can be allocated through ibitmap */
#ifdef NVFUSE_USE_MKFS_INODE_ZEROING
		if (seg_id == 0) 
		{
			zeroing_blocks = ss->ss_itable_size;
			zeroing_buf = nvfuse_alloc_aligned_buffer(zeroing_blocks * CLUSTER_SIZE);
			if (zeroing_buf == NULL)
			{
				printf(" malloc error \n");
				return -1;
			}
			memset(zeroing_buf, 0x00, zeroing_blocks * CLUSTER_SIZE);
			//printf(" zeroing blocks = %d \n", zeroing_blocks);
		}

		/* write zero data to inode table */
		io_manager->io_write(io_manager, ss->ss_itable_start, ss->ss_itable_size, zeroing_buf);

		if (seg_id + 1 == num_segs)
		{
			nvfuse_free_aligned_buffer(zeroing_buf);
		}
#endif

#if 0
		for (clu = ss->ss_dtable_start; 
		     clu < ss->ss_dtable_start + ss->ss_dtable_size; 
		     clu++)
		{
			nvfuse_write_cluster(buf, clu, io_manager);
		}
#endif
	}

	nvfuse_free_aligned_buffer(ss_buf);
	nvfuse_free_aligned_buffer(buf);

	return 0;
}

static s32 nvfuse_format_segment(struct nvfuse_handle *nvh, struct nvfuse_superblock *sb_disk, u32 num_segs, u32 seg_size)
{
	s32 res;
	/* building and writing segment summary for each segment */
	res = nvfuse_format_write_segment_summary(nvh, sb_disk, num_segs, seg_size);
	if (res)
	{
		printf(" Error: format write segment summary \n");
		return res;
	}
	/* zeroing bitmap and inode tables */
	res = nvfuse_format_metadata_zeroing(nvh, sb_disk, num_segs, seg_size);
	if(res)
	{
		printf(" Error: Metadata Zeroing \n");
		return res;
	}

	return 0;
}

void nvfuse_type_check()
{
	assert(sizeof(s32) == 4);
	assert(sizeof(u32) == 4);
	assert(sizeof(s64) == 8);
	assert(sizeof(u64) == 8);
}


s32 nvfuse_format(struct nvfuse_handle *nvh) {
	s32 i, j, clu = 0, add_clu = 0;
	
	u32 seg_size_bits;
	s32 seg_p_clu = 0;
	u32 seg_size = 0;

	s32 su_blocks = 0, su_segs = 0;
	u32 su_start = 0;

	u32 dir_hash = 0;
	u64 num_clu, num_sectors, num_seg;
	s8 *buf, *su_p;

	struct nvfuse_superblock	*nvfuse_sb_disk;
	struct nvfuse_segment_summary	seg_sum;
	struct nvfuse_segment_usage *seg_usage;
	
	struct nvfuse_dir_entry d_entry[3];
	struct nvfuse_inode inode[5]; // root_inode, ifile, segment_usage file, bpfile
	
	struct nvfuse_inode *root_inode, *ifile_inode, *su_inode;	
	struct nvfuse_inode *bp_tree_inode, *ss_inode;	
	struct nvfuse_inode *ip;	
	master_node_t *bp_master;

	struct timeval tv; 
	struct timeval format_tv;

	struct nvfuse_io_manager *io_manager = &nvh->nvh_iom;
	key_pair_t *pair;
	s32 pair_count = 0;
	s32 pair_size = 0;

	s32 ret;
	
	printf("-------------------------------------------------------------------\n");
	printf(" Formatting NVFUSE ...\n");
	printf(" Warning: your data will be removed permanently...\n");
	printf("--------------------Option------------------------------------------\n");

	nvfuse_type_check();

	/* beginning of format time measurement */
	gettimeofday(&format_tv, NULL);

	if(INODE_ENTRY_SIZE != sizeof(struct nvfuse_inode)){
		printf(" check inode size = %d \n", (int)sizeof(struct nvfuse_inode)); 
		return -1;
	}
	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL) {
		printf(" nvfuse_malloc error \n");
		return -1;
	}
	
	// initialize super block
	memset(buf, 0x00, CLUSTER_SIZE);
	nvfuse_sb_disk = (struct nvfuse_superblock *) buf;

	memset(inode, 0x00, INODE_ENTRY_SIZE*5);
	root_inode = &inode[0];
	ifile_inode = &inode[1];
	su_inode = &inode[2];
	bp_tree_inode = &inode[3];
	ss_inode = &inode[4];
		
#if NVFUSE_OS == NVFUSE_OS_WINDOWS
	num_sectors = NO_OF_SECTORS;
	num_clu = (u32)NVFUSE_NUM_CLU;
#else
#	ifdef __USE_FUSE__
	num_sectors = get_no_of_sectors(io_manager->dev);
	num_clu = (u32)num_sectors / (u32)SECTORS_PER_CLUSTER;
#	else
#		if USE_RAMDISK == 1 || USE_FILEDISK == 1
		num_sectors = NO_OF_SECTORS;
		num_clu = (u32)NVFUSE_NUM_CLU;
#		elif USE_UNIX_IO == 1
		printf(" unix: nvfuse_io_manager = %p\n", io_manager);
		num_sectors = io_manager->total_blkcount;
		num_clu = num_sectors / SECTORS_PER_CLUSTER;
#		elif USE_SPDK == 1
		printf(" spdk: nvfuse_io_manager = %p\n", io_manager);
		num_sectors = io_manager->total_blkcount;
		num_clu = num_sectors / SECTORS_PER_CLUSTER;
#		endif
#	endif
	num_sectors = io_manager->total_blkcount;
	num_clu = num_sectors / SECTORS_PER_CLUSTER;
	/* FIXME: total_blkcount must be set to when io_manager is initialized. */
#endif

	printf(" nvfuse_io_manager = %p\n", io_manager);

	printf(" sectors = %lu, blocks = %lu\n", (unsigned long)num_sectors, (unsigned long)num_clu);
	
	seg_size = CLUSTER_SIZE << BITS_PER_CLUSTER_BITS << CLUSTER_SIZE_BITS;
	seg_size_bits = (s32)log2(seg_size);

	num_seg = NVFUSE_SEG_NUM(num_clu, seg_size_bits - CLUSTER_SIZE_BITS);
	num_clu = num_seg << (seg_size_bits - CLUSTER_SIZE_BITS);

	printf(" segment size = %dMB \n", (1 << seg_size_bits)/1024/1024);
	printf(" num segments = %ld \n", (unsigned long)num_seg);

	seg_p_clu = 1 << (seg_size_bits - CLUSTER_SIZE_BITS);

	ret = nvfuse_format_segment(nvh, nvfuse_sb_disk, num_seg, seg_p_clu);
	if (ret)
	{
		return NVFUSE_ERROR;
	}
	
	ret = nvfuse_alloc_root_inode_direct(&nvh->nvh_iom, nvfuse_sb_disk, 0, seg_p_clu);
	if (ret)
	{
		return NVFUSE_ERROR;
	}
	
	nvfuse_sb_disk->sb_no_of_sectors = num_sectors;
	nvfuse_sb_disk->sb_no_of_blocks = num_clu;

	nvfuse_sb_disk->sb_signature = NVFUSE_SB_SIGNATURE;
	
	nvfuse_sb_disk->sb_no_of_inodes_per_seg = NVFUSE_IBITMAP_SIZE * CLUSTER_SIZE * 8 /2;
	nvfuse_sb_disk->sb_no_of_blocks_per_seg = NVFUSE_IBITMAP_SIZE * CLUSTER_SIZE * 8;

	printf(" inodes per seg = %d \n", nvfuse_sb_disk->sb_no_of_inodes_per_seg);
	printf(" blocks per seg = %d \n", nvfuse_sb_disk->sb_no_of_blocks_per_seg);

	nvfuse_sb_disk->sb_root_ino = ROOT_INO;

	nvfuse_sb_disk->sb_segment_num = NVFUSE_SEG_NUM(nvfuse_sb_disk->sb_no_of_blocks, seg_size_bits - CLUSTER_SIZE_BITS);
	
	nvfuse_sb_disk->sb_no_of_blocks = (nvfuse_sb_disk->sb_segment_num) * seg_p_clu;
	nvfuse_sb_disk->sb_no_of_sectors = (nvfuse_sb_disk->sb_segment_num) * seg_p_clu * (CLUSTER_SIZE / SECTOR_SIZE);
		
	nvfuse_sb_disk->sb_umount = 1;

	nvfuse_sb_disk->sb_free_segment_num = nvfuse_sb_disk->sb_segment_num - 1;
	
	nvfuse_sb_disk->sb_max_inode_num = (u32)~0 >> (NVFUSE_MAX_BITS - NVFUSE_MAX_INODE_BITS);
	nvfuse_sb_disk->sb_max_file_num = (u32)~0 >> (NVFUSE_MAX_BITS - NVFUSE_MAX_FILE_BITS);
	nvfuse_sb_disk->sb_max_dir_num = (u32)~0 >> (NVFUSE_MAX_BITS - NVFUSE_MAX_DIR_BITS);

	gettimeofday(&tv, NULL);
	nvfuse_sb_disk->sb_last_update_sec = tv.tv_sec;
	nvfuse_sb_disk->sb_last_update_usec = tv.tv_usec;

	nvfuse_write_cluster(buf, INIT_NVFUSE_SUPERBLOCK_NO, io_manager);

	printf(" buffer pool size = %fMB\n",(float)(NVFUSE_BUFFER_SIZE * CLUSTER_SIZE)/(float)(1024*1024));	
	printf("\n NVFUSE capability\n");
	printf(" max file size = %.3fTB\n", (double)MAX_FILE_SIZE / NVFUSE_TERA_BYTES);

	printf(" max files per directory = %08x\n", MAX_FILES_PER_DIR);
	printf("\n NVFUSE was formatted successfully. (%.3f sec)\n", time_since_now(&format_tv));
	printf("-------------------------------------------------------------------\n");
	fflush(stdout);

	nvfuse_free_aligned_buffer(buf);

	return NVFUSE_SUCCESS;
}
