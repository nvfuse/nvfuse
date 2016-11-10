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

extern struct nvfuse_io_manager *nvfuse_io_manager; 

int main()
{
	struct nvfuse_io_manager io_manager;
	int ret;
	int fd;
	int count;
	char *buf;

#if EXAM_USE_RAMDISK == 1 
	nvfuse_init_memio(&io_manager, "RANDISK", "RAM");
#elif EXAM_USE_FILEDISK == 1
	printf(" Open File = %s\n", DISK_FILE_PATH);
	nvfuse_init_fileio(&io_manager, "FILE", DISK_FILE_PATH);
#elif EXAM_USE_UNIXIO == 1
	nvfuse_init_unixio(&io_manager, "SSD", "/dev/sdb", AIO_MAX_QDEPTH);
#elif EXAM_USE_SPDK == 1
	nvfuse_init_spdk(&io_manager, "SPDK", "spdk:namespace0", AIO_MAX_QDEPTH);
#endif 
	
	/* IMPORTANT: don't msiss set global variable */
	nvfuse_io_manager = &io_manager;

	/* open I/O manager */
	io_manager.io_open(&io_manager, 0);

	printf(" hello world \n");

	printf(" total blks = %ld \n", (long)nvfuse_io_manager->total_blkcount);
	printf(" nvfuse_io_manager = %p\n", nvfuse_io_manager);

	/* file system format */
	ret = nvfuse_format();
	if (ret < 0) {
		printf(" Error: format() \n");
	}

	/* file system mount */
	ret = nvfuse_mount();
	if (ret < 0) {
		printf(" Error: mount() \n");
	}
	
	/* file open and create */
	fd = nvfuse_openfile_path("helloworld.file", O_RDWR | O_CREAT, 0);
	if (fd == -1) {
		printf(" Error: open() \n");
		goto UMOUNT;
	}

	/* 4KB memory allocation */
	buf = nvfuse_malloc(4096);
	if (buf == NULL) {
		printf(" Error: malloc() \n");
		goto UMOUNT;
	}

	memset(buf, 0x00, 4096);
	sprintf(buf, "Hello World!\n");

	/* write 4KB */
	printf(" Write Buf: %s", buf);
	for (count = 0; count < 1024; count++) {
		ret = nvfuse_writefile(fd, buf, 4096, 0);
		if (ret != 4096) {
			printf(" Error: file write() \n");
		}
	}
		
	nvfuse_lseek(fd, 0, SEEK_SET);

	memset(buf, 0x00, 4096);

	/* read 4KB */
	for (count = 0; count < 1024; count++) {
		ret = nvfuse_readfile(fd, buf, 4096, 0);
		if (ret != 4096) {
			printf(" Error: file write() \n");
		}
	}
	printf(" Read Buf: %s", buf);

	/* release memory */
	nvfuse_free(buf);
		
	/* close file */
	nvfuse_closefile(fd);

UMOUNT:;
	/* umount file system */
	ret = nvfuse_umount();
	if (ret < 0) {
		printf(" Error: umount() \n");
	}

	/* close I/O manager */
	io_manager.io_close(&io_manager);
}
