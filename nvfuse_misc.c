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
#include "time.h"

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
#include <windows.h>
#endif

//extern u32 CWD;
//extern u32 ROOT_DIR;
extern struct nvfuse_handle *g_nvh;

s32 nvfuse_mkfile(struct nvfuse_handle *nvh, s8 *str, s8 *ssize){
	u64 size;
	s32 i = CLUSTER_SIZE, fd, write_block_size = CLUSTER_SIZE;
	s32 ret = 0;
	s32 num_block;
	s8 file_source[CLUSTER_SIZE];

	if(strlen(str) < 1 || strlen(str) >= FNAME_SIZE)
		return error_msg("mkfile  [filename] [size]\n");

	if(atoi(ssize) ==0)
		return error_msg("mkfile  [filename] [size]\n");

	fd = nvfuse_openfile_path(nvh, str, O_RDWR|O_CREAT, 0);

	if (fd!=-1)
	{
		size = atoi(ssize);

		if(size  < 1) size = CLUSTER_SIZE;

		for (size; size>=write_block_size;
			size-=write_block_size){
				ret = nvfuse_writefile(nvh, fd,file_source,write_block_size, 0);

				if(ret == -1)
					return -1;
		}
		nvfuse_writefile(nvh, fd,file_source, i, 0); /* write remainder */
	}
	
	nvfuse_closefile(nvh, fd);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_cd(struct nvfuse_handle *nvh, s8 *str)
{
	struct nvfuse_dir_entry dir_temp;
	struct nvfuse_inode *d_inode;
	struct nvfuse_superblock *sb;
	
	sb = nvfuse_read_super(nvh);

	if(str[0] =='/' && strlen(str) == 1){
		nvfuse_set_cwd_ino(nvh, nvfuse_get_root_ino(nvh));
		return NVFUSE_SUCCESS;
	}

	if(nvfuse_lookup(sb, &d_inode, &dir_temp, str, nvfuse_get_cwd_ino(nvh)) < 0)
		return error_msg(" invalid dir path ");

	d_inode = nvfuse_read_inode(sb, dir_temp.d_ino, READ);

	if(d_inode->i_type == NVFUSE_TYPE_DIRECTORY){
		nvfuse_set_cwd_ino(nvh, dir_temp.d_ino);
	}else{
		return error_msg(" invalid dir path ");
	}

	nvfuse_release_super(sb);
	return NVFUSE_SUCCESS;
}



void nvfuse_test(struct nvfuse_handle *nvh)
{
	s32 i;
	s8 str[128];
	float time;

	memset(str, 0, 128);

	/* create files*/
	for(i =0;i < 100000;i++)
	{
		sprintf(str,"file%d",i);
		nvfuse_mkfile(nvh, str, "4096");
	}
	/* delete files */
	for (i = 0; i < 100000; i++)
	{
		sprintf(str, "file%d", i);
		nvfuse_rmfile_path(nvh, str);
	}
	
	/* create directories */
	for (i = 0; i < 100000; i++)
	{
		sprintf(str, "dir%d", i);		
		nvfuse_mkdir_path(nvh, str, 0644);
	}

	/* delete directories */
	for (i = 0; i < 100000; i++)
	{
		sprintf(str, "dir%d", i);
		nvfuse_rmdir_path(nvh, str);
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


