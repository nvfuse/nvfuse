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

#include<sys/stat.h>
#include "nvfuse_config.h"

#ifndef _NVFUSE_TYPES_H
#define _NVFUSE_TYPES_H

#if NVFUSE_OS == NVFUSE_OS_LINUX

	typedef signed char			s8;	
	typedef signed short		s16;
	typedef signed int			s32; 
	typedef signed long         s64; 

	typedef unsigned char		u8;	
	typedef unsigned short		u16;
	typedef unsigned int		u32; 
	typedef unsigned long       u64; 

	#define __O_SYNC        04000000

#	else
		
	typedef signed char			s8;	
	typedef signed short		s16;
	typedef signed int			s32; 
	typedef signed long long		s64; 

	typedef unsigned char		u8;	
	typedef unsigned short		u16;
	typedef unsigned int		u32; 
	typedef unsigned long long	u64; 

	typedef long	mode_t;
	typedef unsigned int dev_t;

	#define S_IRWXU 00700
	#define S_IRWXG 00070
	#define S_IRWXO 00007
	#define S_ISUID 0004000
	#define S_ISGID 0002000
	#define S_ISVTX 0001000

#ifndef S_IFMT
	#define S_IFMT  00170000
#endif
	#define S_IFSOCK 0140000
	#define S_IFLNK	 0120000
#ifndef S_IFREG
	#define S_IFREG  0100000

#endif
	#define S_IFBLK  0060000
#ifndef S_IFDIR
	#define S_IFDIR  0040000
	#define S_IFCHR  0020000
#endif
	#define S_IFIFO  0010000
	#define S_ISUID  0004000
	#define S_ISGID  0002000
	#define S_ISVTX  0001000

	#define S_IRWXU 00700
	#define S_IRUSR 00400
	#define S_IWUSR 00200
	#define S_IXUSR 00100

	#define S_IRWXG 00070
	#define S_IRGRP 00040
	#define S_IWGRP 00020
	#define S_IXGRP 00010

	#define S_IRWXO 00007
	#define S_IROTH 00004
	#define S_IWOTH 00002
	#define S_IXOTH 00001

	
	#define S_ISLNK(m)      (((m)& S_IFMT) == S_IFLNK)
	#define S_ISREG(m)      (((m)& S_IFMT) == S_IFREG)
	#define S_ISDIR(m)      (((m)& S_IFMT) == S_IFDIR)
	#define S_ISCHR(m)      (((m)& S_IFMT) == S_IFCHR)
	#define S_ISBLK(m)      (((m)& S_IFMT) == S_IFBLK)
	#define S_ISFIFO(m)     (((m)& S_IFMT) == S_IFIFO)
	#define S_ISSOCK(m)     (((m)& S_IFMT) == S_IFSOCK)

	#ifndef O_DSYNC
	#define O_DSYNC         00010000
	#endif

	#ifndef O_SYNC
	#define __O_SYNC        04000000
	#define O_SYNC			(__O_SYNC | O_DSYNC)
	#endif

	#ifndef O_DIRECT
	#define O_DIRECT     0200000
	#endif

	typedef s64 fsblkcnt_t;
	typedef s64 fsfilcnt_t;

	struct statvfs {
		unsigned long  f_bsize;    /* Filesystem block size */
		unsigned long  f_frsize;   /* Fragment size */
		fsblkcnt_t     f_blocks;   /* Size of fs in f_frsize units */
		fsblkcnt_t     f_bfree;    /* Number of free blocks */
		fsblkcnt_t     f_bavail;   /* Number of free blocks for
								   unprivileged users */
		fsfilcnt_t     f_files;    /* Number of inodes */
		fsfilcnt_t     f_ffree;    /* Number of free inodes */
		fsfilcnt_t     f_favail;   /* Number of free inodes for
								   unprivileged users */
		unsigned long  f_fsid;     /* Filesystem ID */
		unsigned long  f_flag;     /* Mount flags */
		unsigned long  f_namemax;  /* Maximum filename length */
	};
#	endif  
	
typedef s64	nvfuse_off_t;
//typedef u32	nvfuse_poff_t;
typedef u32	nvfuse_loff_t;

typedef u32 inode_t;//inode number
typedef u32 pbno_t;//physical block number
typedef u32 lbno_t;//logical block num

#define NVFUSE_TYPE_DATA 0
#define NVFUSE_TYPE_META 1

#define CEIL(x, y)  ((x + y - 1) / y)
#define DIV_UP(x, y)  ((x + y - 1) / y * y)
#ifdef MAX
#undef MAX
#endif
#define	MAX(x,y)	((x) > (y) ? (x) : (y))
#ifdef MIN
#undef MIN
#endif
#define	MIN(x,y)	((x) < (y) ? (x) : (y))
#endif 
