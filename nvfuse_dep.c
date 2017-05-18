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

/*
 * Collection of some functions and codes which are dependent to license issue.
 */

///*
//* For the benefit of those who are trying to port Linux to another
//* architecture, here are some C-language equivalents.  You should
//* recode these in the native assmebly language, if at all possible.
//*
//* C language equivalents written by Theodore Ts'o, 9/26/92.
//* Modified by Pete A. Zaitcev 7/14/95 to be portable to big endian
//* systems, as well as non-32 bit systems.
//*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvfuse_types.h"

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
