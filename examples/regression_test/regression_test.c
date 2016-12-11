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
#include <stdlib.h>

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"
#include "nvfuse_gettimeofday.h"

#if NVFUSE_OS == NVFUSE_OS_LINUX
#define EXAM_USE_RAMDISK	0
#define EXAM_USE_FILEDISK	0
#define EXAM_USE_UNIXIO		1
#define EXAM_USE_SPDK		0
#else
#define EXAM_USE_RAMDISK	0
#define EXAM_USE_FILEDISK	1
#define EXAM_USE_UNIXIO		0
#define EXAM_USE_SPDK		0
#endif

#define INIT_IOM	1
#define FORMAT		1
#define MOUNT		1
#define DEINIT_IOM	1
#define UMOUNT		1

#define MB (1024*1024)
#define GB (1024*1024*1024)

int rt_create_files(struct nvfuse_handle *nvh)
{
	struct statvfs stat;
	s8 buf[FNAME_SIZE];
	s32 max_inodes;
	s32 i;
	s32 fd;
	s32 res;

	if (nvfuse_statvfs(nvh, NULL, &stat) < 0)
	{
		printf(" statfs error \n");
		return -1;
	}

	max_inodes = stat.f_ffree; /* # of free inodes */

	/* create null files */
	printf(" Start: creating null files (%d).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		sprintf(buf, "file%d\n", i);

		fd = nvfuse_openfile_path(nvh, buf, O_RDWR | O_CREAT, 0);
		if (fd == -1) {
			printf(" Error: open() \n");
			return -1;
		}
		nvfuse_closefile(nvh, fd);
	}
	printf(" Finish: creating null files (%d).\n", max_inodes);

	/* lookup null files */
	printf(" Start: looking up null files (%d).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		struct stat st_buf;
		int res;

		sprintf(buf, "file%d\n", i);

		res = nvfuse_getattr(nvh, buf, &st_buf);
		if (res) 
		{
			printf(" No such file %s\n", buf);
			return -1;
		}
	}
	printf(" Finish: looking up null files (%d).\n", max_inodes);

	/* delete null files */
	printf(" Start: deleting null files (%d).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		sprintf(buf, "file%d\n", i);

		res = nvfuse_rmfile_path(nvh, buf);
		if (res)
		{
			printf(" rmfile = %s error \n", buf);
			return -1;
		}
	}
	printf(" Finish: deleting null files (%d).\n", max_inodes);

	return 0;
}

int rt_create_dirs(struct nvfuse_handle *nvh)
{
	struct statvfs stat;
	s8 buf[FNAME_SIZE];
	s32 max_inodes;
	s32 i;
	s32 fd;
	s32 res;

	if (nvfuse_statvfs(nvh, NULL, &stat) < 0)
	{
		printf(" statfs error \n");
		return -1;
	}

	max_inodes = stat.f_ffree; /* # of free inodes */

	/* create null directories */
	printf(" Start: creating null directories (%d).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		sprintf(buf, "dir%d\n", i);
		res = nvfuse_mkdir_path(nvh, buf, 0644);
		if (res < 0)
		{
			printf(" Error: create dir = %s \n", buf);
			return res;
		}

	}
	printf(" Finish: creating null directories (%d).\n", max_inodes);

	/* lookup null directories */
	printf(" Start: looking up null directories (%d).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		struct stat st_buf;
		int res;

		sprintf(buf, "dir%d\n", i);

		res = nvfuse_getattr(nvh, buf, &st_buf);
		if (res) 
		{
			printf(" No such directory %s\n", buf);
			return -1;
		}
	}
	printf(" Finish: looking up null directories (%d).\n", max_inodes);

	/* delete null directories */
	printf(" Start: deleting null directories (%d).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		sprintf(buf, "dir%d\n", i);

		res = nvfuse_rmdir_path(nvh, buf);
		if (res)
		{
			printf(" rmfile = %s error \n", buf);
			return -1;
		}
	}
	printf(" Finish: deleting null files (%d).\n", max_inodes);
	return 0;
}

int rt_create_max_sized_file(struct nvfuse_handle *nvh)
{
	struct statvfs statvfs_buf;
	struct stat stat_buf;
	char str[128];
	struct timeval tv;
	s64 file_size;
	s64 file_allocated_size;
	s32 res;
	s32 fid;

	if (nvfuse_statvfs(nvh, NULL, &statvfs_buf) < 0)
	{
		printf(" statfs error \n");
		return -1;
	}

	sprintf(str, "file_allocate_test");

	file_size = (s64) statvfs_buf.f_bfree * CLUSTER_SIZE;

	fid = nvfuse_openfile_path(nvh, str, O_RDWR | O_CREAT, 0);
	if (fid < 0)
	{
		printf(" Error: file open or create \n");
		return -1;
	}
	nvfuse_closefile(nvh, fid);

	gettimeofday(&tv, NULL);
	printf("\n Start: (Fallocate and Deallocate file %s size %luMB \n", str, (long)file_size/MB);
	/* pre-allocation of data blocks*/
	nvfuse_fallocate(nvh, str, 0, file_size);

	res = nvfuse_getattr(nvh, str, &stat_buf);
	if (res) 
	{
		printf(" No such file %s\n", str);
		return -1;
	}

	/* NOTE: Allocated size may differ from requested size. */
	file_allocated_size = stat_buf.st_size;

	printf(" requested size %dMB.\n", (long)file_size/MB);
	printf(" allocated size %dMB.\n", (long)file_allocated_size/MB); 

	printf(" nvfuse fallocate throughput %.3fMB/s (%0.3fs)\n", (double)file_allocated_size/MB/time_since_now(&tv), time_since_now(&tv));

	gettimeofday(&tv, NULL);
	printf(" Start: rmfile %s size %luMB \n", str, (long)file_allocated_size/MB);
	res = nvfuse_rmfile_path(nvh, str);
	if (res < 0)
	{
		printf(" Error: rmfile = %s\n", str);
		return -1;
	}
	printf(" nvfuse rmfile throughput %.3fMB/s\n", (double)file_allocated_size/MB/time_since_now(&tv));

	printf("\n Finish: (Fallocate and Deallocate).\n");

	return NVFUSE_SUCCESS;

}

int main(int argc, char *argv[])
{
	struct nvfuse_handle *nvh;	
	int ret;
	int fd;
	int count;
	char *buf;
	char *devname; 

	devname = argv[1];
	if (argc < 2) {
		printf(" please enter the device file (e.g., /dev/sdb)\n");
		return -1;
	}
	printf(" device name = %s \n", devname);

#	if (EXAM_USE_RAMDISK == 1)
	nvh = nvfuse_create_handle(NULL, devname, INIT_IOM, IO_MANAGER_RAMDISK, FORMAT, MOUNT);
#	elif (EXAM_USE_FILEDISK == 1)
	nvh = nvfuse_create_handle(NULL, devname, INIT_IOM, IO_MANAGER_FILEDISK, FORMAT, MOUNT);
#	elif (EXAM_USE_UNIXIO == 1)
	nvh = nvfuse_create_handle(NULL, devname, INIT_IOM, IO_MANAGER_UNIXIO, FORMAT, MOUNT);
#	elif (EXAM_USE_SPDK == 1)
	nvh = nvfuse_create_handle(NULL, devname, INIT_IOM, IO_MANAGER_SPDK, FORMAT, MOUNT);
#	endif

	printf("\n");

	/* Test 1. */
	printf(" Regression Test 1: Creating Max Number of Files in NVFUSE.\n");
	ret = rt_create_files(nvh);
	if (ret < 0)
	{
		printf(" Failed Regression Test 1\n");
		return -1;
	}
	printf(" Regression Test 1: passed successfully.\n\n");

	/* Test 2. */
	printf(" Regression Test 2: Creating Max Number of Directories in NVFUSE.\n");
	ret = rt_create_dirs(nvh);
	if (ret < 0)
	{
		printf(" Failed Regression Test 2\n");
		return -1;
	}
	printf(" Regression Test 2: passed successfully.\n\n");

	/* Test 3. */
	printf(" Regression Test 3: Creating Max Sized Single File in NVFUSE.\n");
	ret = rt_create_max_sized_file(nvh);
	if (ret < 0)
	{
		printf(" Failed Regression Test 3\n");
		return -1;
	}
	printf(" Regression Test 3: passed successfully.\n");

RET:;
	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);
}
