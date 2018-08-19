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
#include "spdk/env.h"
#include "nvfuse_core.h"
#include "nvfuse_config.h"
#include "nvfuse_api.h"
#include "nvfuse_malloc.h"
#include "nvfuse_aio.h"
#include "nvfuse_debug.h"

#define DEINIT_IOM	1
#define UMOUNT		1

static struct nvfuse_ipc_context ipc_ctx;
static struct nvfuse_params params;
static struct nvfuse_handle *nvh;

static void helloworld_run(void *arg1, void *arg2)
{
	int fd;
	int count;
	char *buf;
	int ret;

	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(&ipc_ctx, &params);
	if (nvh == NULL) {
		fprintf(stderr, "Error: nvfuse_create_handle()\n");
		return;
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

	spdk_app_stop(0);
}

static void reactor_run(void *arg1, void *arg2)
{
	struct spdk_event *event;
	u32 i;

	/* Send events to start all I/O */
	SPDK_ENV_FOREACH_CORE(i) {
		printf(" allocate event on lcore = %d \n", i);
		if (i == 1) {
			event = spdk_event_allocate(i, helloworld_run,
						    NULL, NULL);
			spdk_event_call(event);
		}
	}
}

int main(int argc, char *argv[])
{
	s32 ret;

	ret = nvfuse_parse_args(argc, argv, &params);
	if (ret < 0)
		return -1;

	ret = nvfuse_configure_spdk(&ipc_ctx, &params, NVFUSE_MAX_AIO_DEPTH);
	if (ret < 0)
		return -1;

#ifndef NVFUSE_USE_CEPH_SPDK
	spdk_app_start(&params.opts, reactor_run, NULL, NULL);
#else
	spdk_app_start(reactor_run, NULL, NULL);
#endif

	spdk_app_fini();

	return 0;
}
