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

#define SPDK_NUM_CORES  16

#define NVFUSE_OS_WINDOWS 1
#define NVFUSE_OS_LINUX 2

#ifdef __linux__
#	define NVFUSE_OS NVFUSE_OS_LINUX
#else
#	define NVFUSE_OS NVFUSE_OS_WINDOWS
#endif

#define NVFUSE_PROCESS_MODEL NVFUSE_PROCESS_STANDALONE
#define NVFUSE_PROCESS_STANDALONE	0
#define NVFUSE_PROCESS_DATAPLANE	1

static inline int nvfuse_process_model_is_standalone(void)
{
	return NVFUSE_PROCESS_MODEL == NVFUSE_PROCESS_STANDALONE;
}

static inline int nvfuse_process_model_is_dataplane(void)
{
	return NVFUSE_PROCESS_MODEL == NVFUSE_PROCESS_DATAPLANE;
}

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
#	define NV_BLOCK_SIZE CLUSTER_SIZE

/* LRU Cache Size and Hash Size */
//#define NVFUSE_HASH_SIZE 100000
//#define HASH_NUM (15331)
#define HASH_NUM (52631)

/* attempting to allocate buffers and containers as much as desired at mount time*/
#define NVFUSE_CONTAINER_PERALLOCATION_SIZE	1024 /* in 128MB unit */

/* Default Buffer Cache Size */
#define NVFUSE_INITIAL_BUFFER_SIZE_DATA (16) //16MB
#define NVFUSE_INITIAL_BUFFER_SIZE_CONTROL (16) //16MB

/* Maximum Buffer Size per Data Plane*/
#define NVFUSE_MAX_BUFFER_SIZE_DATA (1536) // 1.5GB

/* Default Inode Context Size */
#define NVFUSE_ICTXC_SIZE (32*1024)

/* RATIO BG TO BUFFER Cache */
//#define NVFUSE_BUFFER_RATIO_TO_DATA (0.001) /* data optimized */
//#define NVFUSE_BUFFER_RATIO_TO_DATA (0.005) /* meta optimized*/
//#define NVFUSE_BUFFER_RATIO_TO_DATA (0.01) /* meta optimized*/
#define NVFUSE_BUFFER_DEFAULT_ALLOC_SIZE_PER_MSG 1

/* Buffer Head Mempool Settings */
#define NVFUSE_BH_MEMPOOL_TOTAL_SIZE	(0x10000) /* 256MB */
#define NVFUSE_BH_MEMPOOL_CACHE_SIZE	2048

/* Buffer Cache Mempool Settings */
#define NVFUSE_BC_MEMPOOL_TOTAL_SIZE	(0x400000) /* 16GB */
#define NVFUSE_BC_MEMPOOL_CACHE_SIZE	2048

/* Buffer Cache Mempool Settings */
#define NVFUSE_BPTREE_MEMPOOL_TOTAL_SIZE	(0x800)
#define NVFUSE_BPTREE_MEMPOOL_CACHE_SIZE	(0x10)

#define NVFUSE_BPTREE_MEMPOOL_MASTER_TOTAL_SIZE	(0x10)
#define NVFUSE_BPTREE_MEMPOOL_MASTER_CACHE_SIZE	(0x2)

#define NVFUSE_BPTREE_MEMPOOL_INDEX_TOTAL_SIZE	(0x100)
#define NVFUSE_BPTREE_MEMPOOL_INDEX_CACHE_SIZE	(0x10)

#define NVFUSE_BPTREE_MEMPOOL_PAIR_TOTAL_SIZE	(0x100)
#define NVFUSE_BPTREE_MEMPOOL_PAIR_CACHE_SIZE	(0x10)

enum bp_mempool_type {
	BP_MEMPOOL_INDEX	= 0,
	BP_MEMPOOL_MASTER	= 1,
	BP_MEMPOOL_KEY		= 2,
	BP_MEMPOOL_VALUE	= 3,
	BP_MEMPOOL_PAIR		= 4,
	BP_MEMPOOL_NUM		= 5
};

#define NVFUSE_SYNC_DIRTY_COUNT (2048) /* blocks */

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
//#define NVFUSE_META_DIRTY_POLICY NVFUSE_META_DIRTY_SYNC_FORCE
#define NVFUSE_META_DIRTY_POLICY NVFUSE_META_DIRTY_SYNC_DELAYED

/*	*/
#define NVFUSE_USE_DELAYED_REDISTRIBUTION_BPTREE
/*	*/
#define NVFUSE_USE_DELAYED_BPTREE_CREATION

/* actual dir blocks are allocated lazyily */
#define NVFUSE_USE_DELAYED_DIRECTORY_ALLOC

/* Insert SLEEP to minimize CPU utilization */
#define NVFUSE_USE_USLEEP_US 0 /* 0>: us sleep,  0: disabled */

#define USE_RBNODE

#define MAX_NUM_STAT 8
/* Readahead Size*/
#define NVFUSE_MIN_RA_SIZE (4*CLUSTER_SIZE)
#define NVFUSE_MAX_RA_SIZE (32*CLUSTER_SIZE)

/* MKFS uses zeroing to initialize inode table */
//#define NVFUSE_USE_MKFS_INODE_ZEROING

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

#define NVFUSE_BG_SIZE_BITS 22
#define NVFUSE_BG_GROUP_NUM 1

#define NVFUSE_USE_MINISH
//#define NVFUSE_KEEP_DIRTY_BH_IN_ICTX

/* Please use the below definition when the spdk forked in the ceph repo is deploy */
//#define NVFUSE_USE_CEPH_SPDK

#endif
