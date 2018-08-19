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
#include "spdk/bdev.h"
#include "spdk/event.h"

#ifndef __NVFUSE_REACTOR__
#define __NVFUSE_REACTOR__

#define REACTOR_MAX_REQUEST 1024
#define REACTOR_BUFFER_IOVS 4

struct io_target {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	struct io_target	*next;
	unsigned		lcore;
	int			io_completed;
	int			current_queue_depth;
	uint64_t		size_in_ios;
	uint64_t		offset_in_ios;
	bool			is_draining;
	struct spdk_poller	*run_timer;
	struct spdk_poller	*reset_timer;
	struct rte_mempool *task_pool;
	struct rte_mempool *req_pool;
};

struct reactor_task {
    struct io_target    *target;
    int                 qdepth;

    struct io_job *sq[REACTOR_MAX_REQUEST];
    int32_t             sq_head;
    int32_t             sq_tail;
    pthread_mutex_t     sq_mutex;
    pthread_cond_t      sq_cond;

    struct io_job *cq[REACTOR_MAX_REQUEST];
    int32_t             cq_head;
    int32_t             cq_tail;
    pthread_mutex_t     cq_mutex;
    pthread_cond_t      cq_cond;
};

struct io_job;

int32_t reactor_submit_reqs(struct io_target *target, struct reactor_task *task, struct io_job **reqs, int count);
int reactor_cq_get_reqs(struct reactor_task *task, struct io_job **reqs, int min_reqs, int max_reqs);
void reactor_make_reqs(struct io_target *target, struct io_job **reqs, int count);
#ifndef NVFUSE_USE_CEPH_SPDK
void reactor_bio_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
#else
void reactor_bio_cb(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status success, void *cb_arg);
#endif
struct reactor_task *reactor_alloc_task(struct io_target *target, int32_t qdepth);
void reactor_free_task(struct io_target *target, struct reactor_task *task);
void reactor_free_reqs(struct io_target *target, struct io_job **reqs, int count);
struct io_job *reactor_make_single_req(struct io_target *target, uint64_t offset, int bytes, void *buf, int type);
void reactor_print_reqs(struct io_job **reqs, int count);

int reactor_sq_is_empty(struct reactor_task *task);
int reactor_sq_is_full(struct reactor_task *task);
int reactor_cq_is_empty(struct reactor_task *task);
int reactor_cq_is_full(struct reactor_task *task);
int reactor_cq_size(struct reactor_task *task);
struct io_job *reactor_sq_get_req(struct reactor_task *task);
int reactor_sq_put_req(struct reactor_task *task, struct io_job *req);
struct io_job *reactor_cq_get_req(struct reactor_task *task);
int reactor_cq_put_req(struct reactor_task *task, struct io_job *req);

int reactor_sync_read_blk(struct io_target *target, long block, int count, void *buf);
int reactor_sync_write_blk(struct io_target *target, long block, int count, void *buf);
int reactor_sync_flush(struct io_target *target);
struct io_target * reactor_construct_targets(void);
void reactor_get_opts(const char *config_file, const char *cpumask, struct spdk_app_opts *opts);
void blockdev_heads_init(void);
void reactor_submit_on_core(void *arg1, void *arg2);
void reactor_performance_dump(int io_time);

#endif /* __NVFUSE_REACTOR__ */
