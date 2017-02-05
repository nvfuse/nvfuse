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
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <assert.h>
#include "nvfuse_core.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_types.h"
#include "nvfuse_malloc.h"

static int file_open(struct nvfuse_io_manager *io_manager, int flags);
static int file_close(struct nvfuse_io_manager *io_manager);
static int file_read_blk(struct nvfuse_io_manager *io_manager, long block,
							  int count, void *buf);
static int file_write_blk(struct nvfuse_io_manager *io_manager, long block,
							   int count, void *buf);

/* dev_size in MB units */
void nvfuse_init_fileio(struct nvfuse_io_manager *io_manager, char *name, char *path, s32 dev_size)
{
	int len;

	len = strlen(path)+1;
		
	io_manager->dev_path = (char *)nvfuse_malloc(len);
	memset(io_manager->dev_path, 0x00, len);
	strcpy(io_manager->dev_path, path);
	
	len = strlen(name)+1;		
	io_manager->io_name = (char *)nvfuse_malloc(len);	
	memset(io_manager->io_name, 0x00, len);
	strcpy(io_manager->io_name, name);

	io_manager->io_open = file_open;
	io_manager->io_close = file_close;
	io_manager->io_read = file_read_blk;
	io_manager->io_write = file_write_blk;
	io_manager->dev_format = NULL;

	io_manager->total_blkcount = (s64)dev_size * NVFUSE_MEGA_BYTES / SECTOR_SIZE;
}

static int file_open(struct nvfuse_io_manager *io_manager, int flags)
{
	struct stat	st;
	FILE *fp;
	char buf[CLUSTER_SIZE];
	int	retval = 0;	
	int i;	
	s64 max;

	memset(buf, 0x00, CLUSTER_SIZE);

	fp = fopen(io_manager->dev_path, "rb+");
	if (fp == NULL) {
		fp = fopen(io_manager->dev_path, "wb+");
		if(fp == NULL){
			printf(" fopen error\n");
			retval = -1;
			goto cleanup;
		}
		
		max = io_manager->total_blkcount * 512;
		max = max / CLUSTER_SIZE;
		printf("create file for FILE IO\n");
		for (i = 0; i < max; i++) {
			fwrite(buf, CLUSTER_SIZE, 1, fp);
		}
	}


	io_manager->fp = fp;

	printf(" File Disk Init\n");

cleanup:

	return retval;
}


static int file_close(struct nvfuse_io_manager *io_manager)
{	
	int	retval = 0;
	
	fclose(io_manager->fp);	
	
	free(io_manager->dev_path);
	free(io_manager->io_name);
	
	return retval;
}

static int file_read_blk(struct nvfuse_io_manager *io_manager, long block,
							  int count, void *buf)
{
	int	size, ret;	
	#if (_FILE_OFFSET_BITS==64)
	s64 location;	
	#else
	s32	location;
	#endif
	
	size = count * CLUSTER_SIZE;
	location = ((s64) block * CLUSTER_SIZE);
		
	#if (_FILE_OFFSET_BITS==64)
	fsetpos64(io_manager->fp, (fpos64_t *)(&location));
	#else
	fsetpos(io_manager->fp, (fpos_t *)(&location));
	#endif
	ret = fread(buf, size, 1, io_manager->fp);
	
	if(ret)
		return size;
	else 
		return -1;
}

static int file_write_blk(struct nvfuse_io_manager *io_manager, long block,
							   int count, void *buf)
{
	int	size, ret;	
	#if (FILE_OFFSET_BITS==64)
	s64 location;	
	#else
	s32	location;
	#endif	

	/* debug code */
	#if 0
	if (block % 32768 == NVFUSE_SUMMARY_OFFSET)
	{
		struct nvfuse_segment_summary *ss = buf;
		assert(ss->ss_dbitmap_size);		
	}
	#endif

	size = count * CLUSTER_SIZE;
	location = ((s64) block * CLUSTER_SIZE);
		
	#if (_FILE_OFFSET_BITS==64)
	fsetpos64(io_manager->fp, (fpos64_t *)&location);
	#else
	fsetpos(io_manager->fp, (fpos_t *)(&location));
	#endif
	ret = fwrite(buf, size, 1, io_manager->fp);
	
	if(ret)
		return size;
	else 
		return -1;	
}

