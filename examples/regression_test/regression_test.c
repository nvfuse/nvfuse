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

#define NUM_ELEMENTS(x) (sizeof(x)/sizeof(x[0]))

#define MB (1024*1024)
#define GB (1024*1024*1024)
#define TB ((s64)1024*1024*1024*1024)

#define RT_TEST_TYPE MILL_TEST

#define MAX_TEST    1
#define QUICK_TEST  2
#define MILL_TEST   3

/* 1 million create/delete test */

static s32 last_percent;

void rt_progress_reset()
{
    last_percent = 0;
}

void rt_progress_report(s32 curr, s32 max)
{
    int curr_percent;

    curr_percent = (curr + 1) * 100 / max;

    if	(curr_percent != last_percent)
    {
		last_percent = curr_percent;
		printf(".");
		if (curr_percent % 10 == 0)
		    printf("%d%%\n", curr_percent);
		fflush(stdout);
    }

}

int rt_create_files(struct nvfuse_handle *nvh)
{
	struct timeval tv;
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

#if (RT_TEST_TYPE == MAX_TEST)
	max_inodes = stat.f_ffree; /* # of free inodes */
#elif (RT_TEST_TYPE == QUICK_TEST)
	max_inodes = 100;
#elif (RT_TEST_TYPE == MILL_TEST)
	max_inodes = stat.f_ffree; /* # of free inodes */
	if (max_inodes > 1000000)
	{
	    max_inodes = 1000000;
	}
#endif

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* create null files */
	printf(" Start: creating null files (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		sprintf(buf, "file%d\n", i);

		fd = nvfuse_openfile_path(nvh, buf, O_RDWR | O_CREAT, 0);
		if (fd == -1) {
			printf(" Error: open() \n");
			return -1;
		}
		nvfuse_closefile(nvh, fd);
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: creating null files (0x%x) %.3f OPS.\n", max_inodes, max_inodes / time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* lookup null files */
	printf(" Start: looking up null files (0x%x).\n", max_inodes);
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
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: looking up null files (0x%x) %.3f OPS.\n", max_inodes, max_inodes / time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* delete null files */
	printf(" Start: deleting null files (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		sprintf(buf, "file%d\n", i);

		res = nvfuse_rmfile_path(nvh, buf);
		if (res)
		{
			printf(" rmfile = %s error \n", buf);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: deleting null files (0x%x) %.3f OPS.\n", max_inodes, max_inodes / time_since_now(&tv));

	return 0;
}

int rt_create_dirs(struct nvfuse_handle *nvh)
{
	struct timeval tv;
	struct statvfs stat;
	s8 buf[FNAME_SIZE];
	s32 max_inodes;
	s32 i;
	s32 res;

	if (nvfuse_statvfs(nvh, NULL, &stat) < 0)
	{
		printf(" statfs error \n");
		return -1;
	}

#if (RT_TEST_TYPE == MAX_TEST)
	max_inodes = stat.f_ffree; /* # of free inodes */
#elif (RT_TEST_TYPE == QUICK_TEST)
	max_inodes = 100;
#elif (RT_TEST_TYPE == MILL_TEST)
	max_inodes = stat.f_ffree; /* # of free inodes */
	if (max_inodes > 1000000)
	{
	    max_inodes = 1000000;
	}
#endif
	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* create null directories */
	printf(" Start: creating null directories (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		sprintf(buf, "dir%d\n", i);
		res = nvfuse_mkdir_path(nvh, buf, 0644);
		if (res < 0)
		{
			printf(" Error: create dir = %s \n", buf);
			return res;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);

	}
	printf(" Finish: creating null directories (0x%x) %.3f OPS.\n", max_inodes, max_inodes / time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);
	/* lookup null directories */
	printf(" Start: looking up null directories (0x%x).\n", max_inodes);
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
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: looking up null directories (0x%x) %.3f OPS.\n", max_inodes, max_inodes / time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);
	/* delete null directories */
	printf(" Start: deleting null directories (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++)
	{
		sprintf(buf, "dir%d\n", i);

		res = nvfuse_rmdir_path(nvh, buf);
		if (res)
		{
			printf(" rmfile = %s error \n", buf);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: deleting null files (0x%x) %.3f OPS.\n", max_inodes, max_inodes / time_since_now(&tv));

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

#if (RT_TEST_TYPE == MAX_TEST)
	file_size = (s64) statvfs_buf.f_bfree * CLUSTER_SIZE;
#elif (RT_TEST_TYPE == QUICK_TEST)
	file_size = 100 * MB;
#elif (RT_TEST_TYPE == MILL_TEST)	
	file_size = (s64)1 * TB;
	file_size = (file_size > (s64)statvfs_buf.f_bfree * CLUSTER_SIZE) ?	
				(s64)statvfs_buf.f_bfree * CLUSTER_SIZE : 
				(s64)1 * TB;
#endif

	fid = nvfuse_openfile_path(nvh, str, O_RDWR | O_CREAT, 0);
	if (fid < 0)
	{
		printf(" Error: file open or create \n");
		return -1;
	}
	nvfuse_closefile(nvh, fid);

	gettimeofday(&tv, NULL);
	printf("\n Start: Fallocate and Deallocate (file %s size %luMB). \n", str, (long)file_size/MB);
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

	printf(" nvfuse fallocate throughput %.3fMB/s (%0.3fs).\n", (double)file_allocated_size/MB/time_since_now(&tv), time_since_now(&tv));

	gettimeofday(&tv, NULL);
	printf(" Start: rmfile %s size %luMB \n", str, (long)file_allocated_size/MB);
	res = nvfuse_rmfile_path(nvh, str);
	if (res < 0)
	{
		printf(" Error: rmfile = %s\n", str);
		return -1;
	}
	printf(" nvfuse rmfile throughput %.3fMB/s\n", (double)file_allocated_size/MB/time_since_now(&tv));

	printf("\n Finish: Fallocate and Deallocate.\n");

	return NVFUSE_SUCCESS;
}

int rt_create_max_sized_file_aio(struct nvfuse_handle *nvh)
{
	struct statvfs statvfs_buf;
	char str[FNAME_SIZE];
	struct timeval tv;
	s64 file_size;
	s32 res;
	s32 direct = 1;

	if (nvfuse_statvfs(nvh, NULL, &statvfs_buf) < 0)
	{
		printf(" statfs error \n");
		return -1;
	}

	sprintf(str, "file_allocate_test");

#if (RT_TEST_TYPE == MAX_TEST)
	file_size = (s64) statvfs_buf.f_bfree * CLUSTER_SIZE;
#elif (RT_TEST_TYPE == QUICK_TEST)
	file_size = 100 * MB;
#elif (RT_TEST_TYPE == MILL_TEST)
	file_size = (s64)1 * TB;
	file_size = (file_size > (s64)statvfs_buf.f_bfree * CLUSTER_SIZE) ?
		(s64)statvfs_buf.f_bfree * CLUSTER_SIZE :
		(s64)1 * TB;
#endif

	gettimeofday(&tv, NULL);

	/* write phase */
	{
		res = nvfuse_aio_test_rw(nvh, str, file_size, 4096, AIO_MAX_QDEPTH, WRITE, direct);
		if (res < 0)
		{
			printf(" Error: aio write test \n");
			return -1;
		}
		printf(" nvfuse aio write through %.3fMB/s\n", (double)file_size/MB/time_since_now(&tv));

		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0)
		{
			printf(" Error: rmfile = %s\n", str);
			return -1;
		}
	}

	/* read phase */
	{
		res = nvfuse_aio_test_rw(nvh, str, file_size, 4096, AIO_MAX_QDEPTH, READ, direct);
		if (res < 0)
		{
			printf(" Error: aio read test \n");
			return -1;
		}
		printf(" nvfuse aio read through %.3fMB/s\n", (double)file_size/MB/time_since_now(&tv));

		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0)
		{
			printf(" Error: rmfile = %s\n", str);
			return -1;
		}
	}

	return NVFUSE_SUCCESS;

}

int rt_create_4KB_files(struct nvfuse_handle *nvh)
{
	struct timeval tv;
	struct statvfs statvfs_buf;
	char str[FNAME_SIZE];
	s32 res;
	s32 nr;
	int i;

	if (nvfuse_statvfs(nvh, NULL, &statvfs_buf) < 0)
	{
		printf(" statfs error \n");
		return -1;
	}

#if (RT_TEST_TYPE == MAX_TEST)
	nr = statvfs_buf.f_bfree / 2;
#elif (RT_TEST_TYPE == QUICK_TEST)
	nr = 100;
#elif (RT_TEST_TYPE == MILL_TEST)
	nr = statvfs_buf.f_bfree / 2;
	if (nr > 1000000)
	{
	    nr = 1000000;
	}
#endif

	printf(" # of files = %d \n", nr);

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	printf(" Start: creating 4KB files (0x%x).\n", nr);
	/* create files*/
	for (i = 0; i < nr; i++)
	{
		sprintf(str, "file%d", i);
		res = nvfuse_mkfile(nvh, str, "4096");			
		if (res < 0)
		{
			printf(" mkfile error = %s\n", str);
			return -1;
		}

		/* update progress percent */
		rt_progress_report(i, nr);
	}		
	printf(" Finish: creating 4KB files (0x%x) %.3f OPS.\n", nr, nr / time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	printf(" Start: looking up 4KB files (0x%x).\n", nr);
	/* lookup files */
	for (i = 0; i < nr; i++)
	{
		struct stat st_buf;
		int res;

		sprintf(str, "file%d", i);
		res = nvfuse_getattr(nvh, str, &st_buf);
		if (res)
		{
			printf(" No such file %s\n", str);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, nr);
	}
	printf(" Finish: looking up 4KB files (0x%x) %.3f OPS.\n", nr, nr / time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* delete files */
	printf(" Start: deleting 4KB files (0x%x).\n", nr);
	for (i = 0; i < nr; i++)
	{
		sprintf(str, "file%d", i);
		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0)
		{
			printf(" rmfile error = %s \n", str);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, nr);
	}
	printf(" Finish: deleting 4KB files (0x%x) %.3f OPS.\n", nr, nr / time_since_now(&tv));

	return NVFUSE_SUCCESS;

}

struct regression_test_ctx
{
	s32 (*function)(struct nvfuse_handle *nvh);
	s8 test_name[128];
	s32 pass_criteria; /* compare return code */
	s32 pass_criteria_ignore; /* no compare */
} 
rt_ctx[] = 
{
	{ rt_create_files, "Creating Max Number of Files.", 0, 0},
	{ rt_create_dirs, "Creating Max Number of Directories.", 0, 0},
	{ rt_create_max_sized_file, "Creating Maximum Sized Single File.", 0, 0},
	{ rt_create_max_sized_file_aio, "Creating Maximum Sized Single File with AIO Read and Write.", 0, 0},
	{ rt_create_4KB_files, "Creating 4KB files with fsync.", 0, 0},
};

int main(int argc, char *argv[])
{
	struct nvfuse_handle *nvh;	
	struct regression_test_ctx *cur_rt_ctx;
	char *devname; 
	int ret = 0;

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

	cur_rt_ctx = rt_ctx;

	/* Test Case Handler with Regression Test Context Array */
	while (cur_rt_ctx < rt_ctx + NUM_ELEMENTS(rt_ctx))
	{
		s32 index = cur_rt_ctx - rt_ctx + 1;
		printf(" Regression Test %d: %s\n", index, cur_rt_ctx->test_name);
		ret = cur_rt_ctx->function(nvh);
		if (!cur_rt_ctx->pass_criteria && 
			ret != cur_rt_ctx->pass_criteria)
		{
			printf(" Failed Regression Test %d.\n", index);
			goto RET;
		}

		printf(" Regression Test %d: passed successfully.\n\n", index);
		cur_rt_ctx++;
	}

RET:;
	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);

	return ret;
}
