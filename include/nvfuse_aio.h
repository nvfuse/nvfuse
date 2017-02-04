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
#include "list.h"

#ifndef __NVFUSE_AIO_H
#define __NVFUSE_AIO_H

#define NVFUSE_MAX_AIO_DEPTH		1024
#define NVFUSE_MAX_AIO_COMPLETION	1
#define NVFUSE_READY_QUEUE			0
#define NVFUSE_SUBMISSION_QUEUE		1
#define NVFUSE_COMPLETION_QUEUE		2

#define NVFUSE_AIO_STATUS_READY			0
#define NVFUSE_AIO_STATUS_SUMISSION		1
#define NVFUSE_AIO_STATUS_COMPLETION	2

struct nvfuse_aio_ctx {
	s32 actx_fid; /* file descriptor */
	s32 actx_opcode; /* Read or Write */
	void *actx_buf; /* actual data buffer */
	s64 actx_offset; /* start address in bytes */
	s64 actx_bytes; /* number of bytes */	
	s32 actx_error; /* return error code */
	s32 actx_status; /* status (e.g., ready, submitted, and completed) */
	struct list_head actx_list; /* linked list */
	struct list_head actx_bh_head; /* buffer head list */
	s32 actx_bh_count;
	void(*actx_cb_func)(void *arg); /* callback function to process completion for each context */
	struct nvfuse_aio_queue *actx_queue; /* aio queue pointer */
	struct nvfuse_superblock *actx_sb; /* superblock pointer */
	u64 actx_submit_tsc; /* measurement of latency */
	u64 actx_complete_tsc; /* measurement of latency */

	void *tag1; /* keep track of temp pointer */
	void *tag2; /* keep track of temp pointer */
	void *tag3; /* keep track of temp pointer */
};

struct nvfuse_aio_queue {
	struct list_head arq_head; /* ready queue head */
	s32 arq_max_depth; /* maximum ready queue depth */
	s32 arq_cur_depth; /* current ready queue depth */

	struct list_head asq_head; /* submission queue head */
	s32 asq_max_depth; /* maximum submission queue depth */
	s32 asq_cur_depth; /* current submission queue depth */

	struct list_head acq_head; /* completion queue head */
	s32 acq_max_depth; /* maximum completion queue depth */
	s32 acq_cur_depth; /* current completion queue depth */
	
	s32 aio_cur_depth;

	s32 max_completions;

	union perf_stat uni_stat;
	struct perf_stat_aio *aio_stat;

	u64 aio_cc_sum; /* average cpl count per poll*/
	u64 aio_cc_cnt;
};

s32 nvfuse_aio_queue_init(struct nvfuse_aio_queue *aioq, s32 max_depth);
void nvfuse_aio_queue_deinit(struct nvfuse_handle *nvh, struct nvfuse_aio_queue * aioq);
s32 nvfuse_aio_queue_enqueue(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_ctx *actx, s32 qtype);
s32 nvfuse_aio_queue_dequeue(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_ctx *actx, s32 qtype);
s32 nvfuse_aio_queue_move(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_ctx *actx, s32 qtype);
s32 nvfuse_aio_queue_submission(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq);
s32 nvfuse_aio_queue_completion(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq);

void nvfuse_aio_gen_dev_cpls_buffered(void *arg);
void nvfuse_aio_gen_dev_cpls_directio(void *arg);
s32 nvfuse_aio_gen_dev_reqs_buffered(struct nvfuse_superblock *sb, struct nvfuse_aio_ctx *actx);
s32 nvfuse_aio_gen_dev_reqs_directio(struct nvfuse_superblock *sb, struct nvfuse_aio_ctx *actx);
s32 nvfuse_aio_wait_dev_cpls(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq);

s32 nvfuse_aio_get_queue_depth_total(struct nvfuse_aio_queue *aioq);
s32 nvfuse_aio_get_queue_depth_type(struct nvfuse_aio_queue *aioq, s32 qtype);
s32 nvfuse_aio_alloc_req(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq, void *user_ctx);

#endif
