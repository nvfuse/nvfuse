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
#include "nvfuse_config.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"
#include "nvfuse_aio.h"

#define DEINIT_IOM	1
#define UMOUNT		1

int main(int argc, char *argv[])
{
	struct nvfuse_io_manager io_manager;
	struct nvfuse_ipc_context ipc_ctx;
	struct nvfuse_params params;
	struct nvfuse_handle *nvh;
	int ret;
	int fd;
	int count;
	char *buf;

	ret = nvfuse_parse_args(argc, argv, &params);
	if (ret < 0)
		return -1;

	ret = nvfuse_configure_spdk(&io_manager, &ipc_ctx, params.cpu_core_mask, NVFUSE_MAX_AIO_DEPTH);
	if (ret < 0)
		return -1;

	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(&io_manager, &ipc_ctx, &params);
	if (nvh == NULL) {
		fprintf(stderr, "Error: nvfuse_create_handle()\n");
		return -1;
	}

	/* file open and create */
	fd = nvfuse_openfile_path(nvh, "helloworld.file", O_RDWR | O_CREAT, 0);
	if (fd == -1) {
		printf(" Error: open() \n");
		goto RET;
	}

	/* 4KB memory allocation */
	buf = nvfuse_alloc_aligned_buffer(4096);
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
	nvfuse_free_aligned_buffer(buf);

	/* close file */
	nvfuse_closefile(nvh, fd);

RET:
	;

	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);
	nvfuse_deinit_spdk(&io_manager, &ipc_ctx);
	return 0;
}
