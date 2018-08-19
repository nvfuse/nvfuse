/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2017 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 26/06/2017
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


#ifndef _NVFUSE_FLUSHWORK_H
#define _NVFUSE_FLUSHWORK_H

/* flush worker status */
#define FLUSHWORKER_PENDING 1
#define FLUSHWORKER_RUNNING 2
#define FLUSHWORKER_STOP	3

//s32 nvfuse_flushworker(void *arg);
void * nvfuse_flushworker(void *arg);
s32 nvfuse_start_flushworker(struct nvfuse_superblock *sb);
s32 nvfuse_stop_flushworker();
void nvfuse_queuework();
void nvfuse_set_flushworker_status(s32 status);
s32 nvfuse_get_flushworker_status();

#endif
