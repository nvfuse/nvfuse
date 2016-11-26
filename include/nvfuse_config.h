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

#ifndef _NVFUSE_CONFIG_H
#define _NVFUSE_CONFIG_H

#define NVFUSE_VERSION "0.1"

#define NVFUSE_OS_WINDOWS 1
#define NVFUSE_OS_LINUX 2

#ifdef __linux__
#	define NVFUSE_OS NVFUSE_OS_LINUX
#else 
#	define NVFUSE_OS NVFUSE_OS_WINDOWS
#endif 


#if NVFUSE_OS == NVFUSE_OS_LINUX
#   define __NOUSE_FUSE__
#ifndef __NOUSE_FUSE__
#	define __USE_FUSE__
#	define __USE_LL_FUSE__ //low level fuse
#endif
#else
//#	define __USE_FUSE__
//#	define __USE_LL_FUSE__ //low level fuse
#define __NOUSE_FUSE__
#endif 

#	define CLUSTER_SIZE (4*1024)
#	define CLUSTER_SIZE_BITS 12
#   define BITS_PER_CLUSTER 8
#   define BITS_PER_CLUSTER_BITS 3

/* LRU Cache Size and Hash Size */
#define NVFUSE_HASH_SIZE 100000

#ifdef __linux__
#	define NVFUSE_BUFFER_SIZE (1024*32) //128MB
#else
#	define NVFUSE_BUFFER_SIZE (32*1024) //128MB
#endif

#	define NVFUSE_ICTXC_SIZE (32*1024)

#define NVFUSE_SYNC_TIME	30 /* seconds */
//#define NVFUSE_SYNC_DIRTY_COUNT (1024) /* blocks */
#define NVFUSE_SYNC_DIRTY_COUNT (256) /* blocks */

/* Segment Summary Logging Size for SSR scheme */
#define NVFUSE_SS_LOG_FLUSH	(4096* 4)

/* Use Async Write */
#define NVFUSE_USE_ASYNC_WRITER

/* Use AIO Library for Dirty Sync */
#if NVFUSE_OS == NVFUSE_OS_LINUX
#define USE_AIO_WRITE
#endif

/* Sync Thread Timeout */
#define NVFUSE_SYNC_TIMEOUT_USEC 1000
#define NVFUSE_SYNC_TIMEOUT_SEC 5

/* Meta Data Dirty Sync Policy */
/* buffer cache keeps dirty meta data until a centain amount of time passes*/
#define NVFUSE_META_DIRTY_SYNC_DELAYED DIRTY_FLUSH_DELAY
/* flush dirty meta data right after data updates */
#define NVFUSE_META_DIRTY_SYNC_FORCE   DIRTY_FLUSH_FORCE
#define NVFUSE_META_DIRTY_POLICY NVFUSE_META_DIRTY_SYNC_FORCE

/* Readahead Size*/
#define NVFUSE_MIN_RA_SIZE (4*CLUSTER_SIZE)
#define NVFUSE_MAX_RA_SIZE (32*CLUSTER_SIZE)


/* Directory Indexing */
#define NVFUSE_USE_DIR_INDEXING 1

/* debug message */
//#define printf
#ifdef __linux__
	#define NVFUSE_DEBUG(s...) fprintf(stderr,s);
	//#define NVFUSE_DEBUG
#else
	#define NVFUSE_DEBUG printf
	//#define NVFUSE_DEBUG
#endif 
/* NAND INFORMATIOIN*/
#define SPARE_SIZE 64
#define ERASE_SIZE (256*1024)

#define NAND_PROG_DELAY 200
#define NAND_READ_DELAY 80
#define NAND_ERASE_DELAY (2*1000)


#define NVFUSE_SEGMENT_SIZE_BITS 22
#define NVFUSE_SEGMENT_GROUP_NUM 1

#define NVFUSE_USE_DOUBLE_SS
//#define NVFUSE_WOLF_SSR

#define NVFUSE_USE_MINISH

#endif 
