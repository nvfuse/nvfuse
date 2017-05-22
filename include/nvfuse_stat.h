/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2017 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 04/02/2017
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

#include <sys/time.h>
#include <sys/resource.h>

#ifndef __NVFUSE_STAT__
#define __NVFUSE_STAT__

enum stat_type {
	DEVICE_STAT		= 0,
	AIO_STAT		= 1,
	RT_STAT			= 2,
	IPC_STAT        = 3,
	RUSAGE_STAT     = 4,
	NUM_STAT_TYPE	= 5
};

/* aio perf stat */
struct perf_stat_aio {
	u32 stat_type;
	u32 lcore_id;
	u32 sequence;

	u64 aio_start_tsc;
	u64 aio_end_tsc;
	u64 aio_execution_tsc;

	u64 aio_lat_total_tsc;
	u64 aio_lat_total_count;
	u64 aio_lat_min_tsc;
	u64 aio_lat_max_tsc;
	u64 aio_total_size;
	struct rusage aio_start_rusage;
	struct rusage aio_result_rusage;
	struct timeval aio_usr_time;
	struct timeval aio_sys_time;
};

/* regression test stat*/
struct perf_stat_rt {
	u32 stat_type;
	u32 lcore_id;
	u32 sequence;

	// double create_time;
	// double lookup_time;
	// double delete_time;
	double total_time;
};

struct perf_stat_dev {
	u32 stat_type;
	u32 lcore_id;
	u32 sequence;

	u64 total_io_count; // 4KB unit
	u64 read_io_count; // 4KB unit
	u64 write_io_count;
};

struct perf_stat_ipc {
	u32 stat_type;
	u32 lcore_id;
	u32 sequence;

	u64 total_tsc[22];
	u64 total_count[22];
};

struct perf_stat_rusage {
	struct rusage start;
	struct rusage end;
	struct rusage result;
	u32 tag;
};

union perf_stat {

	struct {
		u32 stat_type;
		u32 lcore_id;
		u32 sequence;
	};

	struct perf_stat_aio stat_aio;
	struct perf_stat_rt stat_rt;
	struct perf_stat_dev stat_dev;
	struct perf_stat_ipc stat_ipc;
	struct perf_stat_rusage stat_rusage;
};


#define PERF_STAT_SIZE (DIV_UP(sizeof(union perf_stat), 512))

#endif