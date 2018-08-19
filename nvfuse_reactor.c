/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2017 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 06/07/2017
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

#ifndef NVFUSE_USE_CEPH_SPDK
#include "spdk/stdinc.h"
#else
#include <stdint.h>
#endif
#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/bdev.h"
#include "spdk/copy_engine.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/io_channel.h"
#include "nvfuse_config.h"
#include "nvfuse_debug.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_reactor.h"

#define REACTOR_LCORE	0
struct io_target *head[RTE_MAX_LCORE];
static int g_target_count = 0;

#ifndef NVFUSE_USE_CEPH_SPDK
void reactor_bio_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
#else
void reactor_bio_cb(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status success, void *cb_arg)
#endif
{
	struct io_target	*target;
	struct io_job *req = cb_arg;
	struct reactor_task	*task = req->task;

	target = task->target;

#ifndef NVFUSE_USE_CEPH_SPDK
	if (!success) {
#else
	if (success != SPDK_BDEV_IO_STATUS_SUCCESS) {
#endif
		dprintf_error(REACTOR, " bdev i/o failed (req = %p, bno = %ld) \n", req, req->offset);
		dprintf_error(REACTOR, " lcore = %d.\n", rte_lcore_id());
		abort();
		req->ret = -1;
	} else {
		req->ret = 0;
	}

	//dprintf_info(REACTOR, " current queue depth = %d \n", target->current_queue_depth);
	target->current_queue_depth--;
	target->io_completed++;

	reactor_cq_put_req(task, req);

	spdk_bdev_free_io(bdev_io);
}

inline int reactor_sq_is_empty(struct reactor_task *task)
{
	return task->sq_head ==  task->sq_tail;
}

inline int reactor_sq_is_full(struct reactor_task *task)
{
	if (task->sq_tail <= task->sq_head)
		return (task->sq_head - task->sq_tail) == (task->qdepth - 1);
	else
		return (task->sq_head + 1) == task->sq_tail;
}

inline int reactor_cq_is_empty(struct reactor_task *task)
{
	return task->cq_head ==  task->cq_tail;
}

inline int reactor_sq_size(struct reactor_task *task)
{
	if (task->sq_tail <= task->sq_head)
		return task->sq_head - task->sq_tail;
	else
		return task->qdepth - task->sq_tail + task->sq_head;
}

inline int reactor_cq_is_full(struct reactor_task *task)
{
	if (task->cq_tail <= task->cq_head)
		return (task->cq_head - task->cq_tail) == (task->qdepth - 1);
	else
		return (task->cq_head + 1) == task->cq_tail;
}

inline int reactor_cq_size(struct reactor_task *task)
{
	int size;
	if (task->cq_tail <= task->cq_head)
		size = task->cq_head - task->cq_tail;
	else
		size = task->qdepth - task->cq_tail + task->cq_head;

	assert(size >= 0);

	return size;
}

struct io_job *reactor_sq_get_req(struct reactor_task *task)
{
	struct io_job *req = NULL;

	pthread_mutex_lock(&task->sq_mutex);

	if (reactor_sq_is_empty(task))
		goto UNLOCK;

	req = task->sq[task->sq_tail];
	task->sq[task->sq_tail] = NULL;
	task->sq_tail = (task->sq_tail + 1) % task->qdepth;

	//dprintf_info(REACTOR, " SQ size = %d \n", reactor_sq_size(task));

UNLOCK:
	pthread_mutex_unlock(&task->sq_mutex);

	return req;
}

int reactor_sq_put_req(struct reactor_task *task, struct io_job *req)
{
	int res = 0;

	pthread_mutex_lock(&task->sq_mutex);

	if (reactor_sq_is_full(task)) {
		res = -1;
		goto UNLOCK;
	}

	task->sq[task->sq_head] = req;
	task->sq_head = (task->sq_head + 1) % task->qdepth;

	//dprintf_info(REACTOR, " SQ size = %d \n", reactor_sq_size(task));

UNLOCK:
	pthread_mutex_unlock(&task->sq_mutex);

	return res;
}

int reactor_cq_put_req(struct reactor_task *task, struct io_job *req)
{
	int res = 0;

	pthread_mutex_lock(&task->cq_mutex);

	if (reactor_cq_is_full(task)) {
		dprintf_error(REACTOR, " CQ is full\n");
		res = -1;
		goto UNLOCK;
	}

	task->cq[task->cq_head] = req;
	task->cq_head = (task->cq_head + 1) % task->qdepth;

	//dprintf_info(REACTOR, " CQ size = %d\n", reactor_cq_size(task));

	pthread_cond_signal(&task->cq_cond);

UNLOCK:
	pthread_mutex_unlock(&task->cq_mutex);

	return res;
}

struct reactor_task *reactor_alloc_task(struct io_target *target, int32_t qdepth)
{
	struct reactor_task	*task = NULL;
	int i;

	if (qdepth + 1 > REACTOR_MAX_REQUEST) {
		dprintf(REACTOR, "qdepth cannot be greater than %d max qdepth\n", REACTOR_MAX_REQUEST);
		return NULL;
	}

	/* alloc task */
	if (rte_mempool_get(target->task_pool, (void **)&task) != 0 || task == NULL) {
		printf("Task pool allocation failed\n");
		abort();
	}

	//dprintf_info(REACTOR, " allocated task = %p\n", task);

	/* init task */
	task->target = target;
	task->qdepth = qdepth + 1;

	for (i = 0;i < qdepth; i++) {
		task->sq[i] = NULL;
	}
	task->sq_head = 0;
	task->sq_tail = 0;
	pthread_mutex_init(&task->sq_mutex, NULL);
	pthread_cond_init(&task->sq_cond, NULL);

	for (i = 0;i < qdepth; i++) {
		task->cq[i] = NULL;
	}
	task->cq_head = 0;
	task->cq_tail = 0;

	pthread_mutex_init(&task->cq_mutex, NULL);
	pthread_cond_init(&task->cq_cond, NULL);

	return task;
}

void reactor_free_task(struct io_target *target, struct reactor_task *task)
{
	rte_mempool_put(target->task_pool, task);
}

struct io_job *reactor_make_single_req(struct io_target *target, uint64_t offset, int bytes, void *buf, int type)
{
	struct io_job *req;

	if (rte_mempool_get_bulk(target->req_pool, (void **)&req, 1) != 0 || req == NULL) {
		printf("request pool allocation failed req = %p\n", req);
		abort();
	}

	req->offset = offset;
	req->bytes = bytes;
	req->iov[0].iov_base = buf;
	req->iov[0].iov_len = bytes;
	req->iovcnt = 1;
	req->req_type = type;
	req->cb = reactor_bio_cb;

	return req;
}

void reactor_print_reqs(struct io_job **reqs, int count)
{
	struct io_job *req;
	int i;

	for (i = 0; i < count; i++) {
		req = reqs[i];
		dprintf_info(REACTOR, " req[%d] offset %lu bytes %u\n", i, req->offset, req->bytes);
	}
}

void reactor_free_reqs(struct io_target *target, struct io_job **reqs, int count)
{
	//reactor_print_reqs(reqs, count);
	rte_mempool_put_bulk(target->req_pool, (void **)(reqs), count);
}

int32_t reactor_submit_reqs(struct io_target *target, struct reactor_task *task, struct io_job **reqs, int count)
{
	struct spdk_event *event;
	struct io_job *req;
	int i;

	for (i = 0; i < count; i++) {

		req = reqs[i];
		req->task = task;

		//if (req->req_type == SPDK_BDEV_IO_TYPE_READ)
		//	dprintf_info(REACTOR, " req = %p blkno %ld\n", req, req->offset);

		if (reactor_sq_put_req(task, req) < 0) {
			dprintf_error(REACTOR, " SQ is full\n");
			return -1;
		}
	}

	assert(target->lcore == REACTOR_LCORE);
	/* enqueue reqs and alloc event */
	event = spdk_event_allocate(target->lcore, reactor_submit_on_core,
					target, task);
	spdk_event_call(event);

	//dprintf_info(REACTOR, " wait lcore = %d\n", rte_lcore_id());
	return 0;
}

int reactor_cq_get_reqs(struct reactor_task *task, struct io_job **reqs, int min_reqs, int max_reqs)
{
	int i;
	int n;

	if (min_reqs == 0) {
		dprintf_error(REACTOR, " min_reqs (%d) is invalid.\n", min_reqs);
		return 0;
	}

	if (max_reqs > task->qdepth) {
		dprintf_error(REACTOR, " max_reqs (%d) is greater than qdepth (%d).\n", max_reqs, task->qdepth);
		assert(0);
		return 0;
	}

	pthread_mutex_lock(&task->cq_mutex);

	while(reactor_cq_size(task) < min_reqs) {
		pthread_cond_wait(&task->cq_cond, &task->cq_mutex);
	}

	n = reactor_cq_size(task);

	n = (n > max_reqs) ? max_reqs : n;

	for (i = 0; i < n; i++) {
		reqs[i] = task->cq[task->cq_tail];
		task->cq[task->cq_tail] = NULL;
		task->cq_tail = (task->cq_tail + 1) % task->qdepth;
	}

	//dprintf_info(REACTOR, " processed requests = %d \n", n);

	/* obtain n requests = task->cq_head - task->cq_tail */

	pthread_mutex_unlock(&task->cq_mutex);

	return n;
}

int reactor_sync_read_blk(struct io_target *target, long block, int count, void *buf)
{
	struct reactor_task *task;
	struct io_job *req;
	int num_reqs = 1;
	int ptr;
	int ret;

	/*if (block == 1)
		dprintf_info(REACTOR, " sync read block %ld count %d\n", block, count);*/

	task = reactor_alloc_task(target, num_reqs);

	req = reactor_make_single_req(target, block * NV_BLOCK_SIZE, 
										count * NV_BLOCK_SIZE, buf, 
										SPDK_BDEV_IO_TYPE_READ);

	reactor_submit_reqs(target, task, &req, num_reqs);

	ptr = 0;
	while (ptr < num_reqs) {
		ptr += reactor_cq_get_reqs(task, &req + ptr, 1, num_reqs - ptr);
	}

	ret = req->ret;
	reactor_free_reqs(target, &req, ptr);
	reactor_free_task(target, task);

	return ret;
}

int reactor_sync_write_blk(struct io_target *target, long block, int count, void *buf)
{
	struct reactor_task *task;
	struct io_job *req;
	int num_reqs = 1;
	int ptr;
	int ret;

	/*if (block == 1)
		dprintf_info(REACTOR, " sync write block %ld count %d\n", block, count);*/

	task = reactor_alloc_task(target, num_reqs);

	req = reactor_make_single_req(target, block * NV_BLOCK_SIZE, 
										count * NV_BLOCK_SIZE, buf, 
										SPDK_BDEV_IO_TYPE_WRITE);

	reactor_submit_reqs(target, task, &req, num_reqs);

	ptr = 0;
	while (ptr < num_reqs) {
		ptr += reactor_cq_get_reqs(task, &req + ptr, 1, num_reqs - ptr);
	}

	ret = req->ret;
	reactor_free_reqs(target, &req, ptr);
	reactor_free_task(target, task);

	return ret;
}

int reactor_sync_flush(struct io_target *target)
{
	struct reactor_task *task;
	struct io_job *req;
	int num_reqs = 1;
	int ptr;
	int ret;

	task = reactor_alloc_task(target, num_reqs);

	req = reactor_make_single_req(target, 0, 
										1, NULL, 
										SPDK_BDEV_IO_TYPE_FLUSH);

	reactor_submit_reqs(target, task, &req, num_reqs);

	ptr = 0;
	while (ptr < num_reqs) {
		ptr += reactor_cq_get_reqs(task, &req + ptr, 1, num_reqs - ptr);
	}

	ret = req->ret;
	reactor_free_reqs(target, &req, ptr);
	reactor_free_task(target, task);

	return ret;
}

struct io_target *reactor_construct_targets(void)
{
	int index = 0;
	struct spdk_bdev *bdev;
	struct io_target *target;
	int rc;

	bdev = spdk_bdev_first();
	while (bdev != NULL) {

#if 0
		if (!spdk_bdev_claim(bdev, NULL, NULL)) {
			bdev = spdk_bdev_next(bdev);
			continue;
		}
#endif

		printf(" bdev name = %p \n", bdev);

		target = malloc(sizeof(struct io_target));
		if (!target) {
			fprintf(stderr, "Unable to allocate memory for new target.\n");
			/* Return immediately because all mallocs will presumably fail after this */
			return NULL;
		}

		target->bdev = bdev;

		rc = spdk_bdev_open(target->bdev, true, NULL, NULL, &target->desc);
		if (rc) {
			SPDK_ERRLOG("Unable to open bdev\n");
			free(target);
			return NULL;
		}
		/* Mapping each target to lcore */
		index = g_target_count % spdk_env_get_core_count();
		target->next = head[index];
		target->lcore = index;
		target->io_completed = 0;
		target->current_queue_depth = 0;
		target->offset_in_ios = 0;

		target->is_draining = false;
		target->run_timer = NULL;
		target->reset_timer = NULL;

		target->task_pool = rte_mempool_create("task_pool", 4096 * spdk_env_get_core_count(),
						   sizeof(struct reactor_task),
						   64, 0, NULL, NULL, NULL, NULL,
						   SOCKET_ID_ANY, 0);

		target->req_pool = rte_mempool_create("req_pool", 4096 * spdk_env_get_core_count(),
						   sizeof(struct io_job),
						   32, 0, NULL, NULL, NULL, NULL,
						   SOCKET_ID_ANY, 0);

		head[index] = target;
		g_target_count++;
		bdev = spdk_bdev_next(bdev);
#if 1
		if (bdev)
			dprintf_info(REACTOR, " The current version only supports single device.\n");
		return target;
#endif
	}
	return NULL;
}

void reactor_get_opts(const char *config_file, const char *cpumask, struct spdk_app_opts *opts)
{
	assert(opts != NULL);

	spdk_app_opts_init(opts);

	opts->name = "bdevtest";
	opts->config_file = config_file;
	opts->reactor_mask = cpumask;
}

void blockdev_heads_init(void)
{
	int i;

	for (i = 0; i < RTE_MAX_LCORE; i++) {
		head[i] = NULL;
	}
}

void reactor_submit_on_core(void *arg1, void *arg2)
{
	struct io_target *target = arg1;
	struct reactor_task *task = arg2;
#ifndef NVFUSE_USE_CEPH_SPDK
	int rc;
#else
	struct spdk_bdev_io *bdev_io;
#endif

#ifndef NVFUSE_USE_CEPH_SPDK
	target->ch = spdk_bdev_get_io_channel(target->desc);
#else
	target->ch = spdk_bdev_get_io_channel(target->desc, SPDK_IO_PRIORITY_DEFAULT);
#endif
	while (1) {
		struct io_job *req;
		
		req = reactor_sq_get_req(task);
		if (req == NULL)
			return;

		//dprintf_info(REACTOR, " rx offset = %ld\n", req->offset);
		assert(req->iovcnt);

#ifndef NVFUSE_USE_CEPH_SPDK
		if (req->req_type == SPDK_BDEV_IO_TYPE_READ) {
			rc = spdk_bdev_readv(target->desc, target->ch, req->iov, req->iovcnt, req->offset, 
					req->bytes, req->cb, req);
		} else if (req->req_type == SPDK_BDEV_IO_TYPE_WRITE) {
			rc = spdk_bdev_writev(target->desc, target->ch, req->iov, req->iovcnt, req->offset, 
					req->bytes, req->cb, req);
		} else if (req->req_type == SPDK_BDEV_IO_TYPE_FLUSH) {
			rc = spdk_bdev_flush(target->desc, target->ch, req->offset, 
					req->bytes, req->cb, req);
#else
		if (req->req_type == SPDK_BDEV_IO_TYPE_READ) {
			bdev_io = spdk_bdev_readv(target->desc, target->ch, req->iov, req->iovcnt, req->offset, 
					req->bytes, req->cb, req);
		} else if (req->req_type == SPDK_BDEV_IO_TYPE_WRITE) {
			bdev_io = spdk_bdev_writev(target->desc, target->ch, req->iov, req->iovcnt, req->offset, 
					req->bytes, req->cb, req);
		} else if (req->req_type == SPDK_BDEV_IO_TYPE_FLUSH) {
			bdev_io = spdk_bdev_flush(target->desc, target->ch, req->offset, 
					req->bytes, req->cb, req);

#endif
		} else {
			dprintf_error(REACTOR, " Unsupported I/O type = %d \n", req->req_type);
			assert(0);
		}

#ifndef NVFUSE_USE_CEPH_SPDK
		if (rc) {
#else
		if (!bdev_io) {
#endif
			printf("Failed to submit request offset = %lu, byte = %u\n", req->offset, req->bytes);
			target->is_draining = true;
			return;
		}
	}
}

#ifdef NVFUSE_USE_CEPH_SPDK
static inline s8 * spdk_bdev_get_name(struct spdk_bdev *bdev)
{
	return bdev->name;
}
#endif

void reactor_performance_dump(int io_time)
{
	uint32_t index;
	unsigned lcore_id;
	float io_per_second, mb_per_second = 0;
	float total_io_per_second, total_mb_per_second;
	struct io_target *target;

	total_io_per_second = 0;
	total_mb_per_second = 0;
	for (index = 0; index < spdk_env_get_core_count(); index++) {
		target = head[index];
		if (target != NULL) {
			lcore_id = target->lcore;
			printf("\r Logical core: %u\n", lcore_id);
		}
		while (target != NULL) {
			io_per_second = (float)target->io_completed /
					io_time;
			printf("\r %-20s: %10.2f IO/s %10.2f MB/s\n",
			       spdk_bdev_get_name(target->bdev), io_per_second,
			       mb_per_second);
			total_io_per_second += io_per_second;
			total_mb_per_second += mb_per_second;
			target = target->next;
		}
	}

	printf("\r =====================================================\n");
	printf("\r %-20s: %10.2f IO/s %10.2f MB/s\n",
	       "Total", total_io_per_second, total_mb_per_second);
	fflush(stdout);
}
