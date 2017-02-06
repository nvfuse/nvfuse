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
#include <dirent.h>

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

extern struct nvfuse_handle *g_nvh;

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
	aioq->aio_lat_total_tsc += latency_tsc;
	aioq->aio_lat_total_count++;
	aioq->aio_lat_min_tsc = MIN_NVFUSE(latency_tsc, aioq->aio_lat_min_tsc);
	aioq->aio_lat_max_tsc = MAX_NVFUSE(latency_tsc, aioq->aio_lat_max_tsc);
	aioq->aio_total_size += actx->actx_bytes;
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

s32 nvfuse_aio_test_rw(struct nvfuse_handle *nvh, s8 *str, s64 file_size, u32 io_size, u32 qdepth, u32 is_read, u32 is_direct, u32 is_rand)
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

		//printf(" Submission depth = %d\n", aioq.aio_cur_depth);
		/* aio completion */		
		ret = nvfuse_aio_queue_completion(&nvh->nvh_sb, &aioq);
		if (ret)
		{
			printf(" Error: queue completion \n");
			goto CLOSE_FD;
		}
		//printf(" completion depth = %d\n", aioq.aio_cur_depth);
	}

CLOSE_FD:
	nvfuse_aio_queue_deinit(&aioq);
	nvfuse_free_aligned_buffer(user_ctx.user_buf);
	nvfuse_fsync(nvh, user_ctx.fd);
	nvfuse_closefile(nvh, user_ctx.fd);
	
	return 0;
}

#define IS_NOTHING      0
#define IS_OPEN_CLOSE   1
#define IS_READDIR      2
#define IS_UNLINK       3
#define IS_CREATE       4
#define IS_RENAME       5
#define IS_MKDIR        6
#define IS_RMDIR        7
#define IS_OP           8

#define DEBUG 0
#define DEBUG_TIME 1
#define DEBUG_FSYNC 0

char *op_list[IS_OP] = {"nothing", "open_close","readdir", "unlink","creat", "rename", "mkdir", "rmdir"};

static int print_timeval(struct timeval *tv, struct timeval *tv_end, int op_index){

    double tv_s = tv->tv_sec + (tv->tv_usec / 1000000.0);
    double tv_e = tv_end->tv_sec + (tv_end->tv_usec / 1000000.0);
#if DEBUG_TIME
    printf("    %s start : %lf  micro seconds \n",op_list[op_index],tv_s);
    printf("    %s end   : %lf  micro seconds \n",op_list[op_index],tv_e);
#endif
    printf("    %s : %lf              seconds \n", op_list[op_index],tv_e -tv_s);
}

s32 nvfuse_metadata_test(struct nvfuse_handle *nvh, s8 *str,s32 meta_check, s32 count)
{
	struct timeval tv,tv_end;
    struct dirent cur_dirent;

	s32 flags_create, flags_rdwr, state, fid, par_ino,i;
    
    char *path_dir = "test_direcpty";
    char *path_file = "test_file.txt";
    char *path_cur = ".";
    off_t offset=0;

    char buf[20] = {0,};
    char rename_buf[20] = {0,};

    flags_create = O_WRONLY | O_CREAT | O_TRUNC;
    flags_rdwr = O_RDWR;

#if DEBUG
    int test_val = 0;
    printf("nvfuse_metadata_test %d \n",test_val++);
#endif

    if(meta_check != IS_NOTHING){

        switch(meta_check){
            /* measure the open/close operation to exisisting file */
            case IS_OPEN_CLOSE :
                printf("metadata operation - open_close operation ...\n");
                
                fid = nvfuse_openfile_path(nvh,path_file,flags_create,0644);
                if(fid == NVFUSE_ERROR){
                    printf("\tError in meta check(IS_OPEN_CLOSE) : %s file create error (before open/close) \n",path_file);
                    return NVFUSE_ERROR;
                }
                state = nvfuse_writefile(nvh, fid, str, sizeof(str), 0);
                if(state == NVFUSE_ERROR){
                    printf("\tError in meta check(IS_OPEN_CLOSE) : file write error (before open/close) \n");
                    return NVFUSE_ERROR;
                }
#if DEBUG_FSYNC
                nvfuse_fsync(nvh,fid);
#endif
                nvfuse_closefile(nvh, fid);

                gettimeofday(&tv,NULL);
                /* measure point  */
                for(i=0; i < count ; i++){
                    fid = nvfuse_openfile_path(nvh,path_file,flags_rdwr,0644);
                    if (fid < 0){
                        printf("\tError in meta check(IS_OPEN_CLOSE) : file open error (measuring open/close) \n");
                        return NVFUSE_ERROR;
                    }
                    nvfuse_closefile(nvh,fid);
                }
                /* measure point  */
                gettimeofday(&tv_end,NULL);

                state = nvfuse_unlink(nvh, path_file);
                if(state == NVFUSE_ERROR){
                    printf("\tError in meta check(IS_OPEN_CLOSE) : %s  file delelte error (after open/close) \n",path_file);
                    return NVFUSE_ERROR;
                }

                print_timeval(&tv, &tv_end,IS_OPEN_CLOSE);
                break;

                /* measure the readdir operation to existing directory */
            case IS_READDIR :
                printf("metadata operation - readdir operation ...\n");
                state = nvfuse_mkdir_path(nvh, path_dir, 0755);
                if(state == NVFUSE_ERROR){
                    printf("\tError in meta check(IS_READDIR) : %s  directory create error (before readdir) \n", path_dir);
                    return NVFUSE_ERROR;
                }

                for(i=0; i < 3 ; i++){
                    sprintf(buf,"%s/%d.txt",path_dir,i);
                    fid = nvfuse_openfile_path(nvh,buf,flags_create,0644);
                    if (fid == NVFUSE_ERROR){
                        printf("Error in meta check(IS_READDIR) : %s file create error (before readdir) \n",buf);
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
#if DEBUG_FSYNC
                    nvfuse_fsync(fd);
#endif                    
                    close(fid);
                }
                s32 par_ino = nvfuse_opendir(nvh,path_dir);

                gettimeofday(&tv,NULL);
                /* measure point  */
                for(i=0; i < count ; i++){
                    while(nvfuse_readdir(nvh,par_ino,&cur_dirent,offset)){
                        offset = offset + 1;
                    }
                }
                /* measure point  */
                gettimeofday(&tv_end,NULL);


                for(i=0; i < 3 ; i++){
                    sprintf(buf,"%s/%d.txt",path_dir,i);
                    state = nvfuse_unlink(nvh,buf);
                    if(state == NVFUSE_ERROR){
                        printf("Error in meta check(IS_READDIR) : %s unlink error (after readdir) \n",buf);
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
                }

                state = nvfuse_rmdir_path(nvh, path_dir);
                if(state == NVFUSE_ERROR){
                    printf("Error in meta check(IS_READDIR) : %s  directory delete error (after readdir) \n",path_dir);
                    return NVFUSE_ERROR;
                }
                print_timeval(&tv, &tv_end,IS_READDIR);
                break;

                /* measure the unlink operation to empty file */
            case IS_UNLINK :
                printf("metadata operation - unlink operation ...\n");

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    fid = nvfuse_openfile_path(nvh,buf,flags_create,0644);
                    if(fid == NVFUSE_ERROR){
                        printf("Error in meta check(IS_UNLINK) : %s  file create error (before unlink) \n",buf);
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
#if DEBUG_FSYNC
                    nvfuse_fsync(nvh,fid);
#endif              
                    nvfuse_closefile(nvh,fid);
                }

                gettimeofday(&tv,NULL);
                /* measure point  */
                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    state = nvfuse_unlink(nvh, buf);
                    if(state == NVFUSE_ERROR){
                        printf("Error in meta check(IS_UNLINK) : %s  file unlink error (measuring unlink) \n",buf);
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
                }
                /* measure point  */
                gettimeofday(&tv_end,NULL);

                print_timeval(&tv, &tv_end, IS_UNLINK);
                break;

                /* measure the create operation to empty file */
            case IS_CREATE :
                printf("metadata operation - create operation ...\n");

                gettimeofday(&tv,NULL);
                /* measure point  */

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    fid = nvfuse_openfile_path(nvh,buf,flags_create,0644);
                    if(fid == NVFUSE_ERROR){
                        printf("Error in meta check(IS_CREATE) : file create error (measuring create) \n");
                        return NVFUSE_ERROR;
                    }

                    memset(buf,0x0,sizeof(buf));
#if DEBUG_FSYNC
                    nvfuse_fsync(nvh,fd);
#endif              
                    nvfuse_closefile(nvh,fid);
                }

                /* measure point  */
                gettimeofday(&tv_end,NULL);

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    state = nvfuse_unlink(nvh, buf);
                    if(state == NVFUSE_ERROR){
                        printf("Error in meta check(IS_CREATE) : file delelte error (after create) \n");
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
                }
                print_timeval(&tv,&tv_end,IS_CREATE);
                break;

                /* measure the rename operation to existing file */
            case IS_RENAME :
                printf("metadata operation - rename operation ...\n");
                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    fid = nvfuse_openfile_path(nvh,buf,flags_create,0664);
                    if(fid == NVFUSE_ERROR){
                        printf("Error in meta check(IS_RENAME) : file create error (before rename) \n");
                        return NVFUSE_ERROR;
                    }
#if DEBUG_FSYNC
                    nvfuse_fsync(nvh,fid);
#endif              
                    nvfuse_closefile(nvh,fid);
                    memset(buf,0x0,sizeof(buf));
                }

                gettimeofday(&tv,NULL);
                /* measure point  */

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    sprintf(rename_buf, "%d_rename.txt",i);
                    state = nvfuse_rename_path(nvh, buf, rename_buf);
                    if(state == NVFUSE_ERROR){
                        printf("Error in meta check(IS_RENAME) : rename error (measuring rename) \n");
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
                    memset(rename_buf,0x0,sizeof(rename_buf));
                }

                /* measure point  */
                gettimeofday(&tv_end,NULL);

                for(i=0; i < count ; i++){
                    sprintf(rename_buf, "%d_rename.txt",i);
                    state = nvfuse_rmfile_path(nvh,rename_buf);
                    if(state == NVFUSE_ERROR){
                        printf("Error in meta check(IS_RENAME) : file delelte error (after rename) \n");
                        return NVFUSE_ERROR;
                    }
                    memset(rename_buf,0x0,sizeof(rename_buf));
                }

                print_timeval(&tv,&tv_end,IS_RENAME);
                break;

                /* measure the mkdir operation */
            case IS_MKDIR :
                printf("metadata operation - mkdir operation ...\n");

                gettimeofday(&tv,NULL);
                /* measure point  */
                for(i=0; i < count ; i++){
                    sprintf(buf,"%d",i);
                    state = nvfuse_mkdir_path(nvh,buf, 0755);
                    if(state == NVFUSE_ERROR){
                        printf("Error in meta check(IS_MKDIR) : directory create error (measuring mkdir) \n");
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
                }

                /* measure point  */
                gettimeofday(&tv_end,NULL);

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d",i);
                    state = nvfuse_rmdir_path(nvh, buf);
                    if(state == NVFUSE_ERROR){
                        printf("Error in meta check(IS_MKDIR) : directory delete error (after mkdir) \n");
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
                }

                print_timeval(&tv,&tv_end,IS_MKDIR);               
                break;

            case IS_RMDIR :
                printf("metadata operation - rmdir operation ...\n");

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d",i);
                    state = nvfuse_mkdir_path(nvh, buf, 0755);
                    if(state == NVFUSE_ERROR){
                        printf("Error in meta check(IS_RMDIR) : directory create error (before rmdir) \n");
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
                }

                gettimeofday(&tv,NULL);
                /* measure point  */

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d",i);
                    state = nvfuse_rmdir_path(nvh, buf);
                    if(state == NVFUSE_ERROR){
                        printf("Error in meta check(IS_RMDIR) : directory delete error (measuring rmdir) \n");
                        return NVFUSE_ERROR;
                    }
                    memset(buf,0x0,sizeof(buf));
                }
                    
                /* measure point  */
                gettimeofday(&tv_end,NULL);

                print_timeval(&tv,&tv_end,IS_RMDIR);               
                break;
            default:
                printf("Invalid metadata type setting  Error\n");
                break;

        }     
    }else{
        printf("\t metadata option is not selected \n");
    }
   
    nvfuse_sync(nvh);
    return NVFUSE_SUCCESS;

CLOSE_META:
    
	return NVFUSE_ERROR;
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
		res = nvfuse_aio_test_rw(nvh, str, file_size, 4096, AIO_MAX_QDEPTH, WRITE, direct, 0 /* sequential */);
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
		res = nvfuse_aio_test_rw(nvh, str, file_size, 4096, AIO_MAX_QDEPTH, READ, direct, 0 /* sequential */);
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
