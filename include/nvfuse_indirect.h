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

#ifndef __NVFUSE_INDIRECT_H__
#define __NVFUSE_INDIRECT_H__
int nvfuse_block_to_path(s32 block, u32 offsets[4], u32 *boundary);
s32 nvfuse_get_block(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 lblock,
		     u32 max_blocks, u32 *num_alloc_blks, u32 *pblock, u32 create);

void nvfuse_truncate_blocks(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
			    u64 offset);

u32 nvfuse_alloc_free_block(struct nvfuse_superblock *sb, struct nvfuse_inode *inode,
			    u32 *alloc_blks, u32 num_blocks);
u32 nvfuse_alloc_free_blocks(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, u32 *blocks,
			     u32 num_indirect_blocks, u32 num_blocks, u32 *direct_map, s32 *error);
void nvfuse_return_free_blocks(struct nvfuse_superblock *sb, u32 *blks, u32 num);
s32 nvfuse_get_block(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 lblock,
		     u32 maxblocks, u32 *num_alloc_blocks, u32 *pblock, u32 create);

#endif /* __NVFUSE_INDIRECT_H__ */
