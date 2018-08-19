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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
//#define NDEBUG
#include <assert.h>

#ifdef __linux__
#include <sys/uio.h>
#endif

#include "spdk/env.h"

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>
#include <rte_log.h>
#include <rte_mempool.h>

#include "nvfuse_dep.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_inode_cache.h"
#include "nvfuse_core.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_indirect.h"
#include "nvfuse_bp_tree.h"
#include "nvfuse_config.h"
#include "nvfuse_malloc.h"
#include "nvfuse_api.h"
#include "nvfuse_dirhash.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_dirhash.h"
#include "nvfuse_debug.h"
#include "nvfuse_flushwork.h"

#define USE_PTHREAD
#ifdef USE_PTHREAD
static pthread_t flush_worker_id;
#else
static unsigned flush_worker_id = 1;
#endif

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int flushworker_status = FLUSHWORKER_STOP;
static pthread_spinlock_t lock;

void nvfuse_queuework() 
{
	pthread_mutex_lock(&mutex);
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);
}

void nvfuse_set_flushworker_status(s32 status)
{
	pthread_spin_lock(&lock);
	flushworker_status = status;
	pthread_spin_unlock(&lock);
}

s32 nvfuse_get_flushworker_status()
{
	s32 status;

	pthread_spin_lock(&lock);
	status = flushworker_status;
	pthread_spin_unlock(&lock);

	return status;
}

#ifndef USE_PTHREAD
s32 nvfuse_flushworker(void *arg)
#else
void *nvfuse_flushworker(void *arg)
#endif
{
	struct nvfuse_superblock *sb = (struct nvfuse_superblock *)arg;

	nvfuse_set_flushworker_status(FLUSHWORKER_PENDING);

	while (flushworker_status != FLUSHWORKER_STOP) {
		dprintf_info(FLUSHWORK, "flush worker is wait lcore = %d.\n", rte_lcore_id());
		fflush(stdout);

		pthread_mutex_lock(&mutex);
		pthread_cond_wait(&cond, &mutex);
		pthread_mutex_unlock(&mutex);

		dprintf_info(FLUSHWORK, "flush worker wakes up lcore = %d.\n", rte_lcore_id());
		fflush(stdout);

		nvfuse_flush_dirty_data(sb);
	}

	return 0;
}

s32 nvfuse_start_flushworker(struct nvfuse_superblock *sb)
{
#ifndef USE_PTHREAD
	s32 ret;
	unsigned lcore_id;
#endif

	pthread_spin_init(&lock, 0);

	dprintf_info(FLUSHWORK, " start flush worker \n");
#ifndef USE_PTHREAD
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		break;
	}

	assert(lcore_id == flush_worker_id);

	flush_worker_id = lcore_id;
	dprintf_info(FLUSHWORK, " launch secondary lcore = %d \n", flush_worker_id);
	fflush(stdout);
	ret = rte_eal_remote_launch(nvfuse_flushworker, sb, flush_worker_id);
	if (ret < 0) {
		dprintf_error(FLUSHWORK, " thread cannot be lauched\n");
		assert(0);
	}
#else
	pthread_create(&flush_worker_id, NULL, nvfuse_flushworker, (void *)sb);
#endif

	return 0;
}

s32 nvfuse_stop_flushworker() 
{
	dprintf_info(FLUSHWORK, " stop flush worker \n");

	nvfuse_set_flushworker_status(FLUSHWORKER_STOP);

#ifndef USE_PTHREAD
	printf(" wait lcore = %d \n", flush_worker_id);
	if (rte_eal_wait_lcore(flush_worker_id) < 0) {
		return -1;
	}
#else
	pthread_join(flush_worker_id, NULL);
#endif

	return 0;
}
