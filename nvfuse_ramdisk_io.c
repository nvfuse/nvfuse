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
#include "nvfuse_core.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"

static int mem_open(struct nvfuse_io_manager *io_manager, int flags);
static int mem_close(struct nvfuse_io_manager *io_manager);
static int mem_read_blk(struct nvfuse_io_manager *io_manager, unsigned long block,int count, void *buf);
static int mem_write_blk(struct nvfuse_io_manager *io_manager, unsigned long block,int count, void *buf);
static int mem_erase_blk(struct nvfuse_io_manager *io_manager, unsigned long block);
static int mem_readspare(struct nvfuse_io_manager *io_manager, unsigned long block,char *buf);
static int mem_writespare(struct nvfuse_io_manager *io_manager, unsigned long block,char *buf);


void nvfuse_init_memio(struct nvfuse_io_manager *io_manager,char *name,char *path)
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

	io_manager->io_open = mem_open;
	io_manager->io_close = mem_close;
	io_manager->io_read = mem_read_blk;
	io_manager->io_write = mem_write_blk;

	io_manager->total_blkcount = NO_OF_SECTORS;
}


static int mem_open(struct nvfuse_io_manager *io_manager,int flags)
{
	if(DISK_SIZE >= 1024*1024*1024)
		printf(" Disk Size = %.2fGB\n", (float)DISK_SIZE/(float)(1024*1024*1024));
	else if((DISK_SIZE >= 1024*1024))
		printf(" Disk Size = %.2fMB\n", (float)DISK_SIZE/(float)(1024*1024));

	io_manager->ramdisk = nvfuse_malloc((size_t)DISK_SIZE);
	if(io_manager->ramdisk == NULL)
	{
		printf(" nvfuse_malloc error\n");
		return -1;
	}

	printf(" Ram Disk Init\n");
	return 0;

}

static int mem_close(struct nvfuse_io_manager *io_manager)
{
	if(io_manager->ramdisk)
		free(io_manager->ramdisk);
	return 0;

}
 
static int mem_read_blk(struct nvfuse_io_manager *io_manager, unsigned long block,
							   int count, void *buf)
{
	int	size;
	int	location;	
	char *disk;

	size = (count < 0) ? -count : count * CLUSTER_SIZE;
	location = ((int) block * (int)CLUSTER_SIZE);

	disk = io_manager->ramdisk;
	memcpy(buf, &disk[location], size);

	return CLUSTER_SIZE;

}

static int mem_write_blk(struct nvfuse_io_manager *io_manager, unsigned long block,
								int count, void *buf)
{
	int		size;
	int	location;	
	char *disk;

	size = (count < 0) ? -count : count * CLUSTER_SIZE;
	location = ((s32) block * CLUSTER_SIZE);

	disk = io_manager->ramdisk;
	memcpy(disk +  location, buf, size);

	return CLUSTER_SIZE;
}
