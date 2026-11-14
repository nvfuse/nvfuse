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
#include <semaphore.h>

#include "spdk/bdev.h"
#include "spdk/version.h"
#if SPDK_VERSION_MAJOR < 20
#include "spdk/copy_engine.h"
#endif
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/util.h"
#if SPDK_VERSION_MAJOR >= 20
#include "spdk/thread.h"
#else
#include "spdk/io_channel.h"
#endif
#include "nvfuse_config.h"
#include "nvfuse_debug.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_reactor.h"
#ifdef NVFUSE_PRIMARY_JOURNAL
#include "nvfuse_primary_journal.h"
#endif

#define REACTOR_LCORE	0
struct io_target *head[RTE_MAX_LCORE];
static int g_target_count = 0;

#if SPDK_VERSION_MAJOR >= 20
static void reactor_bdev_event(enum spdk_bdev_event_type type,
			      struct spdk_bdev *bdev, void *arg)
{
	struct io_target *target = arg;
	(void)bdev;
	if (type == SPDK_BDEV_EVENT_REMOVE)
		target->is_draining = true;
}
#endif

static void reactor_task_disable_polling(struct io_target *, struct reactor_task *);

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
    if (task->polling) {
        int head = __atomic_load_n(&task->sq_head, __ATOMIC_ACQUIRE);
        int tail = __atomic_load_n(&task->sq_tail, __ATOMIC_ACQUIRE);
        return head >= tail ? head - tail : task->qdepth - tail + head;
    }
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
    if (task->polling) {
        int head = __atomic_load_n(&task->cq_head, __ATOMIC_ACQUIRE);
        int tail = __atomic_load_n(&task->cq_tail, __ATOMIC_ACQUIRE);
        return head >= tail ? head - tail : task->qdepth - tail + head;
    }
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
    if (task->polling) {
        int tail = __atomic_load_n(&task->sq_tail, __ATOMIC_RELAXED);
        int head = __atomic_load_n(&task->sq_head, __ATOMIC_ACQUIRE);
        if (head == tail) return NULL;
        struct io_job *req = task->sq[tail];
        __atomic_store_n(&task->sq_tail, (tail + 1) % task->qdepth, __ATOMIC_RELEASE);
        return req;
    }
	struct io_job *req = NULL;

	pthread_mutex_lock(&task->sq_mutex);

	if (reactor_sq_is_empty(task)) {
		/* Clear under the producer's lock so an enqueue cannot lose its
		 * wakeup when this drain returns. */
		task->sq_scheduled = 0;
		goto UNLOCK;
	}

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
    if (task->polling) return reactor_submit_reqs(task->target, task, &req, 1);
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
    if (task->polling) {
        int head = __atomic_load_n(&task->cq_head, __ATOMIC_RELAXED);
        int tail = __atomic_load_n(&task->cq_tail, __ATOMIC_ACQUIRE);
        int next = (head + 1) % task->qdepth;
        assert(next != tail);
        if (next == tail) return -1;
        task->cq[head] = req;
        __atomic_store_n(&task->cq_head, next, __ATOMIC_RELEASE);
        return 0;
    }
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
    task->polling = 0; task->poll_next = NULL;
	task->qdepth = qdepth + 1;

	for (i = 0;i < qdepth; i++) {
		task->sq[i] = NULL;
	}
	task->sq_head = 0;
	task->sq_tail = 0;
	task->sq_scheduled = 0;
	task->sq_batches = task->sq_notifications = 0;
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
    if (task->polling) reactor_task_disable_polling(target, task);
	if (task->qdepth > 2 && task->sq_batches)
		printf("NVFUSE_REACTOR batches=%lu notifications=%lu\n",
		       task->sq_batches, task->sq_notifications);
	pthread_mutex_destroy(&task->sq_mutex);
	pthread_cond_destroy(&task->sq_cond);
	pthread_mutex_destroy(&task->cq_mutex);
	pthread_cond_destroy(&task->cq_cond);
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
	req->buf = buf;
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

#if SPDK_VERSION_MAJOR >= 20
static void reactor_submit_on_thread(void *arg)
{
	struct reactor_task *task = arg;
	reactor_submit_on_core(task->target, task);
}
#endif

int32_t reactor_submit_reqs(struct io_target *target, struct reactor_task *task, struct io_job **reqs, int count)
{
#if SPDK_VERSION_MAJOR < 20
	struct spdk_event *event;
#endif
	struct io_job *req;
	int i, head, rc = 0;

#ifdef NVFUSE_PRIMARY_JOURNAL
    for (int n = 0; n < count; n++) {
        int error = nvfuse_primary_journal_stage(target, reqs[n]);
        if (error) {
            fprintf(stderr, "NVFUSE_JOURNAL staging failed: %d; recovery required\n", error);
            abort(); /* Legacy writeback cannot safely unwind partial failures. */
        }
    }
#endif

    if (task->polling) {
        int head = __atomic_load_n(&task->sq_head, __ATOMIC_RELAXED);
        int tail = __atomic_load_n(&task->sq_tail, __ATOMIC_ACQUIRE);
        int used = head >= tail ? head - tail : task->qdepth - tail + head;
        if (count < 0 || count > task->qdepth - 1 - used) return -1;
        for (int n = 0; n < count; n++) {
            reqs[n]->task = task; task->sq[head] = reqs[n];
            head = (head + 1) % task->qdepth;
        }
        __atomic_store_n(&task->sq_head, head, __ATOMIC_RELEASE);
        if (count) task->sq_batches++;
        return 0;
    }

	/* Publish the entire batch or none: callers may release all jobs on
	 * failure. Hold the lock through notification so a failed send can
	 * roll back before the I/O thread consumes any part of this batch. */
	pthread_mutex_lock(&task->sq_mutex);
	if (count < 0 || count > task->qdepth - 1 - reactor_sq_size(task)) {
		pthread_mutex_unlock(&task->sq_mutex);
		return -1;
	}
	head = task->sq_head;
	for (i = 0; i < count; i++) {

		req = reqs[i];
		req->task = task;

		//dprintf_info(REACTOR, " req = %p blkno %ld type %d\n", req, req->offset, req->req_type);

		task->sq[task->sq_head] = req;
		task->sq_head = (task->sq_head + 1) % task->qdepth;
	}

	/* enqueue reqs and alloc event */
#if SPDK_VERSION_MAJOR >= 20
	if (count && !task->sq_scheduled) {
		task->sq_scheduled = 1;
		rc = spdk_thread_send_msg(target->io_thread, reactor_submit_on_thread, task);
		if (rc) {
			task->sq_head = head;
			task->sq_scheduled = 0;
		} else {
			task->sq_notifications++;
		}
	}
	if (!rc && count) task->sq_batches++;
#else
	event = spdk_event_allocate(target->lcore, reactor_submit_on_core,
					target, task);
	spdk_event_call(event);
	(void)head;
#endif
	pthread_mutex_unlock(&task->sq_mutex);

	//dprintf_info(REACTOR, " wait lcore = %d\n", rte_lcore_id());
	return rc;
}

int reactor_cq_get_reqs(struct reactor_task *task, struct io_job **reqs, int min_reqs, int max_reqs)
{
	int i;
	int n;

	if (min_reqs < 0 || max_reqs < min_reqs) {
		dprintf_error(REACTOR, " invalid completion range %d..%d\n", min_reqs, max_reqs);
		return 0;
	}

	if (max_reqs > task->qdepth) {
		dprintf_error(REACTOR, " max_reqs (%d) is greater than qdepth (%d).\n", max_reqs, task->qdepth);
		assert(0);
		return 0;
	}

    if (task->polling) {
        int available;
        while ((available = reactor_cq_size(task)) < min_reqs) {
            __atomic_signal_fence(__ATOMIC_ACQ_REL);
        }
        int count = available < max_reqs ? available : max_reqs;
        int tail = __atomic_load_n(&task->cq_tail, __ATOMIC_RELAXED);
        for (int n = 0; n < count; n++) {
            reqs[n] = task->cq[tail]; tail = (tail + 1) % task->qdepth;
        }
        __atomic_store_n(&task->cq_tail, tail, __ATOMIC_RELEASE);
        return count;
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
#ifdef NVFUSE_PRIMARY_JOURNAL
    if (nvfuse_primary_journal_sync(target, (int64_t)block * NV_BLOCK_SIZE,
            (size_t)count * NV_BLOCK_SIZE, buf, SPDK_BDEV_IO_TYPE_READ))
        return block ? 0 : nvfuse_primary_journal_read_header(target, buf, (size_t)count * NV_BLOCK_SIZE);
#endif
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

#ifdef NVFUSE_PRIMARY_JOURNAL
    if (!ret && !block)
        ret = nvfuse_primary_journal_read_header(target, buf, (size_t)count * NV_BLOCK_SIZE);
#endif
	return ret;
}

int reactor_sync_write_blk(struct io_target *target, long block, int count, void *buf)
{
#ifdef NVFUSE_PRIMARY_JOURNAL
    if (nvfuse_primary_journal_sync(target, (int64_t)block * NV_BLOCK_SIZE,
            (size_t)count * NV_BLOCK_SIZE, buf, SPDK_BDEV_IO_TYPE_WRITE)) return 0;
#endif
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
#ifdef NVFUSE_PRIMARY_JOURNAL
    if (nvfuse_primary_journal_sync(target, 0, NV_BLOCK_SIZE, NULL, SPDK_BDEV_IO_TYPE_FLUSH)) return 0;
#endif
	struct reactor_task *task;
	struct io_job *req;
	int num_reqs = 1;
	int ptr;
	int ret;

	task = reactor_alloc_task(target, num_reqs);

	req = reactor_make_single_req(target, 0, 
										4096, NULL, 
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
		const char *selected = getenv("NVFUSE_SPDK_BDEV");
		if (selected && strcmp(selected, spdk_bdev_get_name(bdev))) {
			bdev = spdk_bdev_next(bdev);
			continue;
		}

#if 0
		if (!spdk_bdev_claim(bdev, NULL, NULL)) {
			bdev = spdk_bdev_next(bdev);
			continue;
		}
#endif

		printf(" bdev name = %p \n", bdev);

		target = calloc(1, sizeof(struct io_target));
		if (!target) {
			fprintf(stderr, "Unable to allocate memory for new target.\n");
			/* Return immediately because all mallocs will presumably fail after this */
			return NULL;
		}

		target->bdev = bdev;

#if SPDK_VERSION_MAJOR >= 20
		rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), true,
				       reactor_bdev_event, target, &target->desc);
#else
		rc = spdk_bdev_open(target->bdev, true, NULL, NULL, &target->desc);
#endif
		if (rc) {
			SPDK_ERRLOG("Unable to open bdev\n");
			free(target);
			return NULL;
		}
		/* Mapping each target to lcore */
		index = spdk_env_get_first_core();
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
		if (!target->task_pool || !target->req_pool) {
			rte_mempool_free(target->task_pool);
			rte_mempool_free(target->req_pool);
			spdk_bdev_close(target->desc);
			free(target);
			return NULL;
		}
#if SPDK_VERSION_MAJOR >= 20
		struct spdk_cpuset cpuset;
		spdk_cpuset_zero(&cpuset);
		spdk_cpuset_set_cpu(&cpuset, target->lcore, true);
		target->io_thread = spdk_thread_create("nvfuse_io", &cpuset);
		if (!target->io_thread) {
			rte_mempool_free(target->task_pool);
			rte_mempool_free(target->req_pool);
			spdk_bdev_close(target->desc);
			free(target);
			return NULL;
		}
#endif

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

#if SPDK_VERSION_MAJOR >= 20
static int reactor_poll_data_queues(void *arg)
{
    struct io_target *target = arg;
    int work = 0;
    for (struct reactor_task *task = target->poll_tasks; task; task = task->poll_next) {
        if (reactor_sq_size(task)) {
            reactor_submit_on_core(target, task); work = 1;
        }
    }
    return work ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
struct poll_control { struct io_target *target; struct reactor_task *task; sem_t done; int enable, result; };
static void reactor_poll_control(void *arg)
{
    struct poll_control *control = arg;
    struct io_target *target = control->target;
    struct reactor_task *task = control->task;
    if (control->enable) {
        if (!target->queue_poller)
            target->queue_poller = spdk_poller_register(reactor_poll_data_queues, target, 0);
        if (!target->queue_poller) { control->result = -ENOMEM; goto done; }
        task->polling = 1;
        task->poll_next = target->poll_tasks; target->poll_tasks = task;
    } else {
        struct reactor_task **link = &target->poll_tasks;
        while (*link && *link != task) link = &(*link)->poll_next;
        assert(*link && !reactor_sq_size(task) && !reactor_cq_size(task));
        *link = task->poll_next;
        task->polling = 0; task->poll_next = NULL;
        if (!target->poll_tasks) spdk_poller_unregister(&target->queue_poller);
    }
done:
    sem_post(&control->done);
}
static int reactor_set_polling(struct io_target *target, struct reactor_task *task, int enable)
{
    struct poll_control control = {.target = target, .task = task, .enable = enable};
    if (spdk_get_thread() == target->io_thread) return -EINVAL;
    if (sem_init(&control.done, 0, 0)) return -errno;
    int rc = spdk_thread_send_msg(target->io_thread, reactor_poll_control, &control);
    if (!rc) {
        while (sem_wait(&control.done) && errno == EINTR) {}
        rc = control.result;
    }
    sem_destroy(&control.done);
    return rc;
}
#endif
int reactor_task_enable_polling(struct io_target *target, struct reactor_task *task)
{
#if SPDK_VERSION_MAJOR >= 20
    if (task->polling || task->sq_batches) return -EINVAL;
    return reactor_set_polling(target, task, 1);
#else
    (void)target; (void)task; return -ENOTSUP;
#endif
}
static void reactor_task_disable_polling(struct io_target *target, struct reactor_task *task)
{
#if SPDK_VERSION_MAJOR >= 20
    if (reactor_set_polling(target, task, 0)) abort();
#else
    (void)target; (void)task; abort();
#endif
}

static void reactor_idle_done(void *arg1, void *arg2)
{
    (void)arg2;
    sem_post(arg1);
}
#if SPDK_VERSION_MAJOR >= 20
static void reactor_idle_on_thread(void *arg)
{
    reactor_idle_done(arg, NULL);
}
#endif
void reactor_wait_idle(struct io_target *target)
{
    sem_t done;
    if (sem_init(&done, 0, 0)) abort();
#if SPDK_VERSION_MAJOR >= 20
    if (spdk_get_thread() == target->io_thread ||
        spdk_thread_send_msg(target->io_thread, reactor_idle_on_thread, &done)) abort();
#else
    if (rte_lcore_id() == target->lcore) abort();
    spdk_event_call(spdk_event_allocate(target->lcore, reactor_idle_done, &done, NULL));
#endif
    int rc;
    do { rc = sem_wait(&done); } while (rc && errno == EINTR);
    if (rc) abort();
    sem_destroy(&done);
}

static void reactor_release_channel(void *arg1, void *arg2)
{
	struct io_target *target = arg1;
	if (target->ch) {
		spdk_put_io_channel(target->ch);
		target->ch = NULL;
	}
	sem_post(arg2);
}

#if SPDK_VERSION_MAJOR >= 20
struct channel_release { struct io_target *target; sem_t *done; };
static void reactor_release_on_thread(void *arg)
{
	struct channel_release *release = arg;
	reactor_release_channel(release->target, release->done);
	spdk_thread_exit(spdk_get_thread());
}
#endif

void reactor_destroy_target(struct io_target *target)
{
	sem_t done;
	if (!target)
		return;
#ifdef NVFUSE_PRIMARY_JOURNAL
    nvfuse_primary_journal_release(target);
#endif
	sem_init(&done, 0, 0);
#if SPDK_VERSION_MAJOR >= 20
	struct channel_release release = {target, &done};
	if (spdk_thread_send_msg(target->io_thread, reactor_release_on_thread, &release))
		abort();
#else
	if (rte_lcore_id() == target->lcore)
		reactor_release_channel(target, &done);
	else
		spdk_event_call(spdk_event_allocate(target->lcore,
				 reactor_release_channel, target, &done));
#endif
	while (sem_wait(&done) && errno == EINTR) {}
	sem_destroy(&done);
	spdk_bdev_close(target->desc);
	rte_mempool_free(target->req_pool);
	rte_mempool_free(target->task_pool);
	head[target->lcore] = NULL;
	g_target_count--;
	free(target);
}

void reactor_get_opts(const char *config_file, const char *cpumask, struct spdk_app_opts *opts)
{
	assert(opts != NULL);

#if SPDK_VERSION_MAJOR >= 20
	spdk_app_opts_init(opts, sizeof(*opts));
#else
	spdk_app_opts_init(opts);
#endif

	opts->name = "bdevtest";
#if SPDK_VERSION_MAJOR >= 20
	opts->json_config_file = config_file;
#else
	opts->config_file = config_file;
#endif
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
	if (!target->ch)
		target->ch = spdk_bdev_get_io_channel(target->desc);
#else
	target->ch = spdk_bdev_get_io_channel(target->desc, SPDK_IO_PRIORITY_DEFAULT);
#endif
	while (1) {
		struct io_job *req;
		
		req = reactor_sq_get_req(task);
		if (req == NULL)
			return;
#ifdef NVFUSE_PRIMARY_JOURNAL
        if (req->journal_staged) {
            reactor_cq_put_req(task, req);
            continue;
        }
#endif
		target->current_queue_depth++;

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
			printf("Failed to submit request offset = %lu, byte = %u (rc %d)\n", req->offset, req->bytes, rc);
			target->is_draining = true;
			assert(0);
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
