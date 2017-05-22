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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <string.h>
#include "nvfuse_core.h"
#include "nvfuse_dirhash.h"

/*
 * Keyed 32-bit hash function using TEA in a Davis-Meyer function
 *   H0 = Key
 *   Hi = E Mi(Hi-1) + Hi-1
 *
 * (see Applied Cryptography, 2nd edition, p448).
 *
 * Jeremy Fitzhardinge <jeremy@zip.com.au> 1998
 *
 * This code is made available under the terms of the GPL
 */
#define DELTA 0x9E3779B9

static void TEA_transform(u32 buf[4], u32 const in[])
{
	u32	sum = 0;
	u32	b0 = buf[0], b1 = buf[1];
	u32	a = in[0], b = in[1], c = in[2], d = in[3];
	int	n = 16;

	do {
		sum += DELTA;
		b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
		b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
	} while (--n);

	buf[0] += b0;
	buf[1] += b1;
}

/* F, G and H are basic MD4 functions: selection, majority, parity */
#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))

/*
 * The generic round function.  The application is so specific that
 * we don't bother protecting all the arguments with parens, as is generally
 * good macro practice, in favor of extra legibility.
 * Rotation is separate from addition to prevent recomputation
 */
#define ROUND(f, a, b, c, d, x, s)	\
	(a += f(b, c, d) + x, a = (a << s) | (a >> (32-s)))
#define K1 0
#define K2 013240474631UL
#define K3 015666365641UL

/*
 * Basic cut-down MD4 transform.  Returns only 32 bits of result.
 */
static void halfMD4Transform(u32 buf[4], u32 const in[])
{
	u32	a = buf[0], b = buf[1], c = buf[2], d = buf[3];

	/* Round 1 */
	ROUND(F, a, b, c, d, in[0] + K1,  3);
	ROUND(F, d, a, b, c, in[1] + K1,  7);
	ROUND(F, c, d, a, b, in[2] + K1, 11);
	ROUND(F, b, c, d, a, in[3] + K1, 19);
	ROUND(F, a, b, c, d, in[4] + K1,  3);
	ROUND(F, d, a, b, c, in[5] + K1,  7);
	ROUND(F, c, d, a, b, in[6] + K1, 11);
	ROUND(F, b, c, d, a, in[7] + K1, 19);

	/* Round 2 */
	ROUND(G, a, b, c, d, in[1] + K2,  3);
	ROUND(G, d, a, b, c, in[3] + K2,  5);
	ROUND(G, c, d, a, b, in[5] + K2,  9);
	ROUND(G, b, c, d, a, in[7] + K2, 13);
	ROUND(G, a, b, c, d, in[0] + K2,  3);
	ROUND(G, d, a, b, c, in[2] + K2,  5);
	ROUND(G, c, d, a, b, in[4] + K2,  9);
	ROUND(G, b, c, d, a, in[6] + K2, 13);

	/* Round 3 */
	ROUND(H, a, b, c, d, in[3] + K3,  3);
	ROUND(H, d, a, b, c, in[7] + K3,  9);
	ROUND(H, c, d, a, b, in[2] + K3, 11);
	ROUND(H, b, c, d, a, in[6] + K3, 15);
	ROUND(H, a, b, c, d, in[1] + K3,  3);
	ROUND(H, d, a, b, c, in[5] + K3,  9);
	ROUND(H, c, d, a, b, in[0] + K3, 11);
	ROUND(H, b, c, d, a, in[4] + K3, 15);

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}

#undef ROUND
#undef F
#undef G
#undef H
#undef K1
#undef K2
#undef K3

/* The old legacy hash */
static ext2_dirhash_t dx_hack_hash(const char *name, int len,
				   int unsigned_flag)
{
	u32 hash, hash0 = 0x12a3fe2d, hash1 = 0x37abe8f9;
	const unsigned char *ucp = (const unsigned char *) name;
	const signed char *scp = (const signed char *) name;
	int c;

	while (len--) {
		if (unsigned_flag)
			c = (int) * ucp++;
		else
			c = (int) * scp++;
		hash = hash1 + (hash0 ^ (c * 7152373));

		if (hash & 0x80000000) hash -= 0x7fffffff;
		hash1 = hash0;
		hash0 = hash;
	}
	return (hash0 << 1);
}

static void str2hashbuf(const char *msg, int len, u32 *buf, int num,
			int unsigned_flag)
{
	u32	pad, val;
	int	i, c;
	const unsigned char *ucp = (const unsigned char *) msg;
	const signed char *scp = (const signed char *) msg;

	pad = (u32)len | ((u32)len << 8);
	pad |= pad << 16;

	val = pad;
	if (len > num * 4)
		len = num * 4;
	for (i = 0; i < len; i++) {
		if ((i % 4) == 0)
			val = pad;
		if (unsigned_flag)
			c = (int) ucp[i];
		else
			c = (int) scp[i];

		val = c + (val << 8);
		if ((i % 4) == 3) {
			*buf++ = val;
			val = pad;
			num--;
		}
	}
	if (--num >= 0)
		*buf++ = val;
	while (--num >= 0)
		*buf++ = pad;
}

/*
 * Returns the hash of a filename.  If len is 0 and name is NULL, then
 * this function can be used to test whether or not a hash version is
 * supported.
 *
 * The seed is an 4 longword (32 bits) "secret" which can be used to
 * uniquify a hash.  If the seed is all zero's, then some default seed
 * may be used.
 *
 * A particular hash version specifies whether or not the seed is
 * represented, and whether or not the returned hash is 32 bits or 64
 * bits.  32 bit hashes will return 0 for the minor hash.
 */
s32 ext2fs_dirhash(int version, const char *name, int len,
		   const u32 *seed,
		   ext2_dirhash_t *ret_hash,
		   ext2_dirhash_t *ret_minor_hash)
{
	u32	hash;
	u32	minor_hash = 0;
	const char	*p;
	int		i;
	u32 		in[8], buf[4];
	int		unsigned_flag = 0;

	/* Initialize the default seed for the hash checksum functions */
	buf[0] = 0x67452301;
	buf[1] = 0xefcdab89;
	buf[2] = 0x98badcfe;
	buf[3] = 0x10325476;

	/* Check to see if the seed is all zero's */
	if (seed) {
		for (i = 0; i < 4; i++) {
			if (seed[i])
				break;
		}
		if (i < 4)
			memcpy(buf, seed, sizeof(buf));
	}

	switch (version) {
	case EXT2_HASH_LEGACY_UNSIGNED:
		unsigned_flag++;
	case EXT2_HASH_LEGACY:
		hash = dx_hack_hash(name, len, unsigned_flag);
		break;
	case EXT2_HASH_HALF_MD4_UNSIGNED:
		unsigned_flag++;
	case EXT2_HASH_HALF_MD4:
		p = name;
		while (len > 0) {
			str2hashbuf(p, len, in, 8, unsigned_flag);
			halfMD4Transform(buf, in);
			len -= 32;
			p += 32;
		}
		minor_hash = buf[2];
		hash = buf[1];
		break;
	case EXT2_HASH_TEA_UNSIGNED:
		unsigned_flag++;
	case EXT2_HASH_TEA:
		p = name;
		while (len > 0) {
			str2hashbuf(p, len, in, 4, unsigned_flag);
			TEA_transform(buf, in);
			len -= 16;
			p += 16;
		}
		hash = buf[0];
		minor_hash = buf[1];
		break;
	default:
		*ret_hash = 0;
		return 0;
	}
	*ret_hash = hash & ~1;
	if (ret_minor_hash)
		*ret_minor_hash = minor_hash;
	return 0;
}

#if BITS_PER_LONG == 64
#define REX_PRE "0x48, "
#define SCALE_F 8
#else
#define REX_PRE
#define SCALE_F 4
#endif

int crc32c_intel_available = 0;
static int crc32c_probed;

static u32 crc32c_intel_le_hw_byte(u32 crc, unsigned char const *data,
				   unsigned long length)
{
	while (length--) {
		__asm__ __volatile__(
			".byte 0xf2, 0xf, 0x38, 0xf0, 0xf1"
			:"=S"(crc)
			:"0"(crc), "c"(*data)
		);
		data++;
	}

	return crc;
}

static inline void do_cpuid(unsigned int *eax, unsigned int *ebx,
			    unsigned int *ecx, unsigned int *edx)
{
	asm volatile("cpuid"
		     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		     : "0"(*eax), "2"(*ecx)
		     : "memory");
}
/*
 * Steps through buffer one byte at at time, calculates reflected
 * crc using table.
 */
u32 crc32c_intel(unsigned char const *data, unsigned long length)
{
	unsigned int iquotient = length / SCALE_F;
	unsigned int iremainder = length % SCALE_F;
#if BITS_PER_LONG == 64
	uint64_t *ptmp = (uint64_t *) data;
#else
	u32 *ptmp = (u32 *) data;
#endif
	u32 crc = ~0;

	while (iquotient--) {
		__asm__ __volatile__(
			".byte 0xf2, " REX_PRE "0xf, 0x38, 0xf1, 0xf1;"
			:"=S"(crc)
			:"0"(crc), "c"(*ptmp)
		);
		ptmp++;
	}

	if (iremainder)
		crc = crc32c_intel_le_hw_byte(crc, (unsigned char *)ptmp,
					      iremainder);

	return crc;
}

void crc32c_intel_probe(void)
{
	if (!crc32c_probed) {
		unsigned int eax, ebx, ecx = 0, edx;

		eax = 1;

		do_cpuid(&eax, &ebx, &ecx, &edx);
		crc32c_intel_available = (ecx & (1 << 20)) != 0;
		crc32c_probed = 1;
	}
}
