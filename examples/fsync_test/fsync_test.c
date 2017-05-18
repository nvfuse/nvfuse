/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2017 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 13/02/2017
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
#include <assert.h>

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_aio.h"
#include "spdk/env.h"
#include <rte_lcore.h>

#define DEINIT_IOM	1
#define UMOUNT		1

#define MB (1024*1024)
#define GB (1024*1024*1024)
#define TB ((s64)1024*1024*1024*1024)

/* global io_manager */
static struct nvfuse_io_manager _g_io_manager;
static struct nvfuse_io_manager *g_io_manager = &_g_io_manager;
/* global ipc_context */
static struct nvfuse_ipc_context _g_ipc_ctx;
static struct nvfuse_ipc_context *g_ipc_ctx = &_g_ipc_ctx;
/* global params */
static struct nvfuse_params _g_params;
static struct nvfuse_params *g_params = &_g_params;

static s32 last_percent;

static s64 g_file_size = (s64)4 * GB;
static s32 g_block_size = 4 * KB;
static s32 g_fsync_period = 1;

void ft_progress_reset()
{
    last_percent = 0;
}

void ft_progress_report(s32 curr, s32 max)
{
    int curr_percent;

    /* FIXME: */
	if (rte_lcore_id() == 1) 
	{
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
}

int ft_create_max_sized_file(struct nvfuse_handle *nvh, u32 arg)
{
	struct statvfs statvfs_buf;
	struct stat stat_buf;
	char str[128];
	struct timeval tv;	
	s64 file_allocated_size;
	s32 res;
	s32 fid;

	s64 file_size = g_file_size;
	s32 block_size = g_block_size;
	s32 fsync_period = g_fsync_period;

	s64 io_size = 0;
	s8 *user_buffer;

	if (nvfuse_statvfs(nvh, NULL, &statvfs_buf) < 0)
	{
		printf(" statfs error \n");
		return -1;
	}

	sprintf(str, "fsync_test.dat");

	fid = nvfuse_openfile_path(nvh, str, O_RDWR | O_CREAT, 0);
	if (fid < 0)
	{
		printf(" Error: file open or create \n");
		return -1;
	}

	gettimeofday(&tv, NULL);
	printf("\n Start: Fallocate (file %s size %luMB). \n", str, (long)file_size/MB);
	/* pre-allocation of data blocks*/
	nvfuse_fallocate(nvh, str, 0, file_size);
	
	/* getattr() */
	res = nvfuse_getattr(nvh, str, &stat_buf);
	if (res) 
	{
		printf(" No such file %s\n", str);
		return -1;
	}

	/* NOTE: Allocated size may differ from requested size. */
	file_allocated_size = stat_buf.st_size;

	/* allocation of user buffer for request */
	user_buffer = nvfuse_alloc_aligned_buffer(block_size);
	if (user_buffer == NULL)
	{
		fprintf( stderr, " Error: malloc()\n");
		return -1;
	}

	printf(" file size = %lu \n", file_allocated_size);
	printf(" block size = %d \n", block_size);

	gettimeofday(&tv, NULL);
	while (io_size < file_allocated_size)
	{
		s64 offset = (s64)((u64)nvfuse_rand() % (file_allocated_size / block_size)) * block_size;

		//printf("offset = %lu, %.3f\n", offset, offset * block_size);

		res = nvfuse_writefile(nvh, fid, user_buffer, block_size, offset);
		if (res != block_size)
		{
			fprintf(stderr, " Error: write file()\n");
			goto RET;
		}
				
		if (--fsync_period == 0) 
		{
			nvfuse_fsync(nvh, fid);
			fsync_period == g_fsync_period;
		}
		
		io_size += block_size;

		ft_progress_report((s32)(io_size/block_size), (s32)(file_allocated_size/block_size));
	}
	printf(" write with fsync throughput %.3fMB/s\n", (double)file_allocated_size/MB/time_since_now(&tv));

	nvfuse_closefile(nvh, fid);

	gettimeofday(&tv, NULL);
	printf(" Start: rmfile %s size %luMB \n", str, (long)file_allocated_size/MB);
	res = nvfuse_rmfile_path(nvh, str);
	if (res < 0)
	{
		printf(" Error: rmfile = %s\n", str);
		return -1;
	}
	printf(" nvfuse rmfile throughput %.3fMB/s\n", (double)file_allocated_size/MB/time_since_now(&tv));

RET:
	nvfuse_free_aligned_buffer(user_buffer);

	printf("\n Finish: Fallocate and Deallocate.\n");

	return NVFUSE_SUCCESS;
}

#define RANDOM		1
#define SEQUENTIAL	0


void ft_usage(char *cmd)
{
	printf("\nOptions for NVFUSE application: \n");
	printf("\t-F: file size (in MB)\n");
	printf("\t-B: block size (in KB)\n");
	printf("\t-S: fsync period\n");
}

static int ft_main(void *arg)
{
	struct nvfuse_handle *nvh;	
	struct rte_ring *stat_rx_ring;
	struct ret_mempool *stat_message_pool;
	union perf_stat perf_stat;	
	union perf_stat _perf_stat_rusage;
	struct perf_stat_rusage *rusage_stat = &_perf_stat_rusage;
	struct timeval tv;
	double execution_time;
	s32 ret;

	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(g_io_manager, g_ipc_ctx, g_params);
	if (nvh == NULL)
	{
		fprintf(stderr, "Error: nvfuse_create_handle()\n");
		return -1;
	}

	/* stat ring lookup */
	ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RT_STAT);
	if (ret < 0) 
		return -1;

	printf("\n");
	/* rusage */
	getrusage(RUSAGE_THREAD, &rusage_stat->start);
	
	gettimeofday(&tv, NULL);
	
	/* main execution */
	ret = ft_create_max_sized_file(nvh, 0);
	
	execution_time = time_since_now(&tv);
	
	memset(&perf_stat, 0x00, sizeof(union perf_stat));

	perf_stat.stat_rt.stat_type = RT_STAT;
	perf_stat.stat_rt.lcore_id = (s32)arg;
	perf_stat.stat_rt.sequence = 0;
	perf_stat.stat_rt.total_time = execution_time;
	
	nvfuse_stat_ring_put(stat_rx_ring, stat_message_pool, &perf_stat);		

	/* rusage */
	getrusage(RUSAGE_THREAD, &rusage_stat->end);
	nvfuse_rusage_diff(&rusage_stat->start, &rusage_stat->end, &rusage_stat->result);
	print_rusage(&rusage_stat->result, "test", 1, execution_time);
	//rusage_stat->tag = 0xDEADDEAD;
	
	/* stat ring lookup */
	ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RUSAGE_STAT);
	if (ret < 0) 
		return -1;

	nvfuse_stat_ring_put(stat_rx_ring, stat_message_pool, rusage_stat);

	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);
RET:
	return 0;
}


static void print_stats(s32 num_cores, s32 num_tc)
{
	struct rte_ring *stat_rx_ring;
	struct ret_mempool *stat_message_pool;
	union perf_stat *per_core_stat, *cur_stat, sum_stat, temp_stat;		
	s32 ret;
	s32 cur;
	s32 tc;
	s32 i;
	s8 name[128];
	double group_exec_time = 0.0;
	
	per_core_stat = malloc(sizeof(union perf_stat) * num_cores * num_tc);
	if (per_core_stat == NULL) {
		fprintf(stderr, " Error: malloc() \n");
	}
	
	/* stat ring lookup */
	ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RT_STAT);
	if (ret < 0) 
		return -1;

	memset(per_core_stat, 0x00, sizeof(union perf_stat) * num_cores * num_tc);
	memset(&sum_stat, 0x00, sizeof(union perf_stat));

	/* gather rt stats */
	for (i = 0;i < num_cores * num_tc; i++) {
		ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
		if (ret < 0)
			return -1;
		
		assert(temp_stat.stat_rt.sequence * num_cores + temp_stat.stat_rt.lcore_id < num_cores * num_tc);

		cur_stat = per_core_stat + (temp_stat.stat_rt.sequence * num_cores + temp_stat.stat_rt.lcore_id);
		memcpy(cur_stat, &temp_stat, sizeof(union perf_stat));
	}

	for (tc = 0; tc < num_tc; tc++)
	{
		double tc_total = 0.0;
		//printf(" TC %d %s\n", tc, rt_ctx[tc].test_name);
		for (cur = 0;cur < num_cores; cur++)	
		{
			cur_stat = per_core_stat + tc * num_cores + cur;
			tc_total += cur_stat->stat_rt.total_time;
			group_exec_time += cur_stat->stat_rt.total_time;
			printf(" Per core %d execution = %.6f\n", cur, cur_stat->stat_rt.total_time);			
		}
		//printf(" Avg execution = %.6f sec \n", tc, rt_ctx[tc].test_name, tc_total / num_cores);
		printf("\n");
	}
	
	group_exec_time /= num_cores;

	printf("Summary: Avg execution = %.6f sec\n", group_exec_time);
	printf("Summary: Avg bandwidth = %.3f MB/s\n", (double)g_file_size * __builtin_popcount((u32)g_params->cpu_core_mask) / MB / group_exec_time);
	
	free(per_core_stat);

	/* Device Level Stat */
	{
		union perf_stat _sum_stat;
		struct perf_stat_dev *sum_stat = &_sum_stat;
		struct perf_stat_dev *cur_stat;

		memset(sum_stat, 0x00, sizeof(struct perf_stat_dev));

		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, DEVICE_STAT);
		if (ret < 0) 
			return -1;

		/* gather dev stats */
		for (i = 0;i < num_cores; i++) {
			ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
			if (ret < 0)
				return -1;
						
			cur_stat = (struct perf_stat_dev *)&temp_stat;
			
			sum_stat->total_io_count += cur_stat->total_io_count;
			sum_stat->read_io_count += cur_stat->read_io_count;
			sum_stat->write_io_count += cur_stat->write_io_count;
		}

		printf(" Device Total I/O bandwidth = %.3f MB/s\n", (double)sum_stat->total_io_count * CLUSTER_SIZE / MB /group_exec_time);
		printf(" Device Read I/O bandwidth = %.3f MB/s\n", (double)sum_stat->read_io_count * CLUSTER_SIZE / MB /group_exec_time);
		printf(" Device Write I/O bandwidth = %.3f MB/s\n", (double)sum_stat->write_io_count * CLUSTER_SIZE / MB /group_exec_time);
		
		printf(" Device Total I/O Amount = %.3f MB\n", (double)sum_stat->total_io_count * CLUSTER_SIZE / MB);
		printf(" Device Read I/O Amount = %.3f MB\n", (double)sum_stat->read_io_count * CLUSTER_SIZE / MB);
		printf(" Device Write I/O Amount = %.3f MB\n", (double)sum_stat->write_io_count * CLUSTER_SIZE / MB);
	}

	/* IPC Stat */
	if (nvfuse_process_model_is_dataplane()) {
		union perf_stat _sum_stat;
		struct perf_stat_ipc *sum_stat = &_sum_stat;
		struct perf_stat_ipc *cur_stat;
		s32 type;

		memset(sum_stat, 0x00, sizeof(struct perf_stat_ipc));

		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, IPC_STAT);
		if (ret < 0) 
			return -1;

		/* gather dev stats */
		for (i = 0;i < num_cores; i++) {
			ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
			if (ret < 0)
				return -1;
						
			cur_stat = (struct perf_stat_ipc *)&temp_stat;
			for (type = APP_REGISTER_REQ; type < HEALTH_CHECK_CPL; type++)
			{
				sum_stat->total_tsc[type] += cur_stat->total_tsc[type];
				sum_stat->total_count[type] += cur_stat->total_count[type];
			}			
			
			printf(" Core %d Container Alloc Latency = %f us\n", i, (double)cur_stat->total_tsc[CONTAINER_ALLOC_REQ]/cur_stat->total_count[CONTAINER_ALLOC_REQ]/spdk_get_ticks_hz()*1000000);
			printf(" Core %d Container Free Latency = %f us\n", i, (double)cur_stat->total_tsc[CONTAINER_RELEASE_REQ]/cur_stat->total_count[CONTAINER_RELEASE_REQ]/spdk_get_ticks_hz()*1000000);
			printf(" Core %d BUFFER Alloc Latency = %f us\n", i, (double)cur_stat->total_tsc[BUFFER_ALLOC_REQ]/cur_stat->total_count[BUFFER_ALLOC_REQ]/spdk_get_ticks_hz()*1000000);
			printf(" Core %d BUFFER Free Latency = %f us\n", i, (double)cur_stat->total_tsc[BUFFER_FREE_REQ]/cur_stat->total_count[BUFFER_FREE_REQ]/spdk_get_ticks_hz()*1000000);
		}

		printf(" Avg Container Alloc Latency = %f us\n", (double)sum_stat->total_tsc[CONTAINER_ALLOC_REQ]/sum_stat->total_count[CONTAINER_ALLOC_REQ]/spdk_get_ticks_hz()*1000000);
		printf(" Avg Container Free Latency = %f us\n", (double)sum_stat->total_tsc[CONTAINER_RELEASE_REQ]/sum_stat->total_count[CONTAINER_RELEASE_REQ]/spdk_get_ticks_hz()*1000000);
		printf(" Avg BUFFER Alloc Latency = %f us\n", (double)sum_stat->total_tsc[BUFFER_ALLOC_REQ]/sum_stat->total_count[BUFFER_ALLOC_REQ]/spdk_get_ticks_hz()*1000000);
		printf(" Avg BUFFER Free Latency = %f us\n", (double)sum_stat->total_tsc[BUFFER_FREE_REQ]/sum_stat->total_count[BUFFER_FREE_REQ]/spdk_get_ticks_hz()*1000000);
	}

	printf("\n");
	/* Rusage Stat */
	{
		union perf_stat _sum_stat;
		struct perf_stat_rusage *sum_stat = &_sum_stat;
		struct perf_stat_rusage *cur_stat;
		s32 type;

		memset(sum_stat, 0x00, sizeof(struct perf_stat_rusage));

		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RUSAGE_STAT);
		if (ret < 0) 
			return -1;

		/* gather rusage stats */
		for (i = 0;i < num_cores; i++) {
			ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
			if (ret < 0)
				return -1;
						
			cur_stat = (struct perf_stat_rusage *)&temp_stat;
			
			//(group_exec_time / num_cores);			
			sprintf(name, "core %d", i);
			print_rusage(&cur_stat->result, name, 1, group_exec_time);
			
			nvfuse_rusage_add(&cur_stat->result, &sum_stat->result);
			//printf(" tag = %x\n", cur_stat->tag);
		}

		sprintf(name, "Avg", i);
		print_rusage(&sum_stat->result, name, num_cores, group_exec_time);
	}
}

int main(int argc, char *argv[])
{	
	int core_argc = 0;
	char *core_argv[128];		
	int app_argc = 0;
	char *app_argv[128];
	char op;
	int ret = 0;
	int num_cores = 0;
	int lcore_id;

	/* distinguish cmd line into core args and app args */
	nvfuse_distinguish_core_and_app_options(argc, argv, 
											&core_argc, core_argv, 
											&app_argc, app_argv);
	
	ret = nvfuse_parse_args(core_argc, core_argv, g_params);
	if (ret < 0) {
		goto INVALID_ARGS;
	}
		
	
	ret = nvfuse_configure_spdk(g_io_manager, g_ipc_ctx, g_params->cpu_core_mask, NVFUSE_MAX_AIO_DEPTH);
	if (ret < 0)
		return -1;

	/* optind must be reset before using getopt() */
	optind = 0;
	while ((op = getopt(app_argc, app_argv, "F:B:S:")) != -1) {
		switch (op) {			
		case 'F':
			g_file_size = atoi(optarg);
			g_file_size = (s64)g_file_size * MB;
			if (g_file_size == 0)
				goto INVALID_ARGS;

			break;
		case 'B':
			g_block_size = atoi(optarg);
			g_block_size = g_block_size * KB;
			if (g_block_size == 0)
				goto INVALID_ARGS;
			break;

		case 'S':
			g_fsync_period = atoi(optarg);
			if (g_fsync_period == 0)
				goto INVALID_ARGS;
			break;

		default:
			goto INVALID_ARGS;
		}
	}
	
	printf(" file size = %.3fGB\n", (double)g_file_size / GB);
	printf(" block size = %.3fKB\n", (double)g_block_size / KB);
	printf(" fsync period = %d \n", g_fsync_period);

	/* call lcore_recv() on every slave lcore */
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {		
		printf(" launch secondary lcore = %d \n", lcore_id);
		rte_eal_remote_launch(ft_main, (void *)num_cores, lcore_id);
		num_cores++;
	}
	
	printf(" launch primary lcore = %d \n", rte_lcore_id());
	
	ret = ft_main((void *)num_cores);
	if (ret < 0)
		return -1;

	num_cores++;

	//rte_eal_mp_wait_lcore();
	RTE_LCORE_FOREACH(lcore_id) {
		printf(" wait lcore = %d \n", lcore_id);
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
		}
	}
	
	print_stats(num_cores, 1);

	nvfuse_deinit_spdk(g_io_manager, g_ipc_ctx);

	return ret;

INVALID_ARGS:;
	nvfuse_core_usage(argv[0]);
	ft_usage(argv[0]);
	nvfuse_core_usage_example(argv[0]);
	return -1;
}
