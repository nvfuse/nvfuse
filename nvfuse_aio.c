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

#include "nvfuse_io_manager.h"
#include "nvfuse_core.h"
#include "nvfuse_aio.h"
#include "nvfuse_api.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_misc.h"

#ifdef SPDK_ENABLED
#include "spdk/env.h"
#include "rte_lcore.h"
#endif

s32 nvfuse_aio_queue_init(struct nvfuse_aio_queue * aioq, s32 max_depth)
{
	memset(aioq, 0x00, sizeof(struct nvfuse_aio_queue));

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

	aioq->max_completions = NVFUSE_MAX_AIO_COMPLETION;
	aioq->aio_cur_depth = 0;

	aioq->aio_stat = (struct perf_stat_aio *)&aioq->uni_stat;
	aioq->aio_stat->aio_start_tsc = spdk_get_ticks();
	aioq->aio_stat->aio_end_tsc = 0;

	aioq->aio_stat->aio_lat_total_tsc = 0;
	aioq->aio_stat->aio_lat_total_count = 0;
	aioq->aio_stat->aio_lat_min_tsc = ~0;
	aioq->aio_stat->aio_lat_max_tsc = 0;
	aioq->aio_stat->aio_total_size = 0;

	aioq->aio_cc_sum = 0;
	aioq->aio_cc_cnt = 0;
	aioq->aio_stat->stat_type = AIO_STAT;


	getrusage(RUSAGE_THREAD, &aioq->aio_stat->aio_start_rusage);

	return 0;
}

void nvfuse_aio_queue_deinit(struct nvfuse_handle *nvh, struct nvfuse_aio_queue * aioq)
{	
	#ifdef SPDK_ENABLED
	double io_per_second, mb_per_second;
	double average_latency, min_latency, max_latency;
	u64 tsc_rate = spdk_get_ticks_hz();
	struct nvfuse_ipc_context *ipc_ctx = &nvh->nvh_ipc_ctx;
	struct rusage rusage_end;

	aioq->aio_stat->aio_end_tsc = spdk_get_ticks();

	aioq->aio_stat->aio_execution_tsc = (aioq->aio_stat->aio_end_tsc - aioq->aio_stat->aio_start_tsc);

	getrusage(RUSAGE_THREAD, &rusage_end);
	
	#if 0
	timeval_subtract(&aioq->aio_stat->aio_sys_time, 
					&aioq->aio_stat->aio_end_rusage.ru_stime, 
					&aioq->aio_stat->aio_start_rusage.ru_stime);

	timeval_subtract(&aioq->aio_stat->aio_usr_time, 
					&aioq->aio_stat->aio_end_rusage.ru_utime, 
					&aioq->aio_stat->aio_start_rusage.ru_utime);
	#endif

	nvfuse_rusage_diff(&aioq->aio_stat->aio_start_rusage, &rusage_end, 
						&aioq->aio_stat->aio_result_rusage);
	
	assert(aioq->aio_stat->aio_lat_total_count > 0);
	assert(aioq->aio_stat->aio_total_size > 0);
	//printf(" %d lat total count = %ld\n", rte_lcore_id(), aioq->stat.aio_lat_total_count);
	aioq->aio_stat->lcore_id = rte_lcore_id();
	
#if 0
	io_per_second = (double)aioq->aio_lat_total_count / ((aioq->aio_end_tsc - aioq->aio_start_tsc) / tsc_rate);
	mb_per_second = (double)aioq->aio_total_size / (1024 * 1024) / ((aioq->aio_end_tsc - aioq->aio_start_tsc) / tsc_rate);
	average_latency = (double)(aioq->aio_lat_total_tsc / aioq->aio_lat_total_count) * 1000 * 1000 / tsc_rate;
	min_latency = (double)aioq->aio_lat_min_tsc * 1000 * 1000 / tsc_rate;
	max_latency = (double)aioq->aio_lat_max_tsc * 1000 * 1000 / tsc_rate;

	printf("\n NVFUSE AIO Queue Perf Statistics. \n");
	printf("------------------------------------\n");
	printf(" iops = %.0f IOPS (%.3f KIOPS)\n", io_per_second, io_per_second / 1024);
	printf(" bandwidth = %.3f MB/s\n", mb_per_second);
	printf(" avg latency = %.3f us \n", average_latency);
	printf(" min latency = %.3f us \n", min_latency);
	printf(" max latency = %.3f us \n", max_latency);
	printf("------------------------------------\n");
#else
	nvfuse_stat_ring_put(ipc_ctx->stat_ring[AIO_STAT], ipc_ctx->stat_pool[AIO_STAT], &aioq->uni_stat);
#endif
	printf("\n");

	#endif
}

s32 nvfuse_aio_get_queue_depth_total(struct nvfuse_aio_queue *aioq)
{
	return aioq->arq_cur_depth + aioq->asq_cur_depth + aioq->acq_cur_depth;
}

s32 nvfuse_aio_get_queue_depth_type(struct nvfuse_aio_queue *aioq, s32 qtype)
{
	switch (qtype) {
		case NVFUSE_READY_QUEUE:
			return aioq->arq_cur_depth;			
		case NVFUSE_SUBMISSION_QUEUE:		
			return aioq->asq_cur_depth;			
		case NVFUSE_COMPLETION_QUEUE:			
			return aioq->acq_cur_depth;
		default:
			return 0;
	}	
}

s32 nvfuse_aio_queue_enqueue(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_ctx * actx, s32 qtype)
{
	actx->actx_queue = aioq;
	actx->actx_status = qtype;

	switch (qtype) {
		case NVFUSE_READY_QUEUE:
			if (aioq->arq_cur_depth == aioq->arq_max_depth)
			{
				//printf(" aio ready queue is full %d\n", aioq->arq_cur_depth);
				return -1;
			}
			list_add_tail(&actx->actx_list, &aioq->arq_head);
			aioq->arq_cur_depth++;
			break;
		case NVFUSE_SUBMISSION_QUEUE:
			if (aioq->asq_cur_depth == aioq->asq_max_depth)
			{
				//printf(" aio submission queue is full %d\n", aioq->asq_cur_depth);
				return -1;
			}
			list_add_tail(&actx->actx_list, &aioq->asq_head);
			aioq->asq_cur_depth++;
			break;
		case NVFUSE_COMPLETION_QUEUE:
			if (aioq->acq_cur_depth == aioq->acq_max_depth)
			{
				//printf(" aio completion queue is full %d\n", aioq->acq_cur_depth);
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

void nvfuse_aio_gen_dev_cpls_buffered(void *arg)
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
	nvfuse_aio_queue_enqueue(actx->actx_queue, actx, NVFUSE_COMPLETION_QUEUE);
}

void nvfuse_aio_gen_dev_cpls_directio(void *arg)
{
	struct nvfuse_aio_ctx *actx;

	/* casting */
	actx = (struct nvfuse_aio_ctx *)arg;
		
	/* move actx to comletion queue*/
	nvfuse_aio_queue_enqueue(actx->actx_queue, actx, NVFUSE_COMPLETION_QUEUE);
}

s32 nvfuse_aio_gen_dev_reqs_buffered(struct nvfuse_superblock *sb, struct nvfuse_aio_ctx *actx)
{
	struct list_head *head, *ptr, *temp;
	struct nvfuse_buffer_head *bh;
	struct nvfuse_buffer_cache *bc;

#if (NVFUSE_OS == NVFUSE_OS_LINUX)
	struct io_job *jobs[AIO_MAX_QDEPTH];
	struct iocb *iocb[AIO_MAX_QDEPTH];
	s32 count = 0;	
	s32 res;

	res = nvfuse_make_jobs(sb, jobs, actx->actx_bh_count);
	if (res < 0) {
		return res;
	}	
#endif

	head = &actx->actx_bh_head;
	list_for_each_safe(ptr, temp, head) {
		bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_aio_list);
		bc = bh->bh_bc;

#if (NVFUSE_OS == NVFUSE_OS_LINUX)
		jobs[count]->offset = (long)bc->bc_pno * CLUSTER_SIZE;
		jobs[count]->bytes = (size_t)CLUSTER_SIZE;
		jobs[count]->ret = 0;
		jobs[count]->req_type = (actx->actx_opcode == READ) ? READ : WRITE;
		jobs[count]->buf = bc->bc_buf;
		jobs[count]->complete = 0;
		jobs[count]->tag1 = (void *)actx;

		iocb[count] = &jobs[count]->iocb;
		count++;
#else
		if (actx->actx_opcode == READ)
			nvfuse_read_cluster(bc->bc_buf, bc->bc_pno, sb->io_manager);
		else
			nvfuse_write_cluster(bc->bc_buf, bc->bc_pno, sb->io_manager);
#endif
	}
	
	if (count < actx->actx_bh_count)
	{
		nvfuse_release_jobs(sb, jobs + count, actx->actx_bh_count - count);
	}
	
#if (NVFUSE_OS == NVFUSE_OS_LINUX)
	count = 0;
	while(count < actx->actx_bh_count)
	{
		nvfuse_aio_prep(jobs[count], sb->io_manager);
		count ++;
	}

	nvfuse_aio_submit(iocb, actx->actx_bh_count, sb->io_manager);
	sb->io_manager->queue_cur_count += actx->actx_bh_count;

	//actx->tag1 = (void *)jobs;
	//actx->tag2 = (void *)iocb;

#elif (NVFUSE_OS == NVFUSE_OS_WINDOWS)
	nvfuse_aio_gen_dev_cpls_buffered((void *)actx);
#endif

	return 0;
}

s32 nvfuse_aio_gen_dev_reqs_directio(struct nvfuse_superblock *sb, struct nvfuse_aio_ctx *actx)
{
	s64 start, length;
	s32 count = 0;
	s32 req_count = 0;
#if (NVFUSE_OS == NVFUSE_OS_LINUX)
	struct io_job *jobs[AIO_MAX_QDEPTH];
	struct iocb *iocb[AIO_MAX_QDEPTH];
	s32 res;

	assert(actx->actx_bh_count);

	res = nvfuse_make_jobs(sb, jobs, actx->actx_bh_count);
	if (res < 0) {
		return res;
	}	
#endif
	
	start = actx->actx_offset;
	length = actx->actx_bytes;

	//printf(" start = %ld, length = %ld \n", start, length);

	while (length)
	{
		s32 pblk;
		s32 lblk;
		s32 max_blocks;
		s32 num_alloc;
		
		lblk = (s64)start / CLUSTER_SIZE;
#ifdef	SPDK_ENABLED
		max_blocks = length / CLUSTER_SIZE;
#else
		max_blocks = 1;
#endif

		pblk = nvfuse_fgetblk(sb, actx->actx_fid, lblk, max_blocks, &num_alloc);
		if (pblk < 0)
		{
			printf(" Error: nvfuse_fgetblk() lblk = %x\n", lblk);
			return -1;
		}
		
		//printf(" lblk = %d, pblk = %d, num_alloc = %d \n", lblk, pblk, num_alloc);
#if (NVFUSE_OS == NVFUSE_OS_LINUX)
		jobs[req_count]->offset = (long)pblk * CLUSTER_SIZE;
		jobs[req_count]->bytes = (size_t)num_alloc * CLUSTER_SIZE;
		jobs[req_count]->ret = 0;
		jobs[req_count]->req_type = (actx->actx_opcode == READ) ? READ : WRITE;
		jobs[req_count]->buf = actx->actx_buf + count * CLUSTER_SIZE;
		jobs[req_count]->complete = 0;
		jobs[req_count]->tag1 = (void *)actx;

		iocb[req_count] = &jobs[req_count]->iocb;
		assert(iocb[req_count] != NULL);
#else
		if (actx->actx_opcode == READ)
			nvfuse_read_ncluster((s8 *)actx->actx_buf + count * CLUSTER_SIZE, pblk, num_alloc, sb->io_manager);
		else
			nvfuse_write_ncluster((s8 *)actx->actx_buf + count * CLUSTER_SIZE, pblk, num_alloc, sb->io_manager);
#endif
		start += (num_alloc * CLUSTER_SIZE);
		length -= (num_alloc * CLUSTER_SIZE);
		count += num_alloc;
		req_count++;
	}
	
	if (req_count < actx->actx_bh_count)
	{
		nvfuse_release_jobs(sb, jobs + req_count, actx->actx_bh_count - req_count);
	}

	assert(req_count <= count);
	assert(count == actx->actx_bh_count);

#if (NVFUSE_OS == NVFUSE_OS_LINUX)
	count = 0;
	while (count < req_count)
	{
		nvfuse_aio_prep(jobs[count], sb->io_manager);
		count++;
	}

	res = nvfuse_aio_submit(iocb, req_count, sb->io_manager);
	if (res < 0)
	{
		printf(" Error: aio submit error = %d\n", res);
		return -1;
	}

	sb->io_manager->queue_cur_count += req_count;
	actx->actx_bh_count = req_count;

	//printf(" cur queue depth = %d \n", sb->io_manager->queue_cur_count);

	//actx->tag1 = (void *)jobs;
	//actx->tag2 = (void *)iocb;

#elif (NVFUSE_OS == NVFUSE_OS_WINDOWS)
	nvfuse_aio_gen_dev_cpls_directio((void *)actx);
#endif

	return 0;
}

s32 nvfuse_aio_queue_submission(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq)
{
	struct list_head *head, *ptr, *temp;
	struct nvfuse_aio_ctx *actx;	
	u32 bytes;
	s32 res;

	/* Ready Queue */
	head = &aioq->arq_head;

	/* copy user data to buffer cache */
	list_for_each_safe(ptr, temp, head) {
		
		actx = (struct nvfuse_aio_ctx *)list_entry(ptr, struct nvfuse_aio_ctx, actx_list);
		
		#ifdef SPDK_ENABLED
		actx->actx_submit_tsc = spdk_get_ticks();			
		#endif

		//printf(" aio ready queue : fd = %d offset = %ld, bytes = %ld, op = %d\n", actx->actx_fid, (long)actx->actx_offset,
		//	(long)actx->actx_bytes, actx->actx_opcode);
		if (nvfuse_is_directio(&nvh->nvh_sb, actx->actx_fid))
		{
			if (actx->actx_opcode == READ)
			{
				bytes = nvfuse_readfile_aio_directio(nvh, actx->actx_fid, actx->actx_buf, actx->actx_bytes, actx->actx_offset);
			}
			else
			{
				bytes = nvfuse_writefile_directio_prepare(nvh, actx->actx_fid, actx->actx_buf, actx->actx_bytes, actx->actx_offset);
			}
		}
		else
		{
			if (actx->actx_opcode == READ)
				bytes = nvfuse_readfile_aio(nvh, actx->actx_fid, actx->actx_buf, actx->actx_bytes, actx->actx_offset);
			else
				bytes = nvfuse_writefile_buffered_aio(nvh, actx->actx_fid, actx->actx_buf, actx->actx_bytes, actx->actx_offset);
		}

		if (bytes == actx->actx_bytes)
		{
			/* move to submission queue */
			nvfuse_aio_queue_move(aioq, actx, NVFUSE_SUBMISSION_QUEUE);
		}
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
		//printf(" aio submission queue : fd = %d offset = %ld, bytes = %ld, op = %d\n", actx->actx_fid, (long)actx->actx_offset,
		//	(long)actx->actx_bytes, actx->actx_opcode);

		/* buffered AIO requests with buffer head */
		if (!nvfuse_is_directio(&nvh->nvh_sb, actx->actx_fid))
		{
			res = nvfuse_gather_bh(&nvh->nvh_sb, actx->actx_fid, actx->actx_buf, actx->actx_bytes, actx->actx_offset, &actx->actx_bh_head, &actx->actx_bh_count);
			if (res) {
				return -1;
			}
			nvfuse_aio_gen_dev_reqs_buffered(&nvh->nvh_sb, actx);
		}
		else /* in case of direct I/O */
		{/* direct i/o without buffer head */
			actx->actx_bh_count = NVFUSE_SIZE_TO_BLK(actx->actx_bytes);
			res = nvfuse_aio_gen_dev_reqs_directio(&nvh->nvh_sb, actx);
			if (res < 0)
			    return -1;
		}
				
		nvfuse_aio_queue_dequeue(aioq, actx, NVFUSE_SUBMISSION_QUEUE);
	}
	
	return 0;
}

s32 nvfuse_aio_wait_dev_cpls(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq)
{
	struct nvfuse_aio_ctx *actx;
	struct io_job *job;
	struct io_job *jobs[AIO_MAX_QDEPTH];
	int num_jobs = 0;
	int cc = 0; // completion count
	
	cc = nvfuse_aio_complete(sb->io_manager);
	sb->io_manager->queue_cur_count -= cc;
	
	aioq->aio_cc_sum += cc;
	aioq->aio_cc_cnt ++;

	while (cc--) 
	{
		job = nvfuse_aio_getnextcjob(sb->io_manager);
		jobs[num_jobs++] = job;

		job->complete = 1;

		actx = (struct nvfuse_aio_ctx *)job->tag1;
		actx->actx_bh_count--;

		if (job->ret != job->bytes) {
			printf(" IO error \n");
			actx->actx_error = -1;
		}

		if (actx->actx_bh_count == 0)
		{
			if (!nvfuse_is_directio(sb, actx->actx_fid))
			{
				nvfuse_aio_gen_dev_cpls_buffered(actx);
			}
			else
			{
				nvfuse_aio_gen_dev_cpls_directio(actx);
			}
		}
	}

	nvfuse_release_jobs(sb, jobs, num_jobs);

	return 0;
}

s32 nvfuse_aio_queue_completion(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq)
{
	struct list_head *head, *ptr, *temp;
	struct nvfuse_aio_ctx *actx;

	while (aioq->acq_cur_depth < aioq->max_completions)
	{
		/* busy wating here*/
#if (NVFUSE_OS==NVFUSE_OS_LINUX)
		nvfuse_aio_wait_dev_cpls(sb, aioq);
#endif
	}
	
	//printf(" completion queue depth = %d", aioq->acq_cur_depth);

	head = &aioq->acq_head;
	/* copy user data to buffer cache */
	list_for_each_safe(ptr, temp, head) {
		actx = (struct nvfuse_aio_ctx *)list_entry(ptr, struct nvfuse_aio_ctx, actx_list);

		assert(actx->actx_bh_count == 0);
		nvfuse_aio_queue_dequeue(aioq, actx, actx->actx_status);
		
		if (actx->actx_error) 
		{
			printf(" aio I/O error happened with fid = %d offset = %ld\n", actx->actx_fid, (long)actx->actx_offset);
			
		}
		else
		{
			//printf(" aio was done successfully with fid = %d offset = %ld\n", actx->actx_fid, (long)actx->actx_offset);
			#ifdef SPDK_ENABLED
			actx->actx_complete_tsc = spdk_get_ticks();
			#endif
		}
		aioq->aio_cur_depth--;
		actx->actx_cb_func(actx);
	}
	
	return 0;
}
