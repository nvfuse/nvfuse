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
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nvfuse_types.h"

static u64 memalloc_allocated_size = 0;
#if NVFUSE_OS == NVFUSE_OS_LINUX
#include <unistd.h> /* getpagesize() */

#ifdef SPDK_ENABLED
#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_malloc.h>

#define USE_RTE_MEMALLOC
#endif

/* allocate a alignment-bytes aligned buffer */
void *nvfuse_alloc_aligned_buffer(size_t size)
{
	void *p;
#ifndef USE_RTE_MEMALLOC
	s32 ret;
	ret = posix_memalign(&p, getpagesize(), size);
	if (!p || ret) {
		perror("memalign");
		return NULL;
	}
#else
	p = rte_malloc(NULL, size, 0x200);
	if (p == NULL) {
		fprintf(stderr, "rte_malloc failed\n");
		rte_malloc_dump_stats(stdout, NULL);
		return NULL;
	}
#endif
	memalloc_allocated_size++;
	return p;
}
#else
void *nvfuse_alloc_aligned_buffer(size_t size)
{
	memalloc_allocated_size++;
	return malloc((size_t)size);
}
#endif 

void *nvfuse_malloc(size_t size) {
	memalloc_allocated_size++;
	return malloc((size_t)size);
}


void nvfuse_free(void *ptr){
	memalloc_allocated_size--;
	free(ptr);
}

#ifndef USE_RTE_MEMALLOC
void nvfuse_free_aligned_buffer(void *ptr){
	memalloc_allocated_size--;
	free(ptr);
}
#else
void nvfuse_free_aligned_buffer(void *ptr){
	memalloc_allocated_size--;
	rte_free(ptr);
}
#endif

