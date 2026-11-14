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

//#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>

#include "nvfuse_io_manager.h"
#if defined(__APPLE__) && defined(NVFUSE_FILE_BACKEND)
/* macOS has no RUSAGE_THREAD; standalone statistics cover this process. */
#define RUSAGE_THREAD RUSAGE_SELF
#endif
#include "nvfuse_core.h"
#include "nvfuse_aio.h"
#include "nvfuse_api.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_misc.h"
#include "nvfuse_debug.h"
#include "nvfuse_malloc.h"
#include "nvfuse_indirect.h"

#ifdef SPDK_ENABLED
#include "spdk/env.h"
#include "rte_lcore.h"
#endif

s32 nvfuse_aio_queue_init(struct io_target *target, struct nvfuse_aio_queue *aioq, s32 max_depth)
{
	memset(aioq, 0x00, sizeof(struct nvfuse_aio_queue));

	/* Submission Queue */
	INIT_LIST_HEAD(&aioq->asq_head);
	aioq->asq_cur_depth = 0;
	aioq->asq_max_depth = max_depth > NVFUSE_MAX_AIO_DEPTH ? NVFUSE_MAX_AIO_DEPTH : max_depth;

	/* Completion Queue */
	INIT_LIST_HEAD(&aioq->acq_head);
	aioq->acq_cur_depth = 0;
	aioq->acq_max_depth = max_depth > NVFUSE_MAX_AIO_DEPTH ? NVFUSE_MAX_AIO_DEPTH : max_depth;

	aioq->max_completions = NVFUSE_MAX_AIO_COMPLETION;
	aioq->total_bio_job_count = 0;

	/* FIXME: how to consider the fact that a large logical request is split into several small requests. */
	aioq->task = reactor_alloc_task(target, aioq->asq_max_depth * 2);

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

void nvfuse_aio_queue_deinit(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq)
{
	//struct nvfuse_ipc_context *ipc_ctx = &nvh->nvh_ipc_ctx;
	struct rusage rusage_end;

	aioq->aio_stat->aio_end_tsc = spdk_get_ticks();

	aioq->aio_stat->aio_execution_tsc = (aioq->aio_stat->aio_end_tsc - aioq->aio_stat->aio_start_tsc);

	getrusage(RUSAGE_THREAD, &rusage_end);

	nvfuse_rusage_diff(&aioq->aio_stat->aio_start_rusage, &rusage_end,
			   &aioq->aio_stat->aio_result_rusage);

	assert(aioq->aio_stat->aio_lat_total_count > 0);
	assert(aioq->aio_stat->aio_total_size > 0);
	//dprintf_info(AIO, " %d lat total count = %ld\n", rte_lcore_id(), aioq->stat.aio_lat_total_count);
	aioq->aio_stat->lcore_id = rte_lcore_id();

	//nvfuse_stat_ring_put(ipc_ctx->stat_ring[AIO_STAT], ipc_ctx->stat_pool[AIO_STAT], &aioq->uni_stat);
}

s32 nvfuse_aio_get_queue_depth_total(struct nvfuse_aio_queue *aioq)
{
	return aioq->asq_cur_depth + aioq->acq_cur_depth;
}

s32 nvfuse_aio_get_queue_depth_type(struct nvfuse_aio_queue *aioq, s32 qtype)
{
	switch (qtype) {
	case NVFUSE_SUBMISSION_QUEUE:
		return aioq->asq_cur_depth;
	case NVFUSE_COMPLETION_QUEUE:
		return aioq->acq_cur_depth;
	default:
		return 0;
	}
}

s32 nvfuse_aio_queue_enqueue(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq, s32 qtype)
{
	areq->queue = aioq;
	areq->status = qtype;

	switch (qtype) {
	case NVFUSE_SUBMISSION_QUEUE:
		if (aioq->asq_cur_depth == aioq->asq_max_depth) {
			//dprintf_info(AIO, " aio submission queue is full %d\n", aioq->asq_cur_depth);
			return -1;
		}
		list_add_tail(&areq->list, &aioq->asq_head);
		aioq->asq_cur_depth++;
		break;
	case NVFUSE_COMPLETION_QUEUE:
		if (aioq->acq_cur_depth == aioq->acq_max_depth) {
			//dprintf_info(AIO, " aio completion queue is full %d\n", aioq->acq_cur_depth);
			return -1;
		}
		list_add_tail(&areq->list, &aioq->acq_head);
		aioq->acq_cur_depth++;
		break;
	default:
		dprintf_error(AIO, " invalid aio qtype = %d", qtype);
		return -1;
	}

	return 0;
}

s32 nvfuse_aio_queue_dequeue(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq, s32 qtype)
{
	switch (qtype) {
	case NVFUSE_SUBMISSION_QUEUE:
		list_del(&areq->list);
		aioq->asq_cur_depth--;
		assert(aioq->asq_cur_depth >= 0);
		break;
	case NVFUSE_COMPLETION_QUEUE:
		list_del(&areq->list);
		aioq->acq_cur_depth--;
		assert(aioq->acq_cur_depth >= 0);
		break;
	default:
		dprintf_error(AIO, " invalid aio qtype = %d", qtype);
		return -1;
	}

	return 0;
}

s32 nvfuse_aio_queue_move(struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq, s32 qtype)
{
	int res;

	res = nvfuse_aio_queue_dequeue(aioq, areq, areq->status);
	if (res)
		return res;

	res = nvfuse_aio_queue_enqueue(aioq, areq, qtype);
	if (res)
		return res;

	return 0;
}

void nvfuse_aio_gen_dev_cpls(void *arg)
{
	struct nvfuse_aio_req *areq;

	/* casting */
	areq = (struct nvfuse_aio_req *)arg;

	/* move areq to comletion queue*/
	nvfuse_aio_queue_enqueue(areq->queue, areq, NVFUSE_COMPLETION_QUEUE);
}

s32 nvfuse_aio_gen_dev_reqs(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq)
{
	s64 start, length;
	s32 count = 0;
	s32 job_count = 0;
	struct io_job *jobs[AIO_MAX_QDEPTH];
	s32 res;
#ifndef NVFUSE_FILE_BACKEND
    struct nvfuse_file_table *of;
    struct nvfuse_inode_ctx *ictx;
    int write = areq->opcode == WRITE;
    u32 fast_read_physical = 0;
    int fast_read = 0;
#endif

	if ((areq->opcode != READ && areq->opcode != WRITE) ||
        !areq->bytes || areq->bytes % CLUSTER_SIZE ||
	    areq->offset < 0 || areq->offset % CLUSTER_SIZE ||
	    areq->bio_job_count <= 0 || areq->bio_job_count > AIO_MAX_QDEPTH)
		return -1;

	res = nvfuse_make_jobs(sb, jobs, areq->bio_job_count);
	if (res < 0) {
		return res;
	}

#ifndef NVFUSE_FILE_BACKEND
    /* Prepare and resolve mappings under one inode acquisition. The old
     * path prepared every request, then reacquired and remapped it below. */
    if (areq->offset >= MAX_FILE_SIZE ||
        (write ? areq->bytes >= MAX_FILE_SIZE - areq->offset :
                 areq->bytes > MAX_FILE_SIZE - areq->offset)) goto free_jobs;
    of = nvfuse_get_file_table(sb, areq->fid);
    if (!write) fast_read = nvfuse_try_cached_read_mapping(sb, of->ino,
                               areq->offset, areq->bytes, &fast_read_physical);
    ictx = fast_read ? NULL : nvfuse_read_inode(sb, NULL, of->ino);
    if (!ictx && !fast_read) goto free_jobs;
    if (!write && !fast_read && ictx->ictx_inode->i_size < areq->offset + areq->bytes) goto map_failed;
    if (write && ictx->ictx_inode->i_size < areq->offset) {
        /* Preserve dense extension, including a gap before this request. */
        u32 block = ictx->ictx_inode->i_size / CLUSTER_SIZE;
        u32 end = areq->offset / CLUSTER_SIZE;
        while (block < end) {
            u32 allocated = 0;
            res = nvfuse_get_block(sb, ictx, block, end - block, &allocated, NULL, 1);
            if (res || !allocated || allocated > end - block) goto map_failed;
            block += allocated;
        }
    }
#endif
    start = areq->offset;
    length = areq->bytes;

	//dprintf_info(AIO, " start = %ld, length = %ld \n", start, length);

	while (length) {
		s32 pblk;
		s32 lblk;
		s32 max_blocks;
		u32 num_alloc;

		lblk = (s64)start / CLUSTER_SIZE;
#ifdef	SPDK_ENABLED
		max_blocks = length / CLUSTER_SIZE;
#else
		max_blocks = 1;
#endif

#ifdef NVFUSE_FILE_BACKEND
        pblk = nvfuse_fgetblk(sb, areq->fid, lblk, max_blocks, &num_alloc);
#else
        u32 physical = 0;
        if (fast_read) {
            physical = fast_read_physical;
            num_alloc = max_blocks;
            res = 0;
        } else {
            res = nvfuse_get_block_cached(sb, ictx, lblk, max_blocks, &num_alloc, &physical);
        }
        if (!res && !num_alloc && write) {
            /* Allocation can return multiple noncontiguous physical blocks.
             * Re-resolve only newly allocated mappings to obtain a contiguous
             * extent; never submit the allocation count as an extent length. */
            res = nvfuse_get_block(sb, ictx, lblk, max_blocks, &num_alloc, NULL, 1);
            if (!res) res = nvfuse_get_block_cached(sb, ictx, lblk, max_blocks, &num_alloc, &physical);
        }
        pblk = res ? -1 : (s32)physical;
#endif
		if (pblk <= 0 || !num_alloc || num_alloc > max_blocks ||
		    job_count >= areq->bio_job_count) {
			dprintf_error(AIO, " Error: nvfuse_fgetblk() lblk = %x\n", lblk);
#ifdef NVFUSE_FILE_BACKEND
            nvfuse_release_jobs(sb, jobs, areq->bio_job_count);
            return -1;
#else
            goto map_failed;
#endif
		}

		//dprintf_info(AIO, " lblk = %d, pblk = %d, num_alloc = %d \n", lblk, pblk, num_alloc);
		jobs[job_count]->offset = (long)pblk * CLUSTER_SIZE;
		jobs[job_count]->bytes = (size_t)num_alloc * CLUSTER_SIZE;
		jobs[job_count]->ret = 0;
		jobs[job_count]->req_type = (areq->opcode == READ) ? SPDK_BDEV_IO_TYPE_READ : SPDK_BDEV_IO_TYPE_WRITE;
		jobs[job_count]->buf = areq->buf + count * CLUSTER_SIZE;

		jobs[job_count]->iov[0].iov_base = areq->buf + count * CLUSTER_SIZE;
		jobs[job_count]->iov[0].iov_len = (size_t)num_alloc * CLUSTER_SIZE;
		jobs[job_count]->iovcnt = 1;
		jobs[job_count]->cb = reactor_bio_cb;

		jobs[job_count]->complete = 0;
		jobs[job_count]->tag1 = (void *)areq;

		start += (num_alloc * CLUSTER_SIZE);
		length -= (num_alloc * CLUSTER_SIZE);
		count += num_alloc;
		job_count++;
	}

#ifndef NVFUSE_FILE_BACKEND
    if (write) {
        if (ictx->ictx_inode->i_size < areq->offset + areq->bytes)
            ictx->ictx_inode->i_size = areq->offset + areq->bytes;
        /* Overwrites change inode metadata too. Preserve the on-disk format's
         * one-second resolution and journal the timestamps with the mapping. */
        ictx->ictx_inode->i_mtime = ictx->ictx_inode->i_ctime = (u32)time(NULL);
    }
    of->rwoffset = areq->offset + areq->bytes;
    if (of->rwoffset > of->size) of->size = of->rwoffset;
    nvfuse_release_inode(sb, ictx, write ? DIRTY : NVF_CLEAN);
    /* Threshold flush stays outside the inode lock. */
    if (write) nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
#endif
    if (job_count < areq->bio_job_count) {
		nvfuse_release_jobs(sb, jobs + job_count, areq->bio_job_count - job_count);
	}

	assert(job_count <= count);
	assert(count == areq->bio_job_count);

	count = 0;
	while (count < job_count) {
		nvfuse_aio_prep(jobs[count], sb->io_manager);
		count++;
	}

	res = reactor_submit_reqs(sb->target, aioq->task, jobs, job_count);
	if (res < 0) {
		dprintf_error(AIO, " Error: aio submit error = %d\n", res);
		nvfuse_release_jobs(sb, jobs, job_count);
		return -1;
	}

#ifdef NVFUSE_PRIMARY_JOURNAL
    if (sb->target->journal) sb->target->journal_aio++;
#endif
	aioq->total_bio_job_count += job_count;
	areq->bio_job_count = job_count;

    return 0;
#ifndef NVFUSE_FILE_BACKEND
map_failed:
    nvfuse_release_inode(sb, ictx, write ? DIRTY : NVF_CLEAN);
    if (write) nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
free_jobs:
    nvfuse_release_jobs(sb, jobs, areq->bio_job_count);
    return -1;
#endif
}

s32 nvfuse_aio_queue_submission(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq, struct nvfuse_aio_req *areq)
{
	struct list_head *head, *ptr, *temp;
	u32 bytes;
	s32 res;

	/* copy user data to buffer cache */
#ifdef SPDK_ENABLED
	areq->submit_tsc = spdk_get_ticks();
#endif

	//dprintf_info(AIO, " aio ready queue : fd = %d offset = %ld, bytes = %ld, op = %d\n", areq->fid, (long)areq->offset,
	//	(long)areq->bytes, areq->opcode);
	assert(nvfuse_is_directio(&nvh->nvh_sb, areq->fid));
#ifdef NVFUSE_FILE_BACKEND
	if (areq->opcode == READ) {
		bytes = nvfuse_readfile_aio_directio(nvh, areq->fid, areq->buf, areq->bytes,
							 areq->offset);
	} else {
		bytes = nvfuse_writefile_directio_prepare(nvh, areq->fid, areq->buf, areq->bytes,
				areq->offset);
	}

#else
    /* Primary AIO performs bounds checking, preparation and mapping once in
     * nvfuse_aio_gen_dev_reqs, rather than traversing metadata twice. */
    bytes = areq->bytes;
#endif

	if (bytes == areq->bytes) {
		/* move to submission queue */
		nvfuse_aio_queue_enqueue(aioq, areq, NVFUSE_SUBMISSION_QUEUE);
	} else {
		dprintf_error(AIO, " submission error\n ");
		return -1;
	}

	/* collect buffer */
	head = &aioq->asq_head;
	list_for_each_safe(ptr, temp, head) {
		areq = (struct nvfuse_aio_req *)list_entry(ptr, struct nvfuse_aio_req, list);

		INIT_LIST_HEAD(&areq->bh_head);
		//dprintf_info(AIO, " aio submission queue : fd = %d offset = %ld, bytes = %ld, op = %d\n", areq->fid, (long)areq->offset,
		//	(long)areq->bytes, areq->opcode);

		assert(nvfuse_is_directio(&nvh->nvh_sb, areq->fid));
		/* direct i/o without buffer head */
		areq->bio_job_count = NVFUSE_SIZE_TO_BLK(areq->bytes);
		res = nvfuse_aio_gen_dev_reqs(&nvh->nvh_sb, aioq, areq);
		if (res < 0) {
			nvfuse_aio_queue_dequeue(aioq, areq, NVFUSE_SUBMISSION_QUEUE);
			return -1;
		}

		nvfuse_aio_queue_dequeue(aioq, areq, NVFUSE_SUBMISSION_QUEUE);
	}

	return 0;
}

s32 nvfuse_aio_wait_dev_cpls(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq, s32 min_nr, s32 nr)
{
	struct nvfuse_aio_req *areq;
	struct io_job *job;
	struct io_job *jobs[AIO_MAX_QDEPTH];
	int cc = 0; // completion count
	int i;

	cc = reactor_cq_get_reqs(aioq->task, jobs, min_nr, nr);

	aioq->total_bio_job_count -= cc;

	for (i = 0; i < cc; i++) {
		job = jobs[i];

		job->complete = 1;

		areq = (struct nvfuse_aio_req *)job->tag1;
		areq->bio_job_count--;

		if (job->ret != 0) {
			dprintf_error(AIO, " IO error \n");
			areq->error = -1;
		}

		if (areq->bio_job_count == 0) {
#ifdef NVFUSE_PRIMARY_JOURNAL
            if (sb->target->journal) {
                assert(sb->target->journal_aio);
                sb->target->journal_aio--;
            }
#endif
			assert(nvfuse_is_directio(sb, areq->fid));
			nvfuse_aio_gen_dev_cpls(areq);
		}

	}

	nvfuse_release_jobs(sb, jobs, cc);

	return cc;
}

s32 nvfuse_aio_queue_completion(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq)
{
	struct list_head *head, *ptr, *temp;
	struct nvfuse_aio_req *areq;

	while (aioq->acq_cur_depth < aioq->max_completions) {
		/* busy wating here*/
#if (NVFUSE_OS==NVFUSE_OS_LINUX)
		nvfuse_aio_wait_dev_cpls(sb, aioq, aioq->total_bio_job_count, aioq->total_bio_job_count);
#endif
	}

	//dprintf_info(AIO, " completion queue depth = %d", aioq->acq_cur_depth);

	head = &aioq->acq_head;
	/* copy user data to buffer cache */
	list_for_each_safe(ptr, temp, head) {
		areq = (struct nvfuse_aio_req *)list_entry(ptr, struct nvfuse_aio_req, list);

		assert(areq->bio_job_count == 0);
		nvfuse_aio_queue_dequeue(aioq, areq, areq->status);

		if (areq->error) {
			dprintf_error(AIO, " aio I/O error happened with fid = %d offset = %ld\n", areq->fid,
			       (long)areq->offset);

		} else {
			//dprintf_info(AIO, " aio was done successfully with fid = %d offset = %ld\n", areq->fid, (long)areq->offset);
#ifdef SPDK_ENABLED
			areq->complete_tsc = spdk_get_ticks();
#endif
		}

		areq->actx_cb_func(areq);
	}

	return 0;
}

s32 nvfuse_io_submit(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq, s32 nr, struct nvfuse_aio_req **list)
{
	s32 idx;
	s32 ret = 0;

	for (idx = 0; idx < nr; idx++) {
		ret = nvfuse_aio_queue_submission(nvh, aioq, list[idx]);
		if (ret) {
			dprintf_error(AIO, " Error: queue submission\n");
		}
	}

	return ret;
}

s32 nvfuse_io_getevents(struct nvfuse_superblock *sb, struct nvfuse_aio_queue *aioq, s32 min_nr, s32 nr, struct nvfuse_aio_req **list)
{
	struct list_head *head, *ptr, *temp;
	struct nvfuse_aio_req *areq;
	s32 idx = 0;

#if (NVFUSE_OS==NVFUSE_OS_LINUX)
	nvfuse_aio_wait_dev_cpls(sb, aioq, min_nr, nr);
#endif

	//dprintf_info(AIO, " completion queue depth = %d\n", aioq->acq_cur_depth);

	head = &aioq->acq_head;
	/* copy user data to buffer cache */
	list_for_each_safe(ptr, temp, head) {
		areq = (struct nvfuse_aio_req *)list_entry(ptr, struct nvfuse_aio_req, list);

		assert(areq->bio_job_count == 0);
		nvfuse_aio_queue_dequeue(aioq, areq, areq->status);

		if (areq->error) {
			dprintf_error(AIO, " aio I/O error happened with fid = %d offset = %ld\n", areq->fid,
			       (long)areq->offset);

		} else {
			//dprintf_info(AIO, " aio was done successfully with fid = %d offset = %ld\n", areq->fid, (long)areq->offset);
#ifdef SPDK_ENABLED
			areq->complete_tsc = spdk_get_ticks();
#endif
		}

		list[idx++] = areq;
		if (idx == nr)
			break;
	}

	return idx;
}

