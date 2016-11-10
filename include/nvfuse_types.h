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

#	endif  
	
typedef u64	nvfuse_off_t;
typedef u32	nvfuse_poff_t;
typedef u32	nvfuse_loff_t;

typedef u32 inode_t;//inode number
typedef u32 pbno_t;//physical block number
typedef u32 lbno_t;//logical block num

#endif 
