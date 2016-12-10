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

#ifndef _NVFUSE_MEMALLOC_H_
#define _NVFUSE_MEMALLOC_H_
void *nvfuse_malloc(size_t size);
void nvfuse_free(void *ptr);

void *nvfuse_alloc_aligned_buffer(size_t size);
void nvfuse_free_aligned_buffer(void *ptr);
#endif
