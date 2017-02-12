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
#if NVFUSE_OS == NVFUSE_OS_WINDOWS
#include <Windows.h>
struct iocb;
typedef struct io_context io_context_t;
struct io_event;
#endif 
#include "nvfuse_types.h"
#include "nvfuse_stat.h"

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

#define IO_MANAGER_SPDK		1
#define IO_MANAGER_UNIXIO	2
#define IO_MANAGER_FILEDISK	3
#define IO_MANAGER_RAMDISK	4


#ifndef __USE_FUSE__
#if NVFUSE_OS == NVFUSE_OS_LINUX
#	define USE_RAMDISK  0
#	define USE_FILEDISK 0
#   ifdef SPDK_ENABLED
#	define USE_SPDK	    1 
#	define USE_UNIXIO   0 
#   else
#	define USE_SPDK	    0 
#	define USE_UNIXIO   1 
#   endif
#else
#	define USE_RAMDISK  0
#	define USE_FILEDISK 1
#	define USE_UNIXIO   0 
#endif
#else
#	define USE_RAMDISK  0
#	define USE_FILEDISK 0
#	define USE_UNIXIO   1
#	define USE_MTDIO    0
#endif 

// #define DISK_NAME "/dev/nvme0n1p1"

// #define DISK_FILE_NUM 2
// #define DISK_FILE_PATH "d:/DATA_DISK.DAT"

// #ifndef __USE_FUSE__
// #if (USE_RAMDISK == 1)
// 	#define NO_OF_SECTORS	(2*2*1024*1024) // 64M	
// #else
// #	define NO_OF_SECTORS	((long long)128*2*1024*1024) // 512M
// #endif
// #else
// #	define NO_OF_SECTORS	(16*1024*1024) // 512M
// #endif

#define SECTOR_SIZE      512
#define SECTOR_SIZE_BITS 9

#define CLUSTER_PER_SECTOR (CLUSTER_SIZE >> SECTOR_SIZE_BITS)
#define SECTORS_PER_CLUSTER CLUSTER_PER_SECTOR

//#define DISK_SIZE ((unsigned long long)SECTOR_SIZE * NO_OF_SECTORS)

/* Maximum Number of Queue Depth */
#define AIO_MAX_QDEPTH  512

#define AIO_RETRY_COUNT         5
#define AIO_MAX_TIMEOUT_SEC     10   // 5 sec
#define AIO_MAX_TIMEOUT_NSEC    0    // 0 nsec


struct io_job{
#if NVFUSE_OS == NVFUSE_OS_LINUX
    struct iocb iocb;
#endif
    long offset; // in bytes
    long discard_range[2]; //discard
    size_t bytes; // requested bytes
    size_t ret; //return value of pread or pwrite
    int req_type; // is read 
    char *buf;
    int complete;
    void *tag1;
    void *tag2;
};

#define SPDK_QUEUE_SYNC 0
#define SPDK_QUEUE_AIO  1
#define SPDK_QUEUE_NUM  2

struct nvfuse_io_manager {
    char *io_name;
    char *dev_path;
    int type;
    char *ramdisk;
    FILE *fp;
    int dev;

    int blk_size; /* 512B, 4KB */
    long start_blk; /* start block (512B)*/
    long total_blkcount; /* number of sectors (512B) */
    int cpu_core_mask; /* cpu core mask for SPDK */
    
#if NVFUSE_OS == NVFUSE_OS_LINUX
    io_context_t io_ctx;
    struct io_event events[AIO_MAX_QDEPTH];

    struct spdk_nvme_qpair *spdk_queue[SPDK_QUEUE_NUM];
    struct nvfuse_ipc_context *ipc_ctx;
    union perf_stat perf_stat_dev;
#endif
    int queue_cur_count;

    // submit job
    //struct io_job *io_job_subq[AIO_MAX_QDEPTH];
    //int io_job_subq_count;

    // complete job
    struct io_job *cjob[AIO_MAX_QDEPTH];
    int cjob_head; // head ptr
    int cjob_tail; // tail ptr
    int cjob_cnt;

    int iodepth;

    int (*io_open)(struct nvfuse_io_manager *io_manager, int flags);
    int (*io_close)(struct nvfuse_io_manager *io_manager);
    int (*io_read)(struct nvfuse_io_manager *io_manager, long block, int count, void *data);
    int (*io_write)(struct nvfuse_io_manager *io_manager, long block, int count, void *data);

    int (*aio_init)(struct nvfuse_io_manager *);
    int (*aio_cleanup)(struct nvfuse_io_manager *);
    int (*aio_prep)(struct nvfuse_io_manager *, struct io_job *);
    int (*aio_submit)(struct nvfuse_io_manager *, struct iocb **, int qcnt);
    int (*aio_complete)(struct nvfuse_io_manager *);
    struct io_job *(*aio_getnextcjob)(struct nvfuse_io_manager *);
    void (*aio_resetnextcjob)(struct nvfuse_io_manager *);
    void (*aio_resetnextsjob)(struct nvfuse_io_manager *);
    int (*aio_cancel)(struct nvfuse_io_manager *, struct io_job *);
    int (*dev_format)(struct nvfuse_io_manager *);
    int (*dev_flush)(struct nvfuse_io_manager *);
};

#define nvfuse_write_ncluster(b, n, k, io_manager) io_manager->io_write(io_manager, (long)n, k, b)
#define nvfuse_read_ncluster(b, n, k, io_manager) io_manager->io_read(io_manager, (long)n, k, b)

#define nvfuse_write_cluster(b, n, io_manager) io_manager->io_write(io_manager, (long)n, 1, b)
#define nvfuse_read_cluster(b, n, io_manager) io_manager->io_read(io_manager, (long)n, 1, b)

#define nvfuse_aio_prep(b, io_manager) io_manager->aio_prep(io_manager, b)

#define nvfuse_aio_submit(b, n, io_manager) io_manager->aio_submit(io_manager, b, n);

/* reset submission job */
#define nvfuse_aio_resetnextsjob(io_manager) \
	if (io_manager->aio_resetnextsjob) \
		io_manager->aio_resetnextsjob(io_manager);

/* reset completion job */
#define nvfuse_aio_resetnextcjob(io_manager) \
    if (io_manager->aio_resetnextcjob) \
    io_manager->aio_resetnextcjob(io_manager);

#define nvfuse_aio_complete(io_manager) io_manager->aio_complete(io_manager);
#define nvfuse_aio_getnextcjob(io_manager) io_manager->aio_getnextcjob(io_manager);
#define nvfuse_aio_cancel(b, io_manager) io_manager->aio_cancel(io_manager, b);
/* device level format (e.g., nvme format) */
#define nvfuse_dev_format(io_manager) \
    if ((io_manager)->dev_format) \
	(io_manager)->dev_format(io_manager);

/* device level flush (e.g., nvme flush) */
#define nvfuse_dev_flush(io_manager) \
    if ((io_manager)->dev_flush) \
	(io_manager)->dev_flush(io_manager);

extern struct nvfuse_io_manager *nvfuse_io_manager;
void nvfuse_init_unixio(struct nvfuse_io_manager *io_manager, char *name, char *path, int qdepth);
void nvfuse_init_spdk(struct nvfuse_io_manager *io_manager, char *filename, char *path, int iodepth);
void nvfuse_init_fileio(struct nvfuse_io_manager *io_manager, char *name, char *path, int dev_size);
void nvfuse_init_memio(struct nvfuse_io_manager *io_manager, char *name, char *path, int qdepth);

static inline s32 cjob_empty(struct nvfuse_io_manager *io_manager)
{
    return io_manager->cjob_head == io_manager->cjob_head;
}
 
static inline s32 cjob_full(struct nvfuse_io_manager *io_manager)
{
    if (io_manager->cjob_tail <= io_manager->cjob_head)
        return (io_manager->cjob_head - io_manager->cjob_tail) == (io_manager->iodepth - 1);
    else
        return (io_manager->cjob_head + 1) == io_manager->cjob_tail;
}

static inline s32 cjob_size(struct nvfuse_io_manager *io_manager)
{
    if (io_manager->cjob_tail <= io_manager->cjob_head)
        return io_manager->cjob_head - io_manager->cjob_tail;
    else
        return io_manager->iodepth - io_manager->cjob_tail + io_manager->cjob_head;
}
#endif 

int spdk_eal_init(s32 core_mask);
int spdk_alloc_qpair(struct nvfuse_io_manager *io_manager);
void spdk_release_qpair(struct nvfuse_io_manager *io_manager);
#endif