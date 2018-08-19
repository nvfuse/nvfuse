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

#include <stdio.h>
#include <pthread.h>

#include "nvfuse_types.h"
#include "nvfuse_stat.h"
#include "nvfuse_reactor.h"

#ifndef __IO_MANAGER__
#define __IO_MANAGER__

#if NVFUSE_OS == NVFUSE_OS_LINUX
#include <libaio.h>
#endif
#ifndef _NVFUSE_IO_H
#define _NVFUSE_IO_H

//#define DISK_SUCCESS	0
//#define DISK_FAIL		1

//#define USE_NVFUSE_TRACE 0
//#define USE_BUFFER_POOL 0

#ifndef __USE_FUSE__
#if NVFUSE_OS == NVFUSE_OS_LINUX
#	define USE_RAMDISK  0
#	define USE_FILEDISK 0
#   ifdef SPDK_ENABLED
#	define USE_SPDK	    1
#	define USE_BLKDEVIO   0
#   else
#	define USE_SPDK	    0
#	define USE_BLKDEVIO   1
#   endif
#else
#	define USE_RAMDISK  0
#	define USE_FILEDISK 1
#	define USE_BLKDEVIO   0
#endif
#else
#	define USE_RAMDISK  0
#	define USE_FILEDISK 0
#	define USE_BLKDEVIO   1
#endif

#define SECTOR_SIZE      512
#define SECTOR_SIZE_BITS 9

#define CLUSTER_PER_SECTOR (CLUSTER_SIZE >> SECTOR_SIZE_BITS)
#define SECTORS_PER_CLUSTER CLUSTER_PER_SECTOR

/* Maximum Number of Queue Depth */
#define AIO_MAX_QDEPTH  512

#define AIO_RETRY_COUNT         5
#define AIO_MAX_TIMEOUT_SEC     10   // 5 sec
#define AIO_MAX_TIMEOUT_NSEC    0    // 0 nsec

struct io_job {
	void *buf;
	long offset; // in bytes
	int bytes; // requested bytes
	struct iovec		iov[REACTOR_BUFFER_IOVS];
	int					iovcnt;
	int req_type; // is read
	spdk_bdev_io_completion_cb cb;
	struct reactor_task *task;
	size_t ret; //return value of pread or pwrite

	int complete;
	void *tag1;
	void *tag2;
};

#define SPDK_QUEUE_SYNC 0
#define SPDK_QUEUE_AIO  1
#define SPDK_QUEUE_NUM  2

#define SPDK_MAX_CORES	8

#define IO_MAGIC	0xABCDABCD

#define nvfuse_write_cluster(b, n, target) reactor_sync_write_blk(target, (long)n, 1, b)
#define nvfuse_read_cluster(b, n, target) reactor_sync_read_blk(target, (long)n, 1, b)
//#define nvfuse_aio_submit(b, n, t, io_manager) reactor_submit_reqs(io_manager->target, t, b, n);

#define nvfuse_aio_prep(b, target) //spdk_prep(io_manager, b)

#define nvfuse_aio_cancel(b, target) //spdk_cancel(io_manager, b);

//#define nvfuse_open(target, flags) //spdk_open(target, flags);
//#define nvfuse_close(target) //spdk_close(target);

/* device level format (e.g., nvme format) */
#define nvfuse_dev_format(target) //spdk_dev_format(io_manager);

/* device level flush (e.g., nvme flush) */
//#define nvfuse_dev_flush(io_manager) reactor_sync_flush(io_manager->target);
#endif

#endif
