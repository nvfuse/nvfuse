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

#include "nvfuse_core.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_config.h"
#include "nvfuse_api.h"
#include "nvfuse_aio.h"
#include "nvfuse_malloc.h"
#include "time.h"

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
#include <windows.h>
#endif

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
		size = atoi(ssize);

		if(size  < 1) size = CLUSTER_SIZE;

		for (size; size>=write_block_size; size-=write_block_size){
			ret = nvfuse_writefile(nvh, fd, file_source, write_block_size, 0);

			if(ret == -1)
				return -1;
		}
		nvfuse_writefile(nvh, fd,file_source, i, 0); /* write remainder */
	}
	
	nvfuse_fsync(nvh, fd);
	nvfuse_closefile(nvh, fd);

	return NVFUSE_SUCCESS;
}

void nvfuse_aio_test_callback(void *arg)
{
	struct nvfuse_aio_ctx *actx = (struct nvfuse_aio_ctx *)arg;
	free(actx->actx_buf);
	free(actx);
}

s32 nvfuse_aio_test_rw(struct nvfuse_handle *nvh, s8 *str, s64 file_size, u32 io_size, u32 qdepth, u32 is_read)
{
	struct nvfuse_aio_queue aioq;
	struct nvfuse_aio_ctx *actx;	
	s32 fd;
	s8 *user_buf;
	s32 ret;
	s32 i;
	s64 io_remaining;
	s64 io_curr;

	printf(" aiotest %s filesize = %ld io_size = %d qdpeth = %d (%c) \n", str, (long)file_size, io_size, qdepth, is_read ? 'R' : 'W');

	fd = nvfuse_openfile_path(nvh, str, O_RDWR | O_CREAT, 0);

	/* pre-allocation of data blocks*/
	nvfuse_fallocate(nvh, str, 0, file_size);

	/* initialization of aio queue */
	ret = nvfuse_aio_queue_init(&aioq, qdepth);
	if (ret)
	{
		printf(" Error: aio queue init () with ret = %d\n ", ret);
		return -1;
	}
	
	io_curr = 0;
	io_remaining = file_size; 
	
	while (io_remaining > 0)
	{
		for (i = 0; i < qdepth; i++)
		{
			/* user data buffer allocation */
			user_buf = nvfuse_malloc(io_size);
			if (user_buf == NULL)
			{
				printf(" Error: malloc()\n");
				return -1;
			}

			/* initialization of aio context */
			actx = malloc(sizeof(struct nvfuse_aio_ctx));
			memset(actx, 0x00, sizeof(struct nvfuse_aio_ctx));
			actx->actx_fid = fd;
			actx->actx_opcode = is_read ? READ : WRITE;
			actx->actx_buf = user_buf;
			actx->actx_offset = io_curr;
			actx->actx_bytes = io_size;
			actx->actx_error = 0;
			INIT_LIST_HEAD(&actx->actx_list);
			actx->actx_cb_func = nvfuse_aio_test_callback;
			actx->actx_sb = &nvh->nvh_sb;

			/* enqueue actx to aio queue */
			ret = nvfuse_aio_queue_enqueue(&aioq, actx, NVFUSE_READY_QUEUE);
			if (ret)
			{
				printf(" Error: Enqueue error\n");
				return ret;
			}

			io_curr += io_size;
			io_remaining -= io_size;
		}
		
		/* aio submission */
		ret = nvfuse_aio_queue_submission(nvh, &aioq);
		if (ret)
		{
			printf(" Error: queue submission \n");
			return ret;
		}

		/* aio completion */
		ret = nvfuse_aio_queue_completion(&nvh->nvh_sb, &aioq);
		if (ret)
		{
			printf(" Error: queue completion \n");
			return ret;
		}
	}
	
	nvfuse_fsync(nvh, fd);
	nvfuse_closefile(nvh, fd);
	
	return 0;
}

s32 nvfuse_aio_test(struct nvfuse_handle *nvh)
{
	char str[128];
	struct timeval tv;
	s64 file_size;
	int i;
	
	for (i = 0; i < 1000; i++)
	{
		sprintf(str, "file%d", i);
		file_size = (s64)4 * 1024 * 1024 * 1024, 4096;
		gettimeofday(&tv, NULL);
		nvfuse_aio_test_rw(nvh, str, file_size, 4096, 128, WRITE);		
		printf(" nvfuse aio through %.3fMB/s\n", (double)file_size/(1024*1024)/time_since_now(&tv));
		nvfuse_rmfile_path(nvh, str);
	}

	for (i = 0; i < 1000; i++)
	{
		sprintf(str, "file%d", i);
		file_size = (s64)4 * 1024 * 1024 * 1024, 4096;
		gettimeofday(&tv, NULL);
		nvfuse_aio_test_rw(nvh, str, file_size, 4096, 128, READ);
		printf(" nvfuse aio through %.3fMB/s\n", (double)file_size/(1024*1024)/time_since_now(&tv));
		nvfuse_rmfile_path(nvh, str);
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


