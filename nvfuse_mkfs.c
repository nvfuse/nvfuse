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
#include "nvfuse_dep.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_bp_tree.h"
#include "nvfuse_malloc.h"
#include "nvfuse_dirhash.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_mkfs.h"
#include "nvfuse_debug.h"

s32 nvfuse_alloc_root_inode_direct(struct io_target *target,
		struct nvfuse_superblock *sb_disk, u32 bg_id, u32 bg_size)
{
	struct nvfuse_bg_descriptor *bd;
	struct nvfuse_inode *inode;
	struct nvfuse_dir_entry *d_entry;
	void *bd_buf;
	void *buf;
	u32 ino = 0;

	bd_buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (bd_buf == NULL) {
		dprintf_error(MEMALLOC, "Malloc error \n");
		return -1;
	}

	memset(bd_buf, 0x00, CLUSTER_SIZE);
	nvfuse_read_cluster(bd_buf, bg_id * bg_size + NVFUSE_BD_OFFSET, target);
	bd = (struct nvfuse_bg_descriptor *)bd_buf;
	assert(bd->bd_magic == NVFUSE_BD_MAGIC);
	assert(bd->bd_id == bg_id);

	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL) {
		dprintf_error(MEMALLOC, "Malloc error \n");
		return -1;
	}

	// reserved and root inode bitmap allocation
	nvfuse_read_cluster(buf, bd->bd_ibitmap_start, target);
	for (ino = 0; ino < NUM_RESV_INO; ino++) {
		ext2fs_set_bit(ino, buf);
		bd->bd_free_inodes--;
		sb_disk->sb_free_inodes--;
	}
	nvfuse_write_cluster(buf, bd->bd_ibitmap_start, target);

	// data block for root directory allocation
	nvfuse_read_cluster(buf, bd->bd_dbitmap_start, target);
	//printf(" data block for root dir = %d \n", (int)bd->bd_dtable_start);

	ext2fs_set_bit(bd->bd_dtable_start % bg_size, buf);
	bd->bd_free_blocks--;
	sb_disk->sb_free_blocks--;
	nvfuse_write_cluster(buf, bd->bd_dbitmap_start, target);

	// root inode allocation
#if (INODE_ENTRY_SIZE == 128)
	nvfuse_read_cluster(buf, bd->bd_itable_start, target);
	memset(buf, 0x0, CLUSTER_SIZE);
	inode = (struct nvfuse_inode *)buf;
	for (ino = 0; ino < NUM_RESV_INO; ino++) {
		inode[ino].i_ino = ino;
		
		if (ino != ROOT_INO)
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
		inode[ino].i_blocks[0] = bd->bd_dtable_start;
	}
	nvfuse_write_cluster(buf, bd->bd_itable_start, target);
#elif (INODE_ENTRY_SIZE == 4096)
	for (ino = 0; ino < NUM_RESV_INO; ino++) {
		nvfuse_read_cluster(buf, bd->bd_itable_start + ino, target);
		memset(buf, 0x0, CLUSTER_SIZE);

		if (ino == ROOT_INO) {
			inode = (struct nvfuse_inode *)buf;
			//root inode
			inode->i_ino = ROOT_INO;
			inode->i_type = NVFUSE_TYPE_DIRECTORY;
			inode->i_size = DIR_ENTRY_SIZE * DIR_ENTRY_NUM;
			inode->i_version = 1;
			inode->i_ptr = 1;
			inode->i_gid = 0;
			inode->i_uid = 0;
			inode->i_mode = 0600 | S_IFDIR;
			inode->i_atime = time(NULL);
			inode->i_ctime = time(NULL);
			inode->i_mtime = time(NULL);
			inode->i_links_count = 2;
			inode->i_blocks[0] = bd->bd_dtable_start;
		}

		dprintf_debug(FORMAT, " write inode = %d on %d block \n", ino, bd->bd_itable_start + ino);

		nvfuse_write_cluster(buf, bd->bd_itable_start + ino, target);
	}
#else
	dprintf_error(FORMAT, " Unsupported inode size = %d \n", INODE_ENTRY_SIZE);
	assert(0);
#endif

	// root data block allocation
	nvfuse_read_cluster(buf, bd->bd_dtable_start, target);

	memset(buf, 0x0, CLUSTER_SIZE);
	d_entry = (struct nvfuse_dir_entry *)buf;

	//root directory
	strcpy(d_entry[0].d_filename, ".");
	d_entry[0].d_ino = ROOT_INO;
	d_entry[0].d_flag = DIR_USED;

	strcpy(d_entry[1].d_filename, "..");
	d_entry[1].d_ino = ROOT_INO;
	d_entry[1].d_flag = DIR_USED;

	nvfuse_write_cluster(buf, bd->bd_dtable_start, target);
	nvfuse_write_cluster(bd_buf, bg_id * bg_size + NVFUSE_BD_OFFSET, target);
	nvfuse_free_aligned_buffer(buf);
	nvfuse_free_aligned_buffer(bd_buf);

	return 0;
}

void nvfuse_make_bg_descriptor(struct nvfuse_bg_descriptor *bd, u32 bg_id, u32 bg_start, u32 bg_size)
{
	bd->bd_magic	= NVFUSE_BD_MAGIC;
	bd->bd_owner	= 0;
	bd->bd_id		= bg_id;
	bd->bd_bg_start	= 0;
	bd->bd_bd_start	= NVFUSE_BD_OFFSET;
	bd->bd_ibitmap_start	= NVFUSE_IBITMAP_OFFSET;
	bd->bd_ibitmap_size		= NVFUSE_DBITMAP_SIZE;

	bd->bd_max_inodes	= NVFUSE_INODE_PER_BG;
	bd->bd_max_blocks	= NVFUSE_DATA_PER_BG;

	bd->bd_dbitmap_start	= NVFUSE_DBITMAP_OFFSET;
	bd->bd_dbitmap_size	= NVFUSE_DBITMAP_SIZE;
	bd->bd_itable_start	= bd->bd_dbitmap_start + bd->bd_dbitmap_size;
	bd->bd_itable_size	= bd->bd_max_inodes * INODE_ENTRY_SIZE / CLUSTER_SIZE;
	bd->bd_dtable_start	= bd->bd_itable_start + bd->bd_itable_size;
	bd->bd_dtable_size	= bg_size - bd->bd_dtable_start;

	bd->bd_free_inodes = bd->bd_max_inodes;
	/* reserve metadata blocks including sb, inode, and bitmaps. */
	bd->bd_free_blocks = bd->bd_max_blocks - bd->bd_dtable_start;

	bd->bd_bg_start	+= bg_start;
	bd->bd_bd_start	+= bg_start;
	bd->bd_ibitmap_start	+= bg_start;
	bd->bd_dbitmap_start	+= bg_start;
	bd->bd_itable_start	+= bg_start;
	bd->bd_dtable_start	+= bg_start;
}

void nvfuse_print_bd(struct nvfuse_bg_descriptor *bd)
{
	dprintf_info(FORMAT, " magic = %x bytes \n", bd->bd_magic);
	dprintf_info(FORMAT, " inode size = %u bytes \n", INODE_ENTRY_SIZE);
	dprintf_info(FORMAT, " bd_bg_start = %u\n", bd->bd_bd_start);
	dprintf_info(FORMAT, " bd_ibitmap_start = %u\n", bd->bd_ibitmap_start);
	dprintf_info(FORMAT, " bd_ibitmap_size = %u blocks \n", bd->bd_ibitmap_size);
	dprintf_info(FORMAT, " bd_dbitmap_start = %u\n", bd->bd_dbitmap_start);
	dprintf_info(FORMAT, " bd_dbitmap_size = %u blocks \n", bd->bd_dbitmap_size);
	dprintf_info(FORMAT, " itable start = %u \n", bd->bd_itable_start);
	dprintf_info(FORMAT, " itable size = %u blocks \n", bd->bd_itable_size);
	dprintf_info(FORMAT, " dtable start = %u \n", bd->bd_dtable_start);
	dprintf_info(FORMAT, " dtable size = %u blocks\n", bd->bd_dtable_size);
	dprintf_info(FORMAT, " bd end = %u \n", bd->bd_dtable_start + bd->bd_dtable_size);
}

s32 nvfuse_format_write_bd(struct nvfuse_handle *nvh,
		struct nvfuse_superblock *sb_disk, u32 num_bgs, u32 bg_size)
{
	struct io_target *target= nvh->nvh_target;
	struct nvfuse_bg_descriptor *bd;
	void *bd_buf;
	void *buf;
	u32 bg_id;
	u32 bg_start;

	bd_buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (bd_buf == NULL) {
		dprintf_error(MEMALLOC, " malloc error \n");
		return -1;
	}
	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL) {
		dprintf_error(MEMALLOC, " malloc error \n");
		return -1;
	}

	for (bg_id = 0; bg_id < num_bgs; bg_id++) {
		bg_start = bg_id * bg_size;
		//printf(" write bg = %u \n", bg_id);

		/* Initialize and Write Block Descriptor */
		memset(bd_buf, 0x00, CLUSTER_SIZE);
		bd = (struct nvfuse_bg_descriptor *)bd_buf;

		/* make bg descriptor */
		nvfuse_make_bg_descriptor(bd, bg_id, bg_start, bg_size);

		sb_disk->sb_free_inodes += bd->bd_free_inodes;
		sb_disk->sb_free_blocks += bd->bd_free_blocks;

		nvfuse_write_cluster(bd_buf, bd->bd_bd_start, target);
#if 0 /* debug */
		nvfuse_read_cluster(bd_buf, bd->bd_bd_start, target);
		assert(bd->bd_id == bg_id);
#endif
#if 1
		if (bg_id != 0)
			continue;

		dprintf_info(FORMAT, " \n");
		nvfuse_print_bd(bd);
		dprintf_info(FORMAT, "\n");
#endif
	}

	nvfuse_free_aligned_buffer(bd_buf);
	nvfuse_free_aligned_buffer(buf);

	return 0;

}

s32 nvfuse_format_metadata_zeroing(struct nvfuse_handle *nvh,
		struct nvfuse_superblock *sb_disk, u32 num_bgs, u32 bg_size)
{
	struct nvfuse_bg_descriptor *bd;
	void *bd_buf;
	void *buf;

#ifdef NVFUSE_USE_MKFS_INODE_ZEROING
	void *zeroing_buf;
	u32 zeroing_blocks;
#endif

	u32 bg_id;
	u32 bg_start;
	u32 clu;
	struct io_target *target = nvh->nvh_target;

	bd_buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (bd_buf == NULL) {
		dprintf_error(FORMAT, " malloc error \n");
		return -1;
	}

	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (bd_buf == NULL) {
		dprintf_error(FORMAT, " malloc error \n");
		return -1;
	}

	for (bg_id = 0; bg_id < num_bgs; bg_id++) {
		bg_start = bg_id * bg_size;

		/* Initialize and Write BG DEscriptor */
		memset(bd_buf, 0x00, CLUSTER_SIZE);
		bd = (struct nvfuse_bg_descriptor *)bd_buf;

		/* make bg descriptor */
		nvfuse_make_bg_descriptor(bd, bg_id, bg_start, bg_size);

		/* Initialize ibitmap table */
		memset(buf, 0x00, CLUSTER_SIZE);
		nvfuse_write_cluster(buf, bd->bd_ibitmap_start, target);

		/* reserve clusters ranging from bd to itable */
		memset(buf, 0x00, CLUSTER_SIZE);
		for (clu = bd->bd_bg_start; clu < bd->bd_itable_start + bd->bd_itable_size; clu++) {
			ext2fs_set_bit(clu % bg_size, buf);
		}
		nvfuse_write_cluster(buf, bd->bd_dbitmap_start, target);

		/* inode can be allocated through ibitmap */
#ifdef NVFUSE_USE_MKFS_INODE_ZEROING
		if (bg_id == 0) {
			zeroing_blocks = bd->bd_itable_size;
			zeroing_buf = nvfuse_alloc_aligned_buffer(zeroing_blocks * CLUSTER_SIZE);
			if (zeroing_buf == NULL) {
				dprintf_error(FORMAT, " malloc error \n");
				return -1;
			}
			memset(zeroing_buf, 0x00, zeroing_blocks * CLUSTER_SIZE);
			//printf(" zeroing blocks = %d \n", zeroing_blocks);
		}

		/* write zero data to inode table */
		nvfuse_write_ncluster(zeroing_buf, bd->bd_itable_start, bd->bd_itable_size, target);

		if (bg_id + 1 == num_bgs) {
			nvfuse_free_aligned_buffer(zeroing_buf);
		}
#endif

#if 0
		for (clu = bd->bd_dtable_start;
		     clu < bd->bd_dtable_start + bd->bd_dtable_size;
		     clu++) {
			nvfuse_write_cluster(buf, clu, target);
		}
#endif
	}

	nvfuse_free_aligned_buffer(bd_buf);
	nvfuse_free_aligned_buffer(buf);

	return 0;
}

s32 nvfuse_format_bg(struct nvfuse_handle *nvh, struct nvfuse_superblock *sb_disk,
				 u32 num_bgs, u32 bg_size)
{
	s32 res;
	/* building and writing bd for each bg */
	res = nvfuse_format_write_bd(nvh, sb_disk, num_bgs, bg_size);
	if (res) {
		dprintf_error(FORMAT, " Error: format write bd \n");
		return res;
	}
	/* zeroing bitmap and inode tables */
	res = nvfuse_format_metadata_zeroing(nvh, sb_disk, num_bgs, bg_size);
	if (res) {
		dprintf_error(FORMAT, " Error: Metadata Zeroing \n");
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
#if (NVFUSE_OS==NVFUSE_OS_LINUX)
	assert(sizeof(long) == 8);
#endif
}

s32 nvfuse_format(struct nvfuse_handle *nvh)
{
	struct io_target *target = nvh->nvh_target;
	struct nvfuse_superblock *nvfuse_sb_disk;

	struct timeval format_tv;

	u32 bg_size_bits;
	s32 bg_p_clu = 0;
	u32 bg_size = 0;

	u64 num_clu, num_sectors, num_bg;
	s8 *buf;

	s32 ret;

	dprintf_info(FORMAT, "-------------------------------------------------------------------\n");
	dprintf_info(FORMAT, " Formatting NVFUSE ...\n");
	dprintf_info(FORMAT, " Warning: your data will be removed permanently...\n");
	dprintf_info(FORMAT, "--------------------Option------------------------------------------\n");

	nvfuse_type_check();

	/* beginning of format time measurement */
	gettimeofday(&format_tv, NULL);

	if (INODE_ENTRY_SIZE != sizeof(struct nvfuse_inode)) {
		dprintf_error(FORMAT, " check inode size = %d \n", (int)sizeof(struct nvfuse_inode));
		return -1;
	}
	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL) {
		dprintf_error(FORMAT, " %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
		return -1;
	}

	/* low level device format (e.g., nvme format) */
	nvfuse_dev_format(&nvh->nvh_iom);

	// initialize super block
	memset(buf, 0x00, CLUSTER_SIZE);
	nvfuse_sb_disk = (struct nvfuse_superblock *) buf;

	dprintf_info(FORMAT, " spdk: io_target = %p\n", target);
	num_sectors = nvh->total_blkcount;
	num_clu = num_sectors / SECTORS_PER_CLUSTER;

	assert(num_sectors && num_clu);

	dprintf_info(FORMAT, " sectors = %lu, blocks = %lu\n", (unsigned long)num_sectors, (unsigned long)num_clu);

	bg_size = CLUSTER_SIZE << BITS_PER_CLUSTER_BITS << CLUSTER_SIZE_BITS;
	bg_size_bits = (s32)log2(bg_size);

	num_bg = NVFUSE_BG_NUM(num_clu, bg_size_bits - CLUSTER_SIZE_BITS);
	num_clu = num_bg << (bg_size_bits - CLUSTER_SIZE_BITS);

	dprintf_info(FORMAT, " bg size = %dMB \n", (1 << bg_size_bits) / 1024 / 1024);
	dprintf_info(FORMAT, " num bgs = %ld \n", (unsigned long)num_bg);

	bg_p_clu = 1 << (bg_size_bits - CLUSTER_SIZE_BITS);

	ret = nvfuse_format_bg(nvh, nvfuse_sb_disk, num_bg, bg_p_clu);
	if (ret) {
		return NVFUSE_ERROR;
	}

#ifdef NVFUSE_BD_DEBUG
	nvfuse_bd_debug(&nvh->nvh_iom, bg_p_clu, num_bg);
#endif

	ret = nvfuse_alloc_root_inode_direct(nvh->nvh_target, nvfuse_sb_disk, 0, bg_p_clu);
	if (ret) {
		return NVFUSE_ERROR;
	}

	nvfuse_sb_disk->sb_no_of_sectors = num_sectors;
	nvfuse_sb_disk->sb_no_of_blocks = num_clu;

	nvfuse_sb_disk->sb_signature = NVFUSE_SB_SIGNATURE;

	nvfuse_sb_disk->sb_no_of_inodes_per_bg = NVFUSE_INODE_PER_BG;
	nvfuse_sb_disk->sb_no_of_blocks_per_bg = NVFUSE_DATA_PER_BG;

	dprintf_info(FORMAT, " inodes per bg = %d \n", nvfuse_sb_disk->sb_no_of_inodes_per_bg);
	dprintf_info(FORMAT, " blocks per bg = %d \n", nvfuse_sb_disk->sb_no_of_blocks_per_bg);

	nvfuse_sb_disk->sb_root_ino = ROOT_INO;
	nvfuse_sb_disk->asb.asb_root_bg_id = 0;

	nvfuse_sb_disk->sb_bg_num = NVFUSE_BG_NUM(nvfuse_sb_disk->sb_no_of_blocks,
					 bg_size_bits - CLUSTER_SIZE_BITS);

	nvfuse_sb_disk->sb_no_of_blocks = (nvfuse_sb_disk->sb_bg_num) * bg_p_clu;
	nvfuse_sb_disk->sb_no_of_sectors = (nvfuse_sb_disk->sb_bg_num) * bg_p_clu *
					   (CLUSTER_SIZE / SECTOR_SIZE);

	nvfuse_sb_disk->sb_state = FS_STATE_FORMATTED;

	nvfuse_sb_disk->sb_max_inode_num = (u32)~0 >> (NVFUSE_MAX_BITS - NVFUSE_MAX_INODE_BITS);
	nvfuse_sb_disk->sb_max_file_num = (u32)~0 >> (NVFUSE_MAX_BITS - NVFUSE_MAX_FILE_BITS);
	nvfuse_sb_disk->sb_max_dir_num = (u32)~0 >> (NVFUSE_MAX_BITS - NVFUSE_MAX_DIR_BITS);

	dprintf_info(FORMAT, " NVFUSE writes the superblock on 0 block\n");
	nvfuse_write_cluster(buf, INIT_NVFUSE_SUPERBLOCK_NO, target);

	dprintf_info(FORMAT, " NVFUSE capability\n");
	dprintf_info(FORMAT, " max file size = %.3fTB\n", (double)MAX_FILE_SIZE / NVFUSE_TERA_BYTES);

	dprintf_info(FORMAT, " max files per directory = %08x\n", MAX_FILES_PER_DIR);
	dprintf_info(FORMAT, " NVFUSE was formatted successfully. (%.3f sec)\n", nvfuse_time_since_now(&format_tv));
	dprintf_info(FORMAT, "-------------------------------------------------------------------\n");
	fflush(stdout);

	nvfuse_free_aligned_buffer(buf);

	return NVFUSE_SUCCESS;
}

#ifdef NVFUSE_BD_DEBUG
void nvfuse_bd_debug(io_target *target, u32 bg_size, u32 num_bgs)
{
	void *bd_buf;
	s32 bg_id;
	struct nvfuse_bg_descriptor *bd;

	bd_buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (bd_buf == NULL) {
		dprintf_error(FORMAT, " Malloc error \n");
	}

	for (bg_id = 0; bg_id < num_bgs; bg_id++) {
		memset(bd_buf, 0x00, CLUSTER_SIZE);
		nvfuse_read_cluster(bd_buf, bg_id * bg_size + NVFUSE_BD_OFFSET, target);
		bd = (struct nvfuse_bg_descriptor *)bd_buf;
		if (bg_id != bd->bd_id) {
			dprintf_error(FORMAT, " mismatch bgid = %d\n", bg_id);
			assert(0);
		}
	}

	nvfuse_free_aligned_buffer(bd_buf);
}
#endif



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

	return size * blksize;
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

#if USE_BLKDEVIO == 1
	ioctl(fd, BLKGETSIZE, &no_of_sectors);
#endif

	dprintf_info(FORMAT, " no of sectors = %llu\n", no_of_sectors);
	return no_of_sectors;
}

#endif //__NOUSE_FUSE__
#endif // NVFUSE_OS == NVFUSE_OS_LINUX

