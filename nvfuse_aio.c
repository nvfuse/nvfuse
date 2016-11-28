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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#include "nvfuse_core.h"
#include "nvfuse_aio.h"
#include "nvfuse_api.h"
#include "nvfuse_buffer_cache.h"

s32 nvfuse_aio_queue_init(struct nvfuse_aio_queue * aioq, s32 max_depth)
{
	/* Ready Queue */
	INIT_LIST_HEAD(&aioq->arq_head);
	aioq->arq_cur_depth = 0;
	aioq->arq_max_depth = max_depth > NVFUSE_MAX_AIO_DEPTH ? NVFUSE_MAX_AIO_DEPTH : max_depth;
	
	/* Submission Queue */
	INIT_LIST_HEAD(&aioq->asq_head);
	aioq->asq_cur_depth = 0;
	aioq->asq_max_depth = max_depth > NVFUSE_MAX_AIO_DEPTH ? NVFUSE_MAX_AIO_DEPTH : max_depth;

	/* Completion Queue */
	INIT_LIST_HEAD(&aioq->acq_head);
	aioq->acq_cur_depth = 0;
	aioq->acq_max_depth = max_depth > NVFUSE_MAX_AIO_DEPTH ? NVFUSE_MAX_AIO_DEPTH : max_depth;

	return 0;
}

s32 nvfuse_aio_queue_enqueue(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_ctx * actx, s32 qtype)
{
	actx->actx_queue = aioq;
	actx->actx_status = qtype;

	switch (qtype) {
		case NVFUSE_READY_QUEUE:
			if (aioq->arq_cur_depth == aioq->arq_max_depth)
			{
				printf(" aio ready queue is full %d\n", aioq->arq_cur_depth);
				return -1;
			}
			list_add_tail(&actx->actx_list, &aioq->arq_head);
			aioq->arq_cur_depth++;
			break;
		case NVFUSE_SUBMISSION_QUEUE:
			if (aioq->asq_cur_depth == aioq->asq_max_depth)
			{
				printf(" aio submission queue is full %d\n", aioq->asq_cur_depth);
				return -1;
			}
			list_add_tail(&actx->actx_list, &aioq->asq_head);
			aioq->asq_cur_depth++;
			break;
		case NVFUSE_COMPLETION_QUEUE:
			if (aioq->acq_cur_depth == aioq->acq_max_depth)
			{
				printf(" aio completion queue is full %d\n", aioq->acq_cur_depth);
				return -1;
			}
			list_add_tail(&actx->actx_list, &aioq->acq_head);
			aioq->acq_cur_depth++;
			break;
	default:
		printf(" invalid aio qtype = %d", qtype);
		return -1;
	}

	return 0;
}

s32 nvfuse_aio_queue_dequeue(struct nvfuse_aio_queue * aioq, struct nvfuse_aio_ctx * actx, s32 qtype)
{
	switch (qtype) {
	case NVFUSE_READY_QUEUE:		
		list_del(&actx->actx_list);
		aioq->arq_cur_depth--;
		assert(aioq->arq_cur_depth >= 0);
		break;
	case NVFUSE_SUBMISSION_QUEUE:		
		list_del(&actx->actx_list);
		aioq->asq_cur_depth--;
		assert(aioq->asq_cur_depth >= 0);
		break;
	case NVFUSE_COMPLETION_QUEUE:		
		list_del(&actx->actx_list);
		aioq->acq_cur_depth--;
		assert(aioq->acq_cur_depth >= 0);
		break;
	default:
		printf(" invalid aio qtype = %d", qtype);
		return -1;
	}
		
	return 0;
}

s32 nvfuse_aio_queue_move(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_ctx *actx, s32 qtype)
{
	int res;

	res = nvfuse_aio_queue_dequeue(aioq, actx, actx->actx_status);
	if (res)
		return res;

	res = nvfuse_aio_queue_enqueue(aioq, actx, qtype);
	if (res)
		return res;

	return 0;
}

void nvfuse_aio_gen_dev_cpls(void *arg)
{
	struct list_head *head, *ptr, *temp;
	struct nvfuse_buffer_head *bh;
	struct nvfuse_buffer_cache *bc;
	struct nvfuse_aio_ctx *actx;

	/* casting */
	actx = (struct nvfuse_aio_ctx *)arg;

	/* release bh*/
	head = &actx->actx_bh_head;

	list_for_each_safe(ptr, temp, head) {
		bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_aio_list);
		
		list_del(&bh->bh_aio_list);

		bc = bh->bh_bc;

		if (actx->actx_opcode == WRITE)
		{
			bc->bc_ref--;
			nvfuse_remove_bh_in_bc(actx->actx_sb, bc);
			assert(bc->bc_dirty);
			bc->bc_dirty = 0;
		}
		else //if (actx->actx_opcode == READ)
		{
			bc->bc_load = 1;			
			nvfuse_release_bh(actx->actx_sb, bh, 0, CLEAN);
		}		
		if (bc->bc_ref) {
			printf("debug\n");
		}
		nvfuse_move_buffer_type(actx->actx_sb, bc, BUFFER_TYPE_CLEAN, INSERT_HEAD);
	}

	/* move actx to comletion queue*/
	nvfuse_aio_queue_move(actx->actx_queue, actx, NVFUSE_COMPLETION_QUEUE);
}

s32 nvfuse_aio_gen_dev_reqs(struct nvfuse_superblock *sb, struct nvfuse_aio_ctx *actx)
{
	struct list_head *head, *ptr, *temp;
	struct nvfuse_buffer_head *bh;
	struct nvfuse_buffer_cache *bc;

#if (NVFUSE_OS == NVFUSE_OS_LINUX)
	struct io_job *jobs;
	struct iocb **iocb;
	s32 count = 0;	
	s32 res;

	res = nvfuse_make_jobs(&jobs, actx->actx_bh_count);
	if (res < 0) {
		return res;
	}

	iocb = (struct iocb **)malloc(sizeof(struct iocb *) * actx->actx_bh_count);
	if (!iocb) {
		printf(" Malloc error: struct iocb\n");
		return -1;
	}
#endif

	head = &actx->actx_bh_head;
	list_for_each_safe(ptr, temp, head) {
		bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_aio_list);
		bc = bh->bh_bc;

#if (NVFUSE_OS == NVFUSE_OS_LINUX)
		(*(jobs + count)).offset = (long)bc->bc_pno * CLUSTER_SIZE;
		(*(jobs + count)).bytes = (size_t)CLUSTER_SIZE;
		(*(jobs + count)).ret = 0;
		(*(jobs + count)).req_type = (actx->actx_opcode == READ) ? READ : WRITE;
		(*(jobs + count)).buf = bc->bc_buf;
		(*(jobs + count)).complete = 0;
		(*(jobs + count)).tag = (void *)actx;

		iocb[count] = &((*(jobs + count)).iocb);
		count++;
#else
		if (actx->actx_opcode == READ)
			nvfuse_read_cluster(bc->bc_buf, bc->bc_pno, sb->io_manager);
		else
			nvfuse_write_cluster(bc->bc_buf, bc->bc_pno, sb->io_manager);
#endif
	}


#if (NVFUSE_OS == NVFUSE_OS_LINUX)
	count = 0;
	while(count < actx->actx_bh_count)
	{
		nvfuse_aio_prep(jobs + count, sb->io_manager);
		count ++;
	}

	nvfuse_aio_submit(iocb, actx->actx_bh_count, sb->io_manager);
	sb->io_manager->queue_cur_count += actx->actx_bh_count;

	actx->tag1 = (void *)jobs;
	actx->tag2 = (void *)iocb;

#elif (NVFUSE_OS == NVFUSE_OS_WINDOWS)
	nvfuse_aio_gen_dev_cpls((void *)actx);
#endif

	return 0;
}

s32 nvfuse_aio_queue_submission(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq)
{
	struct list_head *head, *ptr, *temp;
	struct nvfuse_aio_ctx *actx;	
	u32 bytes;
	s32 res;

	head = &aioq->arq_head;

	/* copy user data to buffer cache */
	list_for_each_safe(ptr, temp, head) {
		
		actx = (struct nvfuse_aio_ctx *)list_entry(ptr, struct nvfuse_aio_ctx, actx_list);
		
		printf(" aio ready queue : fd = %d offset = %ld, bytes = %ld, op = %d\n", actx->actx_fid, (long)actx->actx_offset,
			(long)actx->actx_bytes, actx->actx_opcode);

		if (actx->actx_opcode == READ)
			bytes = nvfuse_readfile_aio(nvh, actx->actx_fid, actx->actx_buf, actx->actx_bytes, actx->actx_offset);			
		else
			bytes = nvfuse_writefile_buffered_aio(nvh, actx->actx_fid, actx->actx_buf, actx->actx_bytes, actx->actx_offset);			
		
		
		if (bytes == actx->actx_bytes)		
			nvfuse_aio_queue_move(aioq, actx, NVFUSE_SUBMISSION_QUEUE);
		else
		{
			printf(" submission error\n ");
			return -1;
		}		
	}
	
	/* collect buffer */
	head = &aioq->asq_head;
	list_for_each_safe(ptr, temp, head) {
		actx = (struct nvfuse_aio_ctx *)list_entry(ptr, struct nvfuse_aio_ctx, actx_list);
		INIT_LIST_HEAD(&actx->actx_bh_head);

		printf(" aio submission queue : fd = %d offset = %ld, bytes = %ld, op = %d\n", actx->actx_fid, (long)actx->actx_offset,
			(long)actx->actx_bytes, actx->actx_opcode);
				
		res = nvfuse_gather_bh(&nvh->nvh_sb, actx->actx_fid, actx->actx_buf, actx->actx_bytes, actx->actx_offset, &actx->actx_bh_head, &actx->actx_bh_count);
		if (res) {
			return -1;
		}

		nvfuse_aio_gen_dev_reqs(&nvh->nvh_sb, actx);		
	}
	

	return 0;
}

s32 nvfuse_aio_wait_dev_cpls(struct nvfuse_superblock *sb)
{
	struct nvfuse_aio_ctx *actx;
	struct io_job *job;
	int cc = 0; // completion count

	nvfuse_aio_resetnextcjob(sb->io_manager);
	cc = nvfuse_aio_complete(sb->io_manager);
	sb->io_manager->queue_cur_count -= cc;

	while (cc--) 
	{
		job = nvfuse_aio_getnextcjob(sb->io_manager);

		job->complete = 1;

		actx = (struct nvfuse_aio_ctx *)job->tag;
		actx->actx_bh_count--;

		if (job->ret != job->bytes) {
			printf(" IO error \n");
			actx->actx_error = -1;
		}

		if (actx->actx_bh_count==0) 
		{
			nvfuse_aio_gen_dev_cpls(actx);
			free(actx->tag1); // free job 
			free(actx->tag2); // free iocb 
		}
	}

	return 0;
}

s32 nvfuse_aio_queue_completion(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq)
{
	struct list_head *head, *ptr, *temp;
	struct nvfuse_aio_ctx *actx;
	u32 bytes;
	s32 res;

	while (sb->io_manager->queue_cur_count) 
	{
		/* busy wating here*/
#if (NVFUSE_OS==NVFUSE_OS_LINUX)
		nvfuse_aio_wait_dev_cpls(sb);
#endif
	}
		
	head = &aioq->acq_head;

	/* copy user data to buffer cache */
	list_for_each_safe(ptr, temp, head) {
		actx = (struct nvfuse_aio_ctx *)list_entry(ptr, struct nvfuse_aio_ctx, actx_list);

		nvfuse_aio_queue_dequeue(aioq, actx, actx->actx_status);
		
		if (!actx->actx_error) 
		{
			printf(" aio was done successfully with fid = %d offset = %ld\n", actx->actx_fid, (long)actx->actx_offset);
		}
		else
		{
			printf(" aio I/O error happened with fid = %d offset = %ld\n", actx->actx_fid, (long)actx->actx_offset);
		}
				
		actx->actx_cb_func(actx);		
	}
	
	return 0;
}
