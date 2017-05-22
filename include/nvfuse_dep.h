/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2017 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 18/05/2017
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

#include "nvfuse_types.h"

#ifndef __NVFUSE_DEP_H__
#define __NVFUSE_DEP_H__

s32 ext2fs_set_bit(u32 nr, void *addr);
s32 ext2fs_clear_bit(u32 nr, void *addr);
s32 ext2fs_test_bit(u32 nr, const void *addr);
s32 fat_dirname(const s8 *path, s8 *dest);
s32 fat_filename(const s8 *path, s8 *dest);

#endif
