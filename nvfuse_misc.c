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
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>
#include <assert.h>

#include "nvfuse_core.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_config.h"
#include "nvfuse_api.h"
#include "nvfuse_aio.h"
#include "nvfuse_malloc.h"
#include "nvfuse_gettimeofday.h"
#include "time.h"
#ifdef SPDK_ENABLED
#include "spdk/env.h"
#endif

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
#include <windows.h>
#endif

s64 nvfuse_rand();

//extern struct nvfuse_handle *g_nvh;

s32 nvfuse_mkfile(struct nvfuse_handle *nvh, s8 *str, s8 *ssize)
{
#define BLOCK_IO_SIZE (CLUSTER_SIZE * 8)
	u64 size;
	s32 i = CLUSTER_SIZE, fd, write_block_size = BLOCK_IO_SIZE;
	s32 ret = 0;
	s32 num_block;
	s8 file_source[BLOCK_IO_SIZE];

	if(strlen(str) < 1 || strlen(str) >= FNAME_SIZE)
		return error_msg("mkfile  [filename] [size]\n");

	if(atoi(ssize) ==0)
		return error_msg("mkfile  [filename] [size]\n");

	fd = nvfuse_openfile_path(nvh, str, O_RDWR|O_CREAT, 0);

	if (fd!=-1)
	{
#if (NVFUSE_OS == NVFUSE_OS_WINDOWS)
		size = _atoi64(ssize);
#else
		size = atoi(ssize);
#endif

		/*ret = nvfuse_fallocate(nvh, str, 0, size);*/

		if(size  < 1) size = CLUSTER_SIZE;

		for (size; size>=write_block_size; size-=write_block_size){
			ret = nvfuse_writefile(nvh, fd, file_source, write_block_size, 0);

			if(ret == -1)
				return -1;
		}
		nvfuse_writefile(nvh, fd,file_source, i, 0); /* write remainder */

		nvfuse_fsync(nvh, fd);
		nvfuse_closefile(nvh, fd);
	}
	
	if (fd > 0)
		return NVFUSE_SUCCESS;
	
	return NVFUSE_ERROR;
}

void nvfuse_aio_test_callback(void *arg)
{
	struct nvfuse_aio_ctx *actx = (struct nvfuse_aio_ctx *)arg;	
	struct nvfuse_aio_queue *aioq = actx->actx_queue;

#ifdef SPDK_ENABLED
	u64 latency_tsc;
	latency_tsc = actx->actx_complete_tsc - actx->actx_submit_tsc;
	aioq->aio_stat->aio_lat_total_tsc += latency_tsc;
	aioq->aio_stat->aio_lat_total_count++;
	aioq->aio_stat->aio_lat_min_tsc = MIN(latency_tsc, aioq->aio_stat->aio_lat_min_tsc);
	aioq->aio_stat->aio_lat_max_tsc = MAX(latency_tsc, aioq->aio_stat->aio_lat_max_tsc);
	aioq->aio_stat->aio_total_size += actx->actx_bytes;
#endif

	free(actx);
}

struct user_context{
		s64 file_size;
		s32 io_size;
		s32 qdepth;
		s32 is_read;		
		s32 is_rand;

		s32 fd;		
		s64 io_remaining;
		s64 io_curr;
		s8 *user_buf;
		s32 buf_ptr;
};

s32 nvfuse_aio_alloc_req(struct nvfuse_handle *nvh, struct nvfuse_aio_queue *aioq, void *_user_ctx)
{
	struct nvfuse_aio_ctx *actx;
	struct user_context *user_ctx;
	s32 ret;

	user_ctx = (struct user_context *)_user_ctx;

	/* initialization of aio context */
	actx = nvfuse_malloc(sizeof(struct nvfuse_aio_ctx));
	memset(actx, 0x00, sizeof(struct nvfuse_aio_ctx));
	actx->actx_fid = user_ctx->fd;
	actx->actx_opcode = user_ctx->is_read ? READ : WRITE;
	actx->actx_buf = user_ctx->user_buf + user_ctx->io_size * user_ctx->buf_ptr;
	user_ctx->buf_ptr = (user_ctx->buf_ptr + 1) % user_ctx->qdepth;
	if (!user_ctx->is_rand)
	{	
		actx->actx_offset = user_ctx->io_curr;
	}
	else
	{
		s64 blkno = (u64)nvfuse_rand() % (user_ctx->file_size / user_ctx->io_size);
		actx->actx_offset = blkno * user_ctx->io_size;
	}

	assert(actx->actx_offset + user_ctx->io_size  <= user_ctx->file_size);

	//printf(" aio offset = %ld\n", actx->actx_offset);

	actx->actx_bytes = user_ctx->io_size;
	actx->actx_error = 0;
	INIT_LIST_HEAD(&actx->actx_list);
	actx->actx_cb_func = nvfuse_aio_test_callback;
	actx->actx_sb = &nvh->nvh_sb;

	memset(actx->actx_buf, 0xaa, user_ctx->io_size);

	/* enqueue actx to aio queue */
	ret = nvfuse_aio_queue_enqueue(aioq, actx, NVFUSE_READY_QUEUE);
	if (ret)
	{
		printf(" Error: Enqueue error = arq depth = %d\n", aioq->arq_cur_depth);
		return -1;
	}

	user_ctx->io_curr += user_ctx->io_size;
	user_ctx->io_remaining -= user_ctx->io_size;

	return 0;
}

s32 nvfuse_aio_test_rw(struct nvfuse_handle *nvh, s8 *str, s64 file_size, u32 io_size,
						u32 qdepth, u32 is_read, u32 is_direct, u32 is_rand, s32 runtime)
{
	struct nvfuse_aio_queue aioq;
	s32 ret;
	s32 i;	
	s32 last_progress = 0;
	s32 curr_progress = 0;
	s32 flags;
	s64 file_allocated_size;
	struct stat stat_buf;
	struct timeval tv;
	struct user_context user_ctx;
	
	user_ctx.file_size = file_size;
	user_ctx.io_size = io_size;
	user_ctx.qdepth = qdepth;
	user_ctx.is_read = is_read;
	user_ctx.is_rand = is_rand;
		
	printf(" aiotest %s filesize = %0.3fMB io_size = %d qdpeth = %d (%c) direct (%d)\n", str, (double)file_size/(1024*1024), io_size, qdepth, is_read ? 'R' : 'W', is_direct);

	flags = O_RDWR | O_CREAT;
	if (is_direct)
		flags |= O_DIRECT;

	user_ctx.fd = nvfuse_openfile_path(nvh, str, flags, 0);
	if (user_ctx.fd < 0)
	{
		printf(" Error: file open or create \n");
		return -1;
	}

	printf(" start fallocate %s size %lu \n", str, (long)file_size);
	/* pre-allocation of data blocks*/
	nvfuse_fallocate(nvh, str, 0, file_size);
	printf(" finish fallocate %s size %lu \n", str, (long)file_size);

	ret = nvfuse_getattr(nvh, str, &stat_buf);
	if (ret)
	{
		printf(" No such file %s\n", str);
		return -1;
	}
	/* NOTE: Allocated size may differ from requested size. */
	file_allocated_size = stat_buf.st_size;

	printf(" requested size %ldMB.\n", (long)file_size / NVFUSE_MEGA_BYTES);
	printf(" allocated size %ldMB.\n", (long)file_allocated_size / NVFUSE_MEGA_BYTES);

#if (NVFUSE_OS == NVFUSE_OS_LINUX)
	file_size = file_allocated_size;
#endif
	/* initialization of aio queue */
	ret = nvfuse_aio_queue_init(&aioq, qdepth);
	if (ret)
	{
		printf(" Error: aio queue init () with ret = %d\n ", ret);
		return -1;
	}
	
	user_ctx.io_curr = 0;
	user_ctx.io_remaining = file_size; 

	/* user data buffer allocation */
	user_ctx.user_buf = nvfuse_alloc_aligned_buffer(io_size * qdepth);
	if (user_ctx.user_buf == NULL)
	{
	    printf(" Error: malloc()\n");
	    return -1;
	}
	user_ctx.buf_ptr = 0;

	gettimeofday(&tv, NULL);
	while (user_ctx.io_remaining > 0 || aioq.aio_cur_depth)
	{		
		//printf(" total depth = %d arq depth = %d\n", aioq.aio_cur_depth, aioq.arq_cur_depth);
		while (aioq.aio_cur_depth < qdepth && user_ctx.io_remaining > 0)
		{
			ret = nvfuse_aio_alloc_req(nvh, &aioq, &user_ctx);
			if (ret)		
				break;

			aioq.aio_cur_depth++;
		}
		
		/* progress bar */
		curr_progress = (user_ctx.io_curr * 100 / file_size);
		if (curr_progress != last_progress)
		{
			printf(".");
			if (curr_progress % 10 == 0)
			{
			    printf("%d%% %.3fMB avg req cpls per poll  = %.2f\n", curr_progress, 
				(double)user_ctx.io_curr / NVFUSE_MEGA_BYTES / time_since_now(&tv), 
				(double)aioq.aio_cc_sum/aioq.aio_cc_cnt);
			}
			fflush(stdout);
			last_progress = curr_progress;
		}
		
		#if 1
		/* aio submission */
		ret = nvfuse_aio_queue_submission(nvh, &aioq);
		if (ret)
		{
			printf(" Error: queue submission \n");
			goto CLOSE_FD;
		}
		#endif

RETRY_WAIT_COMPLETION:
		//printf(" Submission depth = %d\n", aioq.aio_cur_depth);
		/* aio completion */		
		ret = nvfuse_aio_queue_completion(&nvh->nvh_sb, &aioq);
		if (ret)
		{
			printf(" Error: queue completion \n");
			goto CLOSE_FD;
		}
		//printf(" completion depth = %d\n", aioq.aio_cur_depth);
		if (runtime && time_since_now(&tv) >= (double)runtime)
		{
		    if (aioq.aio_cur_depth)
		    {
			goto RETRY_WAIT_COMPLETION;
		    }
		    break;
		}
	}

	assert(aioq.aio_cur_depth==0);

CLOSE_FD:
	nvfuse_aio_queue_deinit(nvh, &aioq);
	nvfuse_free_aligned_buffer(user_ctx.user_buf);
	nvfuse_fsync(nvh, user_ctx.fd);
	nvfuse_closefile(nvh, user_ctx.fd);
	
	return 0;
}

s32 nvfuse_aio_test(struct nvfuse_handle *nvh, s32 direct)
{
	char str[128];
	struct timeval tv;
	s64 file_size;
	s32 i;
	s32 res;
	
	for (i = 0; i < 1; i++)
	{
		sprintf(str, "file%d", i);
		file_size = (s64)8 * 1024 * 1024 * 1024;
		gettimeofday(&tv, NULL);
		res = nvfuse_aio_test_rw(nvh, str, file_size, 4096, AIO_MAX_QDEPTH, WRITE, direct, 0 /* sequential */, 0);
		if (res < 0)
		{
			printf(" Error: aio write test \n");
			break;
		}

		printf(" nvfuse aio through %.3fMB/s\n", (double)file_size/(1024*1024)/time_since_now(&tv));
		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0)
		{
			printf(" Error: rmfile = %s\n", str);
			break;
		}
	}

	for (i = 0; i < 1; i++)
	{
		sprintf(str, "file%d", i);
		file_size = (s64)8 * 1024 * 1024 * 1024;
		gettimeofday(&tv, NULL);
		res = nvfuse_aio_test_rw(nvh, str, file_size, 4096, AIO_MAX_QDEPTH, READ, direct, 0 /* sequential */, 0);
		if (res < 0)
		{
			printf(" Error: aio write test \n");
			break;
		}
		printf(" nvfuse aio through %.3fMB/s\n", (double)file_size/(1024*1024)/time_since_now(&tv));
		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0)
		{
			printf(" Error: rmfile = %s\n", str);
			break;
		}
	}

	exit(0);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_fallocate_test(struct nvfuse_handle *nvh)
{
	char str[128];
	struct timeval tv;
	s64 file_size;
	s32 i;
	s32 res;
	s32 fid;
	
	for (i = 0; i < 3; i++)
	{
		sprintf(str, "file%d", i);
		switch (i)
		{
		case 0:
			file_size = (s64)16 * 1024;
			break;
		case 1:
			file_size = (s64)16 * 1024 * 1024;
			break;
		case 2:
			file_size = (s64)16 * 1024 * 1024 * 1024;
			break;
		}
				

		fid = nvfuse_openfile_path(nvh, str, O_RDWR | O_CREAT, 0);
		if (fid < 0)
		{
			printf(" Error: file open or create \n");
			return -1;
		}
		nvfuse_closefile(nvh, fid);

		gettimeofday(&tv, NULL);
		printf("\n TEST (Fallocate and Deallocate) %d.\n", i);
		printf(" start fallocate %s size %lu \n", str, (long)file_size);
		/* pre-allocation of data blocks*/
		nvfuse_fallocate(nvh, str, 0, file_size);
		printf(" finish fallocate %s size %lu \n", str, (long)file_size);				
		printf(" nvfuse fallocate throughput %.3f MB/s\n", (double)file_size/(1024*1024)/time_since_now(&tv));

		gettimeofday(&tv, NULL);
		printf(" start rmfile %s size %lu \n", str, (long)file_size);
		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0)
		{
			printf(" Error: rmfile = %s\n", str);
			break;
		}
		printf(" nvfuse rmfile throughput %.3f MB/s\n", (double)file_size / (1024 * 1024) / time_since_now(&tv));
	}

	return NVFUSE_SUCCESS;
}

s32 nvfuse_cd(struct nvfuse_handle *nvh, s8 *str)
{
	struct nvfuse_dir_entry dir_temp;	
	struct nvfuse_inode_ctx *d_ictx;
	struct nvfuse_inode *d_inode;
	struct nvfuse_superblock *sb;
	
	sb = nvfuse_read_super(nvh);

	if(str[0] =='/' && strlen(str) == 1){
		nvfuse_set_cwd_ino(nvh, nvfuse_get_root_ino(nvh));
		return NVFUSE_SUCCESS;
	}

	if(nvfuse_lookup(sb, &d_ictx, &dir_temp, str, nvfuse_get_cwd_ino(nvh)) < 0)
		return error_msg(" invalid dir path ");

	d_inode = d_ictx->ictx_inode;

	if(d_inode->i_type == NVFUSE_TYPE_DIRECTORY){
		nvfuse_set_cwd_ino(nvh, dir_temp.d_ino);
	}else{
		return error_msg(" invalid dir path ");
	}
	nvfuse_release_inode(sb, d_ictx, CLEAN);
	nvfuse_release_super(sb);
	return NVFUSE_SUCCESS;
}

void nvfuse_test(struct nvfuse_handle *nvh)
{
	s32 i, k, j;
	s8 str[128];
	s32 nr = 100000;
	s32 iter = 2;
	
	memset(str, 0, 128);

	for (k = 0; k < iter; k++)
	{
		/* create files*/
		for (i = 0; i < nr; i++)
		{
			sprintf(str, "file%d", i);
			nvfuse_mkfile(nvh, str, "4096");			
		}		

		/* lookup files */
		for (i = 0; i < nr; i++)
		{
			struct stat st_buf;
			int res;

			sprintf(str, "file%d", i);
			res = nvfuse_getattr(nvh, str, &st_buf);
			if (res)
				printf(" No such file %s\n", str);
		}
		
		/* delete files */
		for (i = 0; i < nr; i++)
		{
			sprintf(str, "file%d", i);
			nvfuse_rmfile_path(nvh, str);
		}
	}
	
	for (k = 0; k < iter; k++)
	{
		/* create directories */
		for (i = 0; i < nr; i++)
		{
			sprintf(str, "dir%d", i);
			nvfuse_mkdir_path(nvh, str, 0644);
		}

		/* lookup files */
		for (i = 0; i < nr; i++)
		{
			struct stat st_buf;
			int res;

			sprintf(str, "dir%d", i);
			res = nvfuse_getattr(nvh, str, &st_buf);
			if (res)
				printf(" No such dir %s\n", str);
		}
		
		/* delete directories */
		for (i = 0; i < nr; i++)
		{
			sprintf(str, "dir%d", i);
			nvfuse_rmdir_path(nvh, str);
		}
	}	
}

s32 nvfuse_type(struct nvfuse_handle *nvh, s8 *str)
{
	u32 i, size, offset = 0;
	s32 fid, read_block_size = CLUSTER_SIZE;
	s8 b[CLUSTER_SIZE+1];
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);

	fid = nvfuse_openfile_path(nvh, str,O_RDWR|O_CREAT, 0);

	if (fid!=-1)
	{
		size = sb->sb_file_table[fid].size;
		
		for (i=size; i>=read_block_size;
			i-=read_block_size,offset+=read_block_size)
		{
			nvfuse_readfile(nvh, fid,b,read_block_size, 0);
			b[read_block_size]	= '\0';
			printf("%s",b);
		}

		nvfuse_readfile(nvh, fid,b, i, 0); /* write remainder */

		b[i] = '\0';

		printf("%s",b);
	}

	printf("\n");

	nvfuse_closefile(nvh, fid);
	
	return NVFUSE_SUCCESS;
}

s32 nvfuse_rdfile(struct nvfuse_handle *nvh, s8 *str){	
	struct nvfuse_superblock *sb;
	u32 i, size, offset = 0;
	s32 fid, read_block_size = CLUSTER_SIZE * 32;
	s8 b[CLUSTER_SIZE * 32];

	sb = nvfuse_read_super(nvh);

	fid = nvfuse_openfile_path(nvh, str, O_RDWR, 0);

	if (fid!=-1)
	{
		size = sb->sb_file_table[fid].size;


		for (i=size; i>=read_block_size;
			i-=read_block_size,offset+=read_block_size)
		{
			nvfuse_readfile(nvh, fid,b,read_block_size, 0);
		}

		nvfuse_readfile(nvh, fid, b, i, 0); /* write remainder */
	}

	nvfuse_closefile(nvh, fid);

	return NVFUSE_SUCCESS;
}

void nvfuse_srand(long seed)
{
#if (NVFUSE_OS==NVFUSE_OS_LINUX)
	srand48(seed);
#else
	srand(seed);
#endif
}

s64 nvfuse_rand()
{
	s64 val;
#if (NVFUSE_OS==NVFUSE_OS_LINUX)
	val = mrand48();
#else
	val = (s64)(rand()) << 32;
	val += rand();
#endif
	return val;
}

void nvfuse_rusage_diff(struct rusage *x, struct rusage *y, struct rusage *result)
{
	timeval_subtract(&result->ru_stime,
					&y->ru_stime,
					&x->ru_stime);

	timeval_subtract(&result->ru_utime,
					&y->ru_utime,
					&x->ru_utime);

	result->ru_maxrss = y->ru_maxrss - x->ru_maxrss;        /* maximum resident set size */
	result->ru_ixrss = y->ru_ixrss - x->ru_ixrss;         /* integral shared memory size */
    result->ru_idrss = y->ru_idrss - x->ru_idrss;         /* integral unshared data size */
	result->ru_isrss = y->ru_isrss - x->ru_isrss;         /* integral unshared stack size */
    result->ru_minflt = y->ru_minflt - x->ru_minflt;        /* page reclaims (soft page faults) */
    result->ru_majflt = y->ru_majflt - x->ru_majflt;        /* page faults (hard page faults) */
    result->ru_nswap = y->ru_nswap - x->ru_nswap;         /* swaps */
    result->ru_inblock = y->ru_inblock - x->ru_inblock;       /* block input operations */
	result->ru_oublock = y->ru_oublock - x->ru_oublock;       /* block output operations */
    result->ru_msgsnd = y->ru_msgsnd - x->ru_msgsnd;        /* IPC messages sent */    
    result->ru_msgrcv = y->ru_msgrcv - x->ru_msgrcv;        /* IPC messages received */
    result->ru_nsignals = y->ru_nsignals - x->ru_nsignals;      /* signals received */
    result->ru_nvcsw = y->ru_nvcsw - x->ru_nvcsw;         /* voluntary context switches */
    result->ru_nivcsw = y->ru_nivcsw - x->ru_nivcsw;        /* involuntary context switches */
}

void nvfuse_rusage_add(struct rusage *x, struct rusage *result)
{
	timeval_add(&result->ru_stime,
					&x->ru_stime);

	timeval_add(&result->ru_utime,
					&x->ru_utime);

	result->ru_maxrss += x->ru_maxrss;        /* maximum resident set size */
	result->ru_ixrss += x->ru_ixrss;         /* integral shared memory size */
    result->ru_idrss += x->ru_idrss;         /* integral unshared data size */
	result->ru_isrss += x->ru_isrss;         /* integral unshared stack size */
    result->ru_minflt += x->ru_minflt;        /* page reclaims (soft page faults) */
    result->ru_majflt += x->ru_majflt;        /* page faults (hard page faults) */
    result->ru_nswap += x->ru_nswap;         /* swaps */
    result->ru_inblock += x->ru_inblock;       /* block input operations */
	result->ru_oublock += x->ru_oublock;       /* block output operations */
    result->ru_msgsnd += x->ru_msgsnd;        /* IPC messages sent */    
    result->ru_msgrcv += x->ru_msgrcv;        /* IPC messages received */
    result->ru_nsignals += x->ru_nsignals;      /* signals received */
    result->ru_nvcsw += x->ru_nvcsw;         /* voluntary context switches */
    result->ru_nivcsw += x->ru_nivcsw;        /* involuntary context switches */
}

void print_rusage(struct rusage *rusage, char *prefix, int divisor, double total_exec)
{
//	printf(" usr %f sec \n", (double)tv_to_sec(&rusage->ru_utime));
//	printf(" sys %f sec \n", (double)tv_to_sec(&rusage->ru_stime));

	printf(" %s usr cpu utilization = %3.0f %% (%f sec)\n", prefix, 
			(double)tv_to_sec(&rusage->ru_utime) / (double)divisor / total_exec * 100,
			total_exec);
	printf(" %s sys cpu utilization = %3.0f %% (%f sec)\n", prefix, 
			(double)tv_to_sec(&rusage->ru_stime) / (double)divisor / total_exec * 100,
			total_exec);
	printf(" %s max resider set size = %ld \n", prefix, rusage->ru_maxrss / divisor);
	printf(" %s integral share memory size = %ld \n", prefix, rusage->ru_ixrss / divisor);
	printf(" %s integral unshared data size = %ld \n", prefix, rusage->ru_idrss / divisor);
	printf(" %s integral unshared stack size = %ld \n", prefix, rusage->ru_isrss / divisor);
	printf(" %s page reclaims (soft page faults) = %ld \n", prefix, rusage->ru_minflt / divisor);
	printf(" %s page reclaims (hard page faults) = %ld \n", prefix, rusage->ru_majflt / divisor);
	printf(" %s swaps = %ld \n", prefix, rusage->ru_nswap / divisor);
	printf(" %s block input operations = %ld \n", prefix, rusage->ru_inblock / divisor);
	printf(" %s block output operations = %ld \n", prefix, rusage->ru_oublock / divisor);
	printf(" %s IPC messages sent = %ld \n", prefix, rusage->ru_msgsnd / divisor);
	printf(" %s IPC messages received = %ld \n", prefix, rusage->ru_msgrcv / divisor);
	printf(" %s signals received = %ld \n", prefix, rusage->ru_nsignals / divisor);
	printf(" %s voluntary context switches = %ld \n", prefix, rusage->ru_nvcsw / divisor);
	printf(" %s involuntary context switches = %ld \n", prefix, rusage->ru_nivcsw / divisor);
}