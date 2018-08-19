/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 27/11/2016
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
#include "pthread.h"
#include "nvfuse_types.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_reactor.h"
#include "list.h"

#ifndef __NVFUSE_AIO_H
#define __NVFUSE_AIO_H

#define NVFUSE_MAX_AIO_DEPTH		1024
#define NVFUSE_MAX_AIO_COMPLETION	1
#define NVFUSE_SUBMISSION_QUEUE		1
#define NVFUSE_COMPLETION_QUEUE		2

#define NVFUSE_AIO_STATUS_READY			0
#define NVFUSE_AIO_STATUS_SUMISSION		1
#define NVFUSE_AIO_STATUS_COMPLETION	2

struct nvfuse_aio_req {
	s32 fid; /* file descriptor */
	s32 opcode; /* Read or Write */
	void *buf; /* actual data buffer */
	s64 offset; /* start address in bytes */
	s64 bytes; /* number of bytes */
	s32 error; /* return error code */
	s32 status; /* status (e.g., ready, submitted, and completed) */

	struct list_head list; /* linked list */
	struct list_head bh_head; /* buffer head list */
	s32 bio_job_count;
	void(*actx_cb_func)(void *arg); /* callback function to process completion for each context */
	struct nvfuse_aio_queue *queue; /* aio queue pointer */
	struct nvfuse_superblock *sb; /* superblock pointer */
	u64 submit_tsc; /* measurement of latency */
	u64 complete_tsc; /* measurement of latency */

	void *tag1; /* keep track of temp pointer */
	void *tag2; /* keep track of temp pointer */
	void *tag3; /* keep track of temp pointer */
};

struct nvfuse_aio_queue {
	struct list_head asq_head; /* submission queue head */
	s32 asq_max_depth; /* maximum submission queue depth */
	s32 asq_cur_depth; /* current submission queue depth */

	struct list_head acq_head; /* completion queue head */
	s32 acq_max_depth; /* maximum completion queue depth */
	s32 acq_cur_depth; /* current completion queue depth */

	s32 total_bio_job_count;

	struct reactor_task *task;

	s32 max_completions;

	union perf_stat uni_stat;
	struct perf_stat_aio *aio_stat;

	u64 aio_cc_sum; /* average cpl count per poll*/
	u64 aio_cc_cnt;
};

s32 nvfuse_aio_queue_init(struct io_target *target, struct nvfuse_aio_queue *aioq, s32 max_depth);
void nvfuse_aio_queue_deinit(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq);
s32 nvfuse_aio_queue_enqueue(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq, s32 qtype);
s32 nvfuse_aio_queue_dequeue(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq, s32 qtype);
s32 nvfuse_aio_queue_move(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq, s32 qtype);
s32 nvfuse_aio_queue_submission(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq);
s32 nvfuse_aio_queue_completion(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq);

void nvfuse_aio_gen_dev_cpls(void *arg);
s32 nvfuse_aio_gen_dev_reqs(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq);
s32 nvfuse_aio_wait_dev_cpls(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq, s32 min_nr, s32 nr);

s32 nvfuse_aio_get_queue_depth_total(struct nvfuse_aio_queue *aioq);
s32 nvfuse_aio_get_queue_depth_type(struct nvfuse_aio_queue *aioq, s32 qtype);

s32 nvfuse_io_submit(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq, s32 nr, struct nvfuse_aio_req **list);
s32 nvfuse_io_getevents(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq, s32 min_nr, s32 nr, struct nvfuse_aio_req **list);

#endif
