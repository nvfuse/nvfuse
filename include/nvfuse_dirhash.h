/* SPDX-License-Identifier: GPL-2.0-only
 * Directory-entry hash interface and NVFUSE CRC32C declarations.
 *
 * Copyright (c) 2001 Daniel Phillips
 * Copyright (c) 2002 Theodore Ts'o.
 *
 * Hash implementation source: e2fsprogs v1.43, lib/ext2fs/dirhash.c.
 * That upstream copy is available under the GNU Library GPL, version 2.
 * For this distribution, its section 3 option is exercised to distribute
 * this copy under the ordinary GNU GPL, version 2. See NOTICE and COPYING.
 * This does not change the license of the upstream project or old releases.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See COPYING
 * for the GNU General Public License, version 2.
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
