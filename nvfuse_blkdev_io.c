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
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>

#include "nvfuse_core.h"
#include "nvfuse_config.h"
#include "nvfuse_io_manager.h"
#include "list.h"

#if NVFUSE_OS == NVFUSE_OS_LINUX
#include <libaio.h>
#	include <unistd.h>
#	include <sys/types.h>
#endif

static int blkdev_open(struct nvfuse_io_manager *io_manager, int flags);
static int blkdev_close(struct nvfuse_io_manager *io_manager);
static int blkdev_read_blk(struct nvfuse_io_manager *io_manager, long block,
			 int count, void *buf);
static int blkdev_write_blk(struct nvfuse_io_manager *io_manager, long block,
			  int count, void *buf);

static void io_getevents_error(int error)
{
	switch (error) {
	case -EFAULT:
		fprintf(stderr, " aio error: EFAULT\n");
		assert(0);
		break;
	case -EINVAL:
		fprintf(stderr, " aio error: EINVAL\n");
		assert(0);
		break;
	case -ENOSYS:
		fprintf(stderr, " aio error: ENOSYS\n");
		assert(0);
	case -EAGAIN:
		fprintf(stderr, " aio error: EAGAIN\n");
		assert(0);
		break;
	default:
		assert(0);
	}
}

static void io_cancel_error_decode(int error)
{
	switch (error) {
	case -EAGAIN:
		printf(" EAGAIN: io was not canceled\n");
		break;
	case -EFAULT:
		printf(" EFAULT: invalid data \n");
		break;
	case -EINVAL:
		printf(" EINVAL: ctx is invalid \n");
		break;
	case -ENOSYS:
		printf(" ENOSYS: IO_CANCEL is not implemented \n");
		break;
	default:
		break;
	}
}

static int libaio_init(struct nvfuse_io_manager *io_manager)
{
	int ret;

	ret = io_queue_init(io_manager->iodepth, &io_manager->io_ctx);
	if (ret) {
		printf(" io_queue_init fail ret = %d \n", ret);
	}

	printf(" called: libaio init \n");

	return 0;
}

static int libaio_cleanup(struct nvfuse_io_manager *io_manager)
{
	io_queue_release(io_manager->io_ctx);
	return 0;
}

static int libaio_prep(struct nvfuse_io_manager *io_manager, struct io_job *job)
{
	if (job->req_type == READ)
		io_prep_pread(&job->iocb, io_manager->dev, job->buf, job->bytes, job->offset);
	else
		io_prep_pwrite(&job->iocb, io_manager->dev, job->buf, job->bytes, job->offset);

	return 0;
}

static int libaio_submit(struct nvfuse_io_manager *io_manager, struct iocb **ioq, int qcnt)
{
	int ret;

	//printf(" called: libaio submit = %d\n", qcnt);

	ret = io_submit(io_manager->io_ctx, (long)qcnt, ioq);
	if (ret < 0) {
		switch (ret) {
		case -EBADF:
			printf(" EBADF: file descriptor invalid\n");
			break;
		case -EAGAIN:
			printf(" EAGAIN: io was not canceled\n");
			break;
		case -EFAULT:
			printf(" EFAULT: invalid data \n");
			break;
		case -EINVAL:
			printf(" EINVAL: ctx is invalid \n");
			break;
		case -ENOSYS:
			printf(" ENOSYS: IO_CANCEL is not implemented \n");
			break;
		default:
			printf(" unkown error = %d \n", ret);
			break;
		}
	}

	return ret;
}

static int libaio_complete(struct nvfuse_io_manager *io_manager)
{
	struct timespec time_out = {AIO_MAX_TIMEOUT_SEC, AIO_MAX_TIMEOUT_NSEC}; // {sec, nano}
	int max_nr = io_manager->queue_cur_count;
	int min_nr = 1;
	int retry_count = AIO_RETRY_COUNT;
	int cc = 0; // completion count
	int res;
	int i;

	//printf(" libaio_complete: max_nr = %d \n", max_nr);
	while (max_nr) {
		do {
			res = io_getevents(io_manager->io_ctx, min_nr, max_nr,
					   io_manager->events + cc, &time_out);
#if 0
			if (res == -EINTR) {
				fprintf(stderr, "libaio: EINTR happens and retry ...\n");
			}
#endif
		} while (res == -EINTR);

		if (res < 0) {
			io_getevents_error(res);
		}

		if ((res == 0 && min_nr) || res == -EAGAIN) { // timer expires
			printf(" AIO timer expires curr qdepth = %d, retval = %d  \n", min_nr, res);
			if (--retry_count)
				continue;

			break;
		}

		if (res) {
			max_nr -= res;
			cc += res;
			//printf(" cc = %d \n", cc);
		}
	}

	for (i = 0; i < cc; i++) {
		struct iocb *iocb;
		struct io_event *event;
		struct io_job *job;

		event = &io_manager->events[i];
		iocb = (struct iocb *)(event->obj);
		job  = (struct io_job *)container_of(iocb, struct io_job, iocb);

		job->ret = event->res; // return value
		io_manager->cjob[io_manager->cjob_head] = job;
		io_manager->cjob_head++;
	}

	return cc;
}

static struct io_job *libaio_getnextcjob(struct nvfuse_io_manager *io_manager)
{
	struct io_job *cur_job;

	assert(!cjob_empty(io_manager));

	cur_job = io_manager->cjob[io_manager->cjob_tail];
	io_manager->cjob[io_manager->cjob_tail] = NULL;

	io_manager->cjob_tail = (io_manager->cjob_tail + 1) % io_manager->iodepth;
	return cur_job;
}

#if 0
static void libaio_resetnextsjob(struct nvfuse_io_manager *io_manager)
{

}
#endif

static void libaio_resetnextcjob(struct nvfuse_io_manager *io_manager)
{

}

static int libaio_cancel(struct nvfuse_io_manager *io_manager, struct io_job *job)
{
	struct io_event event;
	int r;

	r = io_cancel(io_manager->io_ctx, &job->iocb, &event);
	if (r < 0) {
		io_cancel_error_decode(r);
	}

	return 0;
}

void nvfuse_init_blkdevio(struct nvfuse_io_manager *io_manager, char *name, char *path, int qdepth)
{
	int len;
	int i;

	len = strlen(path) + 1;

	io_manager->dev_path = (char *)malloc(len);
	memset(io_manager->dev_path, 0x00, len);
	strcpy(io_manager->dev_path, path);

	len = strlen(name) + 1;
	io_manager->io_name = (char *)malloc(len);
	memset(io_manager->io_name, 0x00, len);
	strcpy(io_manager->io_name, name);

	/* Sync I/O Function Pointers */
	io_manager->io_open = blkdev_open;
	io_manager->io_close = blkdev_close;
	io_manager->io_read = blkdev_read_blk;
	io_manager->io_write = blkdev_write_blk;

	io_manager->cjob_head = 0;
	io_manager->cjob_tail = 0;
	io_manager->iodepth = AIO_MAX_QDEPTH;
	io_manager->queue_cur_count = 0;

	for (i = 0; i < AIO_MAX_QDEPTH; i++) {
		io_manager->cjob[i] = NULL;
	}

	/* Linux AIO Function Pointers */
	io_manager->aio_init = libaio_init;
	io_manager->aio_cleanup = libaio_cleanup;
	io_manager->aio_prep = libaio_prep;
	io_manager->aio_submit = libaio_submit;
	io_manager->aio_complete = libaio_complete;
	io_manager->aio_getnextcjob = libaio_getnextcjob;
	io_manager->aio_resetnextcjob = libaio_resetnextcjob;
	io_manager->aio_cancel = libaio_cancel;
	io_manager->dev_format = NULL;
}


static int blkdev_open(struct nvfuse_io_manager *io_manager, int flags)
{
	int	retval = 0;
	int	open_flags = O_RDWR;

#if NVFUSE_OS == NVFUSE_OS_LINUX
	open_flags |= O_LARGEFILE;
	open_flags |= O_DIRECT;

	if (open_flags & O_DIRECT)
		printf(" USE DIRECT_IO \n");
	else if (open_flags & O_SYNC)
		printf(" USE SYNC_IO \n");
	else
		printf(" USE WRITE BACK CACHE\n");

	io_manager->dev = open64(io_manager->dev_path, open_flags);
#endif
	if (io_manager->dev < 0) {
		printf(" open error %s\n", io_manager->dev_path);
		exit(0);
	}

	io_manager->aio_init(io_manager);

	{
		u64 no_of_sectors;
		ioctl(io_manager->dev, BLKGETSIZE, &no_of_sectors);

		io_manager->blk_size = 512;
		io_manager->total_blkcount = no_of_sectors;
	}

	printf(" Init io manager (blkdev) = %ld sectors\n", io_manager->total_blkcount);
	return retval;
}


static int blkdev_close(struct nvfuse_io_manager *io_manager)
{
	int retval = 0;

	if (io_manager->aio_cleanup(io_manager) < 0) {
		retval = 1;
		goto RES;
	}

	if (close(io_manager->dev) < 0) {
		retval = errno;
	}

RES:

	return retval;
}

static int blkdev_read_blk(struct nvfuse_io_manager *io_manager, long block, int count, void *buf)
{
	int	size, rbytes = 0;
	s64	location;

	size =  count * CLUSTER_SIZE;
	location = ((s64) block * (s64)CLUSTER_SIZE);

#if NVFUSE_OS == NVFUSE_OS_LINUX
	rbytes = pread64(io_manager->dev, buf, size, location);
#endif

	if (rbytes != size) {
		printf(" read error, block = %lu, count = %d, size = %d\n", block, count, rbytes);
		//memcpy(NULL, NULL, 880);
		//	goto RETRY;
	}

	return rbytes;

}

static int blkdev_write_blk(struct nvfuse_io_manager *io_manager, long block, int count, void *buf)
{
	int	size, wbytes = 0;
	s64	location;

	size = count * CLUSTER_SIZE;
	location = ((s64) block * (s64)CLUSTER_SIZE);

#if NVFUSE_OS == NVFUSE_OS_LINUX
	wbytes = pwrite64(io_manager->dev, buf, size, location);
#endif

	return wbytes;
}

