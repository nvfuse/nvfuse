/*
 * dirhash.c -- Calculate the hash of a directory entry
 *
 * Copyright (c) 2001  Daniel Phillips
 * 
 * Copyright (c) 2002 Theodore Ts'o.
 *
 *   This program is free software.
 *   You can redistribute it and/or modify it under the terms of either
 *   (1) the GNU General Public License; either version 3 of the License,
 *   or (at your option) any later version as published by
 *   the Free Software Foundation; or (2) obtain a commercial license
 *   by contacting the Author.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY;  without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program;  if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef __DIR_HASH__
#define __DIR_HASH__

#define USE_INTEL_CRC32C
#define EXT2_HASH_LEGACY		0
#define EXT2_HASH_HALF_MD4		1
#define EXT2_HASH_TEA			2
#define EXT2_HASH_LEGACY_UNSIGNED	3 /* reserved for userspace lib */
#define EXT2_HASH_HALF_MD4_UNSIGNED	4 /* reserved for userspace lib */
#define EXT2_HASH_TEA_UNSIGNED		5 /* reserved for userspace lib */

typedef u32	ext2_dirhash_t;
s32 ext2fs_dirhash(int version, const char *name, int len,
			 const u32 *seed,
			 ext2_dirhash_t *ret_hash,
			 ext2_dirhash_t *ret_minor_hash);

void crc32c_intel_probe(void);
u32 crc32c_intel(unsigned char const *data, unsigned long length);

#endif
