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

#ifndef _NVFUSE_INDIRECT_H
#define _NVFUSE_INDIRECT_H
s32 nvfuse_get_block(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 lblock,
		     u32 max_blocks, u32 *num_alloc_blks, u32 *pblock, u32 create);
void nvfuse_truncate_blocks(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
			    u64 offset);
#endif
