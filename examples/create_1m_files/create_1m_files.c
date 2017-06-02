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
#include <assert.h>

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_aio.h"
#include "nvfuse_misc.h"
#include "spdk/env.h"
#include <rte_lcore.h>

#define DEINIT_IOM	1
#define UMOUNT		1

#define NUM_ELEMENTS(x) (sizeof(x)/sizeof(x[0]))

#define RT_TEST_TYPE MILL_TEST

#define MAX_TEST    1
/* quick test */
#define QUICK_TEST  2
/* 1 million create/delete test */
#define MILL_TEST   3

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
static s32 test_type = MILL_TEST;

/* Function Declaration */
void rt_progress_reset(void);
void rt_progress_report(s32 curr, s32 max);
char *rt_decode_test_type(s32 type);
int rt_create_files(struct nvfuse_handle *nvh, u32 arg);
int rt_stat_files(struct nvfuse_handle *nvh, u32 arg);
int rt_rm_files(struct nvfuse_handle *nvh, u32 arg);
void rt_usage(char *cmd);
static int rt_main(void *arg);
static void print_stats(s32 num_cores, s32 num_tc);


void rt_progress_reset(void)
{
	last_percent = 0;
}

void rt_progress_report(s32 curr, s32 max)
{
	int curr_percent;

	/* FIXME: */
	if (rte_lcore_id() == 1) {
		curr_percent = (curr + 1) * 100 / max;

		if	(curr_percent != last_percent) {
			last_percent = curr_percent;
			printf(".");
			if (curr_percent % 10 == 0)
				printf("%d%%\n", curr_percent);
			fflush(stdout);
		}
	}
}

char *rt_decode_test_type(s32 type)
{
	switch (type) {
	case MAX_TEST:
		return "MAX_TEST";
	case QUICK_TEST:
		return "QUICK_TEST";
	case MILL_TEST:
		return "MILL_TEST";
	}

	return NULL;
}

int rt_create_files(struct nvfuse_handle *nvh, u32 arg)
{
	struct timeval tv;
	struct statvfs stat;
	s8 buf[FNAME_SIZE];
	s32 max_inodes;
	s32 i;
	s32 fd;

	if (nvfuse_statvfs(nvh, NULL, &stat) < 0) {
		printf(" statfs error \n");
		return -1;
	}

	switch (test_type) {
	case MAX_TEST:
		max_inodes = stat.f_ffree; /* # of free inodes */
		break;
	case QUICK_TEST:
		max_inodes = 100;
		break;
	case MILL_TEST:
		/* # of free inodes */
		max_inodes = stat.f_ffree < 1000000 ? stat.f_ffree : 1000000;
		break;
	default:
		printf(" Invalid test type = %d\n", test_type);
		return -1;
	}

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	nvh->nvh_sb.bp_set_index_tsc = 0;
	nvh->nvh_sb.bp_set_index_count = 0;
	nvh->nvh_sb.nvme_io_tsc = 0;
	nvh->nvh_sb.nvme_io_count = 0;

	/* create null files */
	printf(" Start: creating null files (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++) {
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
	nvfuse_check_flush_dirty(&nvh->nvh_sb, 1);

	printf(" Finish: creating null files (0x%x) %.3f OPS (%.f sec).\n", max_inodes,
	       max_inodes / time_since_now(&tv), time_since_now(&tv));
	printf(" bp tree cpu = %f sec\n",
	       (double)nvh->nvh_sb.bp_set_index_tsc / (double)spdk_get_ticks_hz());
	printf(" sync meta i/o = %f sec\n", (double)nvh->nvh_sb.nvme_io_tsc / (double)spdk_get_ticks_hz());

	return 0;
}

int rt_stat_files(struct nvfuse_handle *nvh, u32 arg)
{
	struct timeval tv;
	struct statvfs stat;
	s8 buf[FNAME_SIZE];
	s32 max_inodes;
	s32 i;

	if (nvfuse_statvfs(nvh, NULL, &stat) < 0) {
		printf(" statfs error \n");
		return -1;
	}

	switch (test_type) {
	case MAX_TEST:
		max_inodes = stat.f_ffree; /* # of free inodes */
		break;
	case QUICK_TEST:
		max_inodes = 100;
		break;
	case MILL_TEST:
		/* # of free inodes */
		max_inodes = stat.f_ffree < 1000000 ? stat.f_ffree : 1000000;
		break;
	default:
		printf(" Invalid test type = %d\n", test_type);
		return -1;
	}

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* lookup null files */
	printf(" Start: looking up null files (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++) {
		struct stat st_buf;
		int res;

		sprintf(buf, "file%d\n", i);

		res = nvfuse_getattr(nvh, buf, &st_buf);
		if (res) {
			printf(" No such file %s\n", buf);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: looking up null files (0x%x) %.3f OPS (%.f sec).\n", max_inodes,
	       max_inodes / time_since_now(&tv), time_since_now(&tv));


	return 0;
}


int rt_rm_files(struct nvfuse_handle *nvh, u32 arg)
{
	struct timeval tv;
	struct statvfs stat;
	s8 buf[FNAME_SIZE];
	s32 max_inodes;
	s32 i;

	if (nvfuse_statvfs(nvh, NULL, &stat) < 0) {
		printf(" statfs error \n");
		return -1;
	}

	switch (test_type) {
	case MAX_TEST:
		max_inodes = stat.f_ffree; /* # of free inodes */
		break;
	case QUICK_TEST:
		max_inodes = 100;
		break;
	case MILL_TEST:
		/* # of free inodes */
		max_inodes = stat.f_ffree < 1000000 ? stat.f_ffree : 1000000;
		break;
	default:
		printf(" Invalid test type = %d\n", test_type);
		return -1;
	}

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* delete null files */
	printf(" Start: deleting null files (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++) {
		sprintf(buf, "file%d\n", i);

		if (nvfuse_rmfile_path(nvh, buf)) {
			printf(" rmfile = %s error \n", buf);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: deleting null files (0x%x) %.3f OPS (%.f sec).\n", max_inodes,
	       max_inodes / time_since_now(&tv), time_since_now(&tv));

	nvfuse_check_flush_dirty(&nvh->nvh_sb, 1);

	return 0;
}

#define RANDOM		1
#define SEQUENTIAL	0

struct regression_test_ctx {
	s32(*function)(struct nvfuse_handle *nvh, u32 arg);
	s8 test_name[128];
	s32 arg;
	s32 pass_criteria; /* compare return code */
	s32 pass_criteria_ignore; /* no compare */
}
rt_ctx[] = {
	{ rt_create_files, "Creating Max Number of Files.", 0, 0, 0},
	{ rt_stat_files, "Looking up Max Number of Files.", 0, 0, 0},
	{ rt_rm_files, "Deleting Max Number of Files.", 0, 0, 0},
};

void rt_usage(char *cmd)
{
	printf("\nOptions for NVFUSE application: \n");
	printf("\t-T: test type (e.g., 1: max_test, 2: quick_test, 3: million test \n");
}

static int rt_main(void *arg)
{
	struct nvfuse_handle *nvh;
	struct regression_test_ctx *cur_rt_ctx;
	struct rte_ring *stat_rx_ring;
	struct rte_mempool *stat_message_pool;
	union perf_stat perf_stat;
	union perf_stat _perf_stat_rusage;
	struct perf_stat_rusage *rusage_stat = (struct perf_stat_rusage *)&_perf_stat_rusage;
	struct timeval tv;
	double execution_time;
	s32 ret;

	printf(" Perform test %s thread id = %d... \n", rt_decode_test_type(test_type), (s32)arg);

	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(g_io_manager, g_ipc_ctx, g_params);
	if (nvh == NULL) {
		fprintf(stderr, "Error: nvfuse_create_handle()\n");
		return -1;
	}

	/* stat ring lookup */
	ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RT_STAT);
	if (ret < 0)
		return -1;

	printf("\n");

	cur_rt_ctx = rt_ctx;

	getrusage(RUSAGE_THREAD, &rusage_stat->start);
	/* Test Case Handler with Regression Test Context Array */
	while (cur_rt_ctx < rt_ctx + NUM_ELEMENTS(rt_ctx)) {
		s32 index = cur_rt_ctx - rt_ctx + 1;

		printf(" lcore = %d Regression Test %d: %s\n", rte_lcore_id(), index, cur_rt_ctx->test_name);
		gettimeofday(&tv, NULL);
		ret = cur_rt_ctx->function(nvh, cur_rt_ctx->arg);
		if (!cur_rt_ctx->pass_criteria &&
		    ret != cur_rt_ctx->pass_criteria) {
			printf(" Failed Regression Test %d.\n", index);
			goto RET;
		}

		execution_time = time_since_now(&tv);

		memset(&perf_stat, 0x00, sizeof(union perf_stat));

		perf_stat.stat_rt.stat_type = RT_STAT;
		perf_stat.stat_rt.lcore_id = (s32)arg;
		perf_stat.stat_rt.sequence = (index - 1);
		perf_stat.stat_rt.total_time = execution_time;

		printf(" rt stat sequence = %d\n", index - 1);
		nvfuse_stat_ring_put(stat_rx_ring, stat_message_pool, &perf_stat);

		printf(" lcore = %d Regression Test %d: passed successfully.\n\n", rte_lcore_id(), index);
		cur_rt_ctx++;
	}
	/* rusage */
	getrusage(RUSAGE_THREAD, &rusage_stat->end);
	nvfuse_rusage_diff(&rusage_stat->start, &rusage_stat->end, &rusage_stat->result);
	print_rusage(&rusage_stat->result, "test", 1, execution_time);
	//rusage_stat->tag = 0xDEADDEAD;

	{
		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RUSAGE_STAT);
		if (ret < 0)
			return -1;

		nvfuse_stat_ring_put(stat_rx_ring, stat_message_pool, (union perf_stat *)rusage_stat);
	}

	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);
RET:
	return 0;
}


static void print_stats(s32 num_cores, s32 num_tc)
{
	struct rte_ring *stat_rx_ring;
	struct rte_mempool *stat_message_pool;
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
		return;

	memset(per_core_stat, 0x00, sizeof(union perf_stat) * num_cores * num_tc);
	memset(&sum_stat, 0x00, sizeof(union perf_stat));

#if 0
	printf(" perf stat size = %d \n", sizeof(union perf_stat));
	printf(" rt stat size = %d \n", sizeof(struct perf_stat_rt));
	printf(" aio stat size = %d \n", sizeof(struct perf_stat_aio));
	printf(" ipc stat msg = %d \n", PERF_STAT_SIZE);
#endif

	/* gather rt stats */
	for (i = 0; i < num_cores * num_tc; i++) {
		ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
		if (ret < 0)
			return;

		assert(temp_stat.stat_rt.sequence * num_cores + temp_stat.stat_rt.lcore_id < num_cores * num_tc);

#if 0
		printf(" stat type = %d \n", temp_stat.stat_rt.stat_type);
		printf(" seq = %d \n", temp_stat.stat_rt.sequence);
		printf(" core = %d \n", temp_stat.stat_rt.lcore_id);
		printf(" index = %d \n", (temp_stat.stat_rt.sequence * num_cores + temp_stat.stat_rt.lcore_id));
		printf(" \n");
#endif
		cur_stat = per_core_stat + (temp_stat.stat_rt.sequence * num_cores + temp_stat.stat_rt.lcore_id);
		memcpy(cur_stat, &temp_stat, sizeof(union perf_stat));
	}

	for (tc = 0; tc < num_tc; tc++) {
		double tc_total = 0.0;
		printf(" TC %d %s\n", tc, rt_ctx[tc].test_name);
		for (cur = 0; cur < num_cores; cur++) {
			cur_stat = per_core_stat + tc * num_cores + cur;
			tc_total += cur_stat->stat_rt.total_time;
			group_exec_time += cur_stat->stat_rt.total_time;
			printf(" Per core %d execution = %.6f\n", cur, cur_stat->stat_rt.total_time);
		}
		printf(" TC %d Execution %s = %.6f sec \n", tc, rt_ctx[tc].test_name, tc_total / num_cores);
		printf("\n");
	}

	group_exec_time /= num_cores;

	printf("Summary: Avg execution = %.6f sec\n", group_exec_time);

	free(per_core_stat);

	/* Device Level Stat */
	{
		union perf_stat _sum_stat;
		struct perf_stat_dev *sum_stat = (struct perf_stat_dev *)&_sum_stat;
		struct perf_stat_dev *cur_stat;

		memset(sum_stat, 0x00, sizeof(struct perf_stat_dev));

		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, DEVICE_STAT);
		if (ret < 0)
			return;

		/* gather dev stats */
		for (i = 0; i < num_cores; i++) {
			ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
			if (ret < 0)
				return;

			cur_stat = (struct perf_stat_dev *)&temp_stat;

			sum_stat->total_io_count += cur_stat->total_io_count;
			sum_stat->read_io_count += cur_stat->read_io_count;
			sum_stat->write_io_count += cur_stat->write_io_count;
		}

		printf(" Device Total I/O bandwidth = %.3f MB/s\n",
		       (double)sum_stat->total_io_count * CLUSTER_SIZE / MB / group_exec_time);
		printf(" Device Read I/O bandwidth = %.3f MB/s\n",
		       (double)sum_stat->read_io_count * CLUSTER_SIZE / MB / group_exec_time);
		printf(" Device Write I/O bandwidth = %.3f MB/s\n",
		       (double)sum_stat->write_io_count * CLUSTER_SIZE / MB / group_exec_time);

		printf(" Device Total I/O Amount = %.3f MB\n",
		       (double)sum_stat->total_io_count * CLUSTER_SIZE / MB);
		printf(" Device Read I/O Amount = %.3f MB\n", (double)sum_stat->read_io_count * CLUSTER_SIZE / MB);
		printf(" Device Write I/O Amount = %.3f MB\n",
		       (double)sum_stat->write_io_count * CLUSTER_SIZE / MB);
	}

	/* IPC Stat */
	if (nvfuse_process_model_is_dataplane()) {
		union perf_stat _sum_stat;
		struct perf_stat_ipc *sum_stat = (struct perf_stat_ipc *)&_sum_stat;
		struct perf_stat_ipc *cur_stat;
		s32 type;

		memset(sum_stat, 0x00, sizeof(struct perf_stat_ipc));

		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, IPC_STAT);
		if (ret < 0)
			return;

		/* gather dev stats */
		for (i = 0; i < num_cores; i++) {
			ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
			if (ret < 0)
				return;

			cur_stat = (struct perf_stat_ipc *)&temp_stat;
			for (type = APP_REGISTER_REQ; type < HEALTH_CHECK_CPL; type++) {
				sum_stat->total_tsc[type] += cur_stat->total_tsc[type];
				sum_stat->total_count[type] += cur_stat->total_count[type];
			}

			printf(" Core %d Container Alloc Latency = %f us\n", i,
			       (double)cur_stat->total_tsc[CONTAINER_ALLOC_REQ] / cur_stat->total_count[CONTAINER_ALLOC_REQ] /
			       spdk_get_ticks_hz() * 1000000);
			printf(" Core %d Container Free Latency = %f us\n", i,
			       (double)cur_stat->total_tsc[CONTAINER_RELEASE_REQ] / cur_stat->total_count[CONTAINER_RELEASE_REQ] /
			       spdk_get_ticks_hz() * 1000000);
			printf(" Core %d BUFFER Alloc Latency = %f us\n", i,
			       (double)cur_stat->total_tsc[BUFFER_ALLOC_REQ] / cur_stat->total_count[BUFFER_ALLOC_REQ] /
			       spdk_get_ticks_hz() * 1000000);
			printf(" Core %d BUFFER Free Latency = %f us\n", i,
			       (double)cur_stat->total_tsc[BUFFER_FREE_REQ] / cur_stat->total_count[BUFFER_FREE_REQ] /
			       spdk_get_ticks_hz() * 1000000);
		}

		printf(" Avg Container Alloc Latency = %f us\n",
		       (double)sum_stat->total_tsc[CONTAINER_ALLOC_REQ] / sum_stat->total_count[CONTAINER_ALLOC_REQ] /
		       spdk_get_ticks_hz() * 1000000);
		printf(" Avg Container Free Latency = %f us\n",
		       (double)sum_stat->total_tsc[CONTAINER_RELEASE_REQ] / sum_stat->total_count[CONTAINER_RELEASE_REQ] /
		       spdk_get_ticks_hz() * 1000000);
		printf(" Avg BUFFER Alloc Latency = %f us\n",
		       (double)sum_stat->total_tsc[BUFFER_ALLOC_REQ] / sum_stat->total_count[BUFFER_ALLOC_REQ] /
		       spdk_get_ticks_hz() * 1000000);
		printf(" Avg BUFFER Free Latency = %f us\n",
		       (double)sum_stat->total_tsc[BUFFER_FREE_REQ] / sum_stat->total_count[BUFFER_FREE_REQ] /
		       spdk_get_ticks_hz() * 1000000);
	}

	printf("\n");
	/* Rusage Stat */
	{
		union perf_stat _sum_stat;
		struct perf_stat_rusage *sum_stat = (struct perf_stat_rusage *)&_sum_stat;
		struct perf_stat_rusage *cur_stat;

		memset(sum_stat, 0x00, sizeof(struct perf_stat_rusage));

		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RUSAGE_STAT);
		if (ret < 0)
			return;

		/* gather rusage stats */
		for (i = 0; i < num_cores; i++) {
			ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
			if (ret < 0)
				return;

			cur_stat = (struct perf_stat_rusage *)&temp_stat;

			//(group_exec_time / num_cores);
			sprintf(name, "core %d", i);
			print_rusage(&cur_stat->result, name, 1, group_exec_time);

			nvfuse_rusage_add(&cur_stat->result, &sum_stat->result);
			//printf(" tag = %x\n", cur_stat->tag);
		}

		if (num_cores > 1) {
			sprintf(name, "Avg");
			print_rusage(&sum_stat->result, name, num_cores, group_exec_time);
		}
	}
}

int main(int argc, char *argv[])
{
	int core_argc = 0;
	char *core_argv[128];
	int app_argc = 0;
	char *app_argv[128];
#if 0
	char op;
#endif
	int ret = 0;
	int num_cores = 0;
	int lcore_id;

	/* distinguish cmd line into core args and app args */
	nvfuse_distinguish_core_and_app_options(argc, argv,
						&core_argc, core_argv,
						&app_argc, app_argv);

	ret = nvfuse_parse_args(core_argc, core_argv, g_params);
	if (ret < 0)
		return -1;

#if 0
	if (__builtin_popcount((u32)g_params->cpu_core_mask) > 1) {
		printf(" This example is only executed on single core.\n");
		printf(" Given cpu core mask = %x \n", g_params->cpu_core_mask);
		return -1;
	}
#endif

	ret = nvfuse_configure_spdk(g_io_manager, g_ipc_ctx, g_params->cpu_core_mask, NVFUSE_MAX_AIO_DEPTH);
	if (ret < 0)
		return -1;

#if 0
	/* optind must be reset before using getopt() */
	optind = 0;
	while ((op = getopt(app_argc, app_argv, "T:")) != -1) {
		switch (op) {
		case 'T':
			test_type = atoi(optarg);
			if (test_type < MAX_TEST || test_type > MILL_TEST) {
				fprintf(stderr, " Invalid test type = %d", test_type);
				goto INVALID_ARGS;
			}
			break;
		default:
			goto INVALID_ARGS;
		}
	}
#endif

	/* call lcore_recv() on every slave lcore */
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		printf(" launch secondary lcore = %d \n", lcore_id);
		rte_eal_remote_launch(rt_main, (void *)num_cores, lcore_id);
		num_cores++;
	}

	printf(" launch primary lcore = %d \n", rte_lcore_id());

	ret = rt_main((void *)num_cores);
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

	print_stats(num_cores, NUM_ELEMENTS(rt_ctx));

	nvfuse_deinit_spdk(g_io_manager, g_ipc_ctx);

	return ret;
#if 0
INVALID_ARGS:
	;
#endif
	nvfuse_core_usage(argv[0]);
	rt_usage(argv[0]);
	nvfuse_core_usage_example(argv[0]);
	return -1;
}
