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
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"

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

int main()
{
	struct nvfuse_handle *nvh;	
	int ret;
	int fd;
	int count;
	char *buf;

#	if (EXAM_USE_RAMDISK == 1)
	nvh = nvfuse_create_handle(NULL, INIT_IOM, IO_MANAGER_RAMDISK, FORMAT, MOUNT);
#	elif (EXAM_USE_FILEDISK == 1)
	nvh = nvfuse_create_handle(NULL, INIT_IOM, IO_MANAGER_FILEDISK, FORMAT, MOUNT);
#	elif (EXAM_USE_UNIXIO == 1)
	nvh = nvfuse_create_handle(NULL, INIT_IOM, IO_MANAGER_UNIXIO, FORMAT, MOUNT);
#	elif (EXAM_USE_SPDK == 1)
	nvh = nvfuse_create_handle(NULL, INIT_IOM, IO_MANAGER_SPDK, FORMAT, MOUNT);
#	endif

	/* file open and create */
	fd = nvfuse_openfile_path(nvh, "helloworld.file", O_RDWR | O_CREAT, 0);
	if (fd == -1) {
		printf(" Error: open() \n");
		goto RET;
	}

	/* 4KB memory allocation */
	buf = nvfuse_malloc(4096);
	if (buf == NULL) {
		printf(" Error: malloc() \n");
		goto RET;
	}

	memset(buf, 0x00, 4096);
	sprintf(buf, "Hello World!\n");

	/* write 4KB */
	printf(" Write Buf: %s", buf);
	for (count = 0; count < 1024; count++) {
		ret = nvfuse_writefile(nvh, fd, buf, 4096, 0);
		if (ret != 4096) {
			printf(" Error: file write() \n");
		}
	}
		
	nvfuse_lseek(nvh, fd, 0, SEEK_SET);

	memset(buf, 0x00, 4096);

	/* read 4KB */
	for (count = 0; count < 1024; count++) {
		ret = nvfuse_readfile(nvh, fd, buf, 4096, 0);
		if (ret != 4096) {
			printf(" Error: file write() \n");
		}
	}
	printf(" Read Buf: %s", buf);

	/* release memory */
	nvfuse_free(buf);
		
	/* close file */
	nvfuse_closefile(nvh, fd);

RET:;

	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);
}