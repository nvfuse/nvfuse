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
#ifndef __NVFUSE_MKFS__
#define __NVFUSE_MKFS__

void nvfuse_make_bg_descriptor(struct nvfuse_bg_descriptor *bd, u32 bg_id, u32 bg_start, u32 bg_size);
s32 nvfuse_alloc_root_inode_direct(struct nvfuse_io_manager *io_manager,
		struct nvfuse_superblock *sb_disk, u32 bg_id, u32 bg_size);

s32 nvfuse_format_write_bd(struct nvfuse_handle *nvh,
		struct nvfuse_superblock *sb_disk, u32 num_bgs, u32 bg_size);

s32 nvfuse_format_metadata_zeroing(struct nvfuse_handle *nvh,
		struct nvfuse_superblock *sb_disk, u32 num_bgs, u32 bg_size);

s32 nvfuse_format_bg(struct nvfuse_handle *nvh, struct nvfuse_superblock *sb_disk,
				 u32 num_bgs, u32 bg_size);

void nvfuse_type_check();

s32 nvfuse_format(struct nvfuse_handle *nvh);

//#define NVFUSE_BD_DEBUG
#ifdef NVFUSE_BD_DEBUG
static void nvfuse_bd_debug(struct nvfuse_io_manager *io_manager, u32 bg_size, u32 num_bgs);
#endif
u32 get_part_size(s32 fd);
u32 get_sector_size(s32 fd);
u64 get_no_of_sectors(s32 fd);
#endif
