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

#ifdef SPDK_ENABLED
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#define NDEBUG
#include <assert.h>

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_cycles.h>

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"

#include "nvfuse_types.h"
#include "nvfuse_config.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_core.h"
#include "nvfuse_malloc.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_stat.h"

//#ifdef LIST_HEAD
//#undef LIST_HEAD
//#endif
//#include "list.h"

#if NVFUSE_OS == NVFUSE_OS_LINUX
#include <libaio.h>
#	include <unistd.h>
#	include <sys/types.h>
#endif

struct ctrlr_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct ctrlr_entry	*next;
	char			name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct ns_entry		*next;
	//struct spdk_nvme_qpair	*qpair;
};

static struct ctrlr_entry *g_controllers = NULL;
static struct ns_entry *g_namespaces = NULL;

static char *ealargs[] = {
	"hello_world",
	"-c 0x1",
	"-n 4",
	"--proc-type=auto",
};

int spdk_close(struct nvfuse_io_manager *io_manager);
int spdk_cleanup(struct nvfuse_io_manager *io_manager);

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	/*
	 * spdk_nvme_ctrlr is the logical abstraction in SPDK for an NVMe
	 *  controller.  During initialization, the IDENTIFY data for the
	 *  controller is read using an NVMe admin command, and that data
	 *  can be retrieved using spdk_nvme_ctrlr_get_data() to get
	 *  detailed information on the controller.  Refer to the NVMe
	 *  specification for more details on IDENTIFY for NVMe controllers.
	 */
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->next = g_namespaces;
	g_namespaces = entry;

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid, num_ns;
	struct ctrlr_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	printf("Attached to %s\n", trid->traddr);

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	entry->next = g_controllers;
	g_controllers = entry;

	/*
	 * Each controller has one of more namespaces.  An NVMe namespace is basically
	 *  equivalent to a SCSI LUN.  The controller's IDENTIFY data tells us how
	 *  many namespaces exist on the controller.  For Intel(R) P3X00 controllers,
	 *  it will just be one namespace.
	 *
	 * Note that in NVMe, namespace IDs start at 1, not 0.
	 */
	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	printf("Using controller %s with %d namespaces.\n", entry->name, num_ns);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		register_ns(ctrlr, spdk_nvme_ctrlr_get_ns(ctrlr, nsid));
	}
}

s8 *spdk_qname_decode(s32 qid)
{
	switch (qid) {
	case SPDK_QUEUE_SYNC:
		return "SPDK_QUEUE_SYNC";
	case SPDK_QUEUE_AIO:
		return "SPDK_QUEUE_AIO";
	}
	return "SPDK_QUEUE_UNKOWN";
}

int spdk_alloc_qpair(struct nvfuse_io_manager *io_manager)
{
	struct ns_entry *ns_entry = g_namespaces;
	int i;

	for (i = 0; i < SPDK_QUEUE_NUM; i++) {
		printf(" Alloc NVMe Queue = %d (%s)\n", i, spdk_qname_decode(i));

		io_manager->spdk_queue[i] = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, 0);
		if (io_manager->spdk_queue[i] == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			return -1;
		}
	}

	return 0;
}

void spdk_release_qpair(struct nvfuse_io_manager *io_manager)
{
	int i;
	int ret;

	fprintf(stdout, " Release NVMe I/O Q pair.\n");

	for (i = 0; i < SPDK_QUEUE_NUM; i++) {
		ret = spdk_nvme_ctrlr_free_io_qpair(io_manager->spdk_queue[i]);
		if (ret < 0) {
			fprintf(stderr, " Error: release NVMe I/O Q pair.\n");
			break;
		}
	}

	printf(" Device Total I/O = %.3f MB\n",
	       (double)io_manager->perf_stat_dev.stat_dev.total_io_count * CLUSTER_SIZE / MB);
	printf(" Device Read I/O = %.3f MB\n",
	       (double)io_manager->perf_stat_dev.stat_dev.read_io_count * CLUSTER_SIZE / MB);
	printf(" Device Write I/O = %.3f MB\n",
	       (double)io_manager->perf_stat_dev.stat_dev.write_io_count * CLUSTER_SIZE / MB);

	printf(" Send stat dev msg to perf_stat_dev ring queue.\n");
	nvfuse_stat_ring_put(io_manager->ipc_ctx->stat_ring[DEVICE_STAT],
			     io_manager->ipc_ctx->stat_pool[DEVICE_STAT],
			     &io_manager->perf_stat_dev);
}

static int spdk_init(struct nvfuse_io_manager *io_manager)
{
	struct ns_entry *ns_entry = g_namespaces;
	int rc = 0;
	int i;
	int core;

	printf(" called: spdk init \n");

	if (ns_entry == NULL) {
		printf(" spdk init failed. \n");
		return -1;
	}

#if 0
	for (i = 0 ; i < SPDK_QUEUE_NUM; i++) {
		for (core = 0; core < SPDK_NUM_CORES; core++) {
			printf(" Alloc NVMe Queue = %d (%s)\n", i, spdk_qname_decode(i));

			io_manager->spdk_queue[core][i] = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, 0);
			if (io_manager->spdk_queue[core][i] == NULL) {
				printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
				return -1;
			}
		}
	}
#else
	for (i = 0 ; i < SPDK_QUEUE_NUM; i++) {
		io_manager->spdk_queue[i] = NULL;
	}
#endif

	printf(" alloc io qpair for nvme \n");

	return rc;
}

int spdk_cleanup(struct nvfuse_io_manager *io_manager)
{
	struct ns_entry *ns_entry = g_namespaces;
	struct ctrlr_entry *ctrlr_entry = g_controllers;

	while (ns_entry) {
		struct ns_entry *next = ns_entry->next;
		free(ns_entry);
		ns_entry = next;
	}

	while (ctrlr_entry) {
		struct ctrlr_entry *next = ctrlr_entry->next;

		spdk_nvme_detach(ctrlr_entry->ctrlr);
		free(ctrlr_entry);
		ctrlr_entry = next;
	}

	return 0;
}

static int spdk_prep(struct nvfuse_io_manager *io_manager, struct io_job *job)
{
	job->complete = 0;
	job->tag2 = (void *)io_manager;
	return 0;
}

/* spdk aio queue callback function */
static void spdk_callback(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct io_job *job = (struct io_job *)arg;
	struct nvfuse_io_manager *io_manager = (struct nvfuse_io_manager *)job->tag2;

	job->complete = 1;

	if (!spdk_nvme_cpl_is_error(cpl)) {
		job->ret = job->bytes;
	} else {
		job->ret = -1;
	}

	//printf(" spdk callback()\n");

	assert(!spdk_cjob_full(io_manager));

	//printf(" cjob head = %d \n", io_manager->cjob_head);

	io_manager->cjob[io_manager->cjob_head] = job;
	/* point to next location */
	io_manager->cjob_head = (io_manager->cjob_head + 1) % io_manager->iodepth;
	io_manager->cjob_cnt++;
}

static int spdk_submit(struct nvfuse_io_manager *io_manager, struct iocb **ioq, int qcnt)
{
	struct ns_entry *ns_entry = g_namespaces;
	struct io_job *job;
	int i;
	int ret = 0;

	for (i = 0; i < qcnt; i++) {
		struct iocb *iocb = ioq[i];
		job  = (struct io_job *)container_of(iocb, struct io_job, iocb);

		if (job->req_type == READ) {
			ret = spdk_nvme_ns_cmd_read(ns_entry->ns, io_manager->spdk_queue[SPDK_QUEUE_AIO], job->buf,
						    job->offset / 512, /* LBA start */
						    job->bytes / 512, /* number of LBAs */
						    spdk_callback, job, 0);

			io_manager->perf_stat_dev.stat_dev.read_io_count += (job->bytes / 4096);
		} else {
			ret = spdk_nvme_ns_cmd_write(ns_entry->ns, io_manager->spdk_queue[SPDK_QUEUE_AIO], job->buf,
						     job->offset / 512, /* LBA start */
						     job->bytes / 512, /* number of LBAs */
						     spdk_callback, job, 0);

			io_manager->perf_stat_dev.stat_dev.write_io_count += (job->bytes / 4096);
		}

		io_manager->perf_stat_dev.stat_dev.total_io_count += (job->bytes / 4096);

		if (ret != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			exit(1);
		}
	}

	//printf(" spdk: %d jobs submitted total count = %d\n", qcnt, io_manager->io_job_subq_count);

	return qcnt;
}

static int spdk_complete(struct nvfuse_io_manager *io_manager)
{
	s32 max_completions = 0;

#if NVFUSE_USE_USLEEP_US > 0
	rte_delay_us_block(io_manager->queue_cur_count * NVFUSE_USE_USLEEP_US);
#endif
	/* Polling */
	while (cjob_size(io_manager) == 0)
		spdk_nvme_qpair_process_completions(io_manager->spdk_queue[SPDK_QUEUE_AIO], max_completions);

	//printf(" spdk cjob size = %d, cnt = %d\n", cjob_size(io_manager), io_manager->cjob_cnt);

	return cjob_size(io_manager);
}

static struct io_job *spdk_getnextcjob(struct nvfuse_io_manager *io_manager)
{
	struct io_job *cur_job;

	assert(!spdk_cjob_empty(io_manager));

	cur_job = io_manager->cjob[io_manager->cjob_tail];
	io_manager->cjob[io_manager->cjob_tail] = NULL;

	io_manager->cjob_tail = (io_manager->cjob_tail + 1) % io_manager->iodepth;
	io_manager->cjob_cnt--;

	return cur_job;
}

static void spdk_resetnextsjob(struct nvfuse_io_manager *io_manager)
{

}

static void spdk_resetnextcjob(struct nvfuse_io_manager *io_manager)
{

}

struct spdk_job {
	struct ns_entry	*ns_entry;
	char		*buf;
	int		is_completed;
};

#if 0
static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct spdk_job *job = arg;

	/*
	 * The read I/O has completed.  Print the contents of the
	 *  buffer, free the buffer, then mark the sequence as
	 *  completed.  This will trigger the hello_world() function
	 *  to exit its polling loop.
	 */
	job->is_completed = 1;
}
#endif

static void
sync_req_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct spdk_job *job = arg;

	/*
	 * The write I/O has completed.  Free the buffer associated with
	 *  the write I/O and allocate a new zeroed buffer for reading
	 *  the data back from the NVMe namespace.
	 */
	job->is_completed = 1;
}

static int spdk_read_blk(struct nvfuse_io_manager *io_manager, long block, int count, void *buf)
{
	struct ns_entry *ns_entry;
	struct spdk_job job;
	int rbytes = 0;
	int res;

	ns_entry = g_namespaces;
	job.buf = buf;
	job.is_completed = 0;
	job.ns_entry = ns_entry;

	/*if ( block/32768 < 10 &&  (block % 32768) == NVFUSE_BD_OFFSET)
	printf(" bd read: block = %ld count = %d \n", block, count);*/

	io_manager->perf_stat_dev.stat_dev.total_io_count += count;
	io_manager->perf_stat_dev.stat_dev.read_io_count += count;

	res = spdk_nvme_ns_cmd_read(ns_entry->ns, io_manager->spdk_queue[SPDK_QUEUE_SYNC], job.buf,
				    (uint64_t)block * 8, /* LBA start */
				    count * 8, /* number of LBAs */
				    sync_req_complete, &job, 0);

	if (res != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}

#if NVFUSE_USE_USLEEP_US > 0
	rte_delay_us_block(count * NVFUSE_USE_USLEEP_US);
#endif

	while (!job.is_completed) {
		spdk_nvme_qpair_process_completions(io_manager->spdk_queue[SPDK_QUEUE_SYNC], 0);
	}

	rbytes = count * CLUSTER_SIZE;
	return rbytes;
}


static int spdk_write_blk(struct nvfuse_io_manager *io_manager, long block, int count, void *buf)
{
	struct ns_entry *ns_entry;
	struct spdk_job job;
	int wbytes = 0;
	int res;

	/*if (block/32768 < 10 &&  (block % 32768) == NVFUSE_BD_OFFSET)
	printf(" bd write: block = %ld count = %d \n", block, count);*/

	io_manager->perf_stat_dev.stat_dev.total_io_count += count;
	io_manager->perf_stat_dev.stat_dev.write_io_count += count;

	ns_entry = g_namespaces;
	job.buf = buf;
	job.is_completed = 0;
	job.ns_entry = ns_entry;

	res = spdk_nvme_ns_cmd_write(ns_entry->ns, io_manager->spdk_queue[SPDK_QUEUE_SYNC], job.buf,
				     (uint64_t)block * 8, /* LBA start */
				     count * 8, /* number of LBAs */
				     sync_req_complete, &job, 0);

	if (res != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

#if NVFUSE_USE_USLEEP_US > 0
	rte_delay_us_block(count * NVFUSE_USE_USLEEP_US);
#endif

	while (!job.is_completed) {
		spdk_nvme_qpair_process_completions(io_manager->spdk_queue[SPDK_QUEUE_SYNC], 0);
	}

	wbytes = count * CLUSTER_SIZE;

	return wbytes;
}

static int spdk_flush(struct nvfuse_io_manager *io_manager)
{
	struct ns_entry *ns_entry;
	struct spdk_job job;
	int res;

	ns_entry = g_namespaces;
	job.is_completed = 0;
	job.ns_entry = ns_entry;

	res = spdk_nvme_ns_cmd_flush(ns_entry->ns, io_manager->spdk_queue[SPDK_QUEUE_SYNC],
				     sync_req_complete, &job);

	if (res != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	while (!job.is_completed) {
		spdk_nvme_qpair_process_completions(io_manager->spdk_queue[SPDK_QUEUE_SYNC], 0);
	}

	return 0;
}

static int spdk_cancel(struct nvfuse_io_manager *io_manager, struct io_job *job)
{
	return 0;
}

int spdk_close(struct nvfuse_io_manager *io_manager)
{
	struct ns_entry *ns_entry = g_namespaces;
	struct ctrlr_entry *ctrlr_entry = g_controllers;
	int i;
	int core;

	// for (i = 0;i < SPDK_QUEUE_NUM; i++)
	// {
	//     for (core = 0; core < SPDK_NUM_CORES; core++)
	//     {
	//         spdk_nvme_ctrlr_free_io_qpair(io_manager->spdk_queue[core][i]);
	//     }
	// }

	while (ns_entry) {
		struct ns_entry *next = ns_entry->next;
		free(ns_entry);
		ns_entry = next;
	}

	while (ctrlr_entry) {
		struct ctrlr_entry *next = ctrlr_entry->next;

		spdk_nvme_detach(ctrlr_entry->ctrlr);
		free(ctrlr_entry);
		ctrlr_entry = next;
	}

	return 0;
}

int spdk_eal_init(s32 core_mask)
{
	char core_mask_str[128];
	int ret = 0;

	/*
	 * By default, the SPDK NVMe driver uses DPDK for huge page-based
	 *  memory management and NVMe request buffer pools.  Huge pages can
	 *  be either 2MB or 1GB in size (instead of 4KB) and are pinned in
	 *  memory.  Pinned memory is important to ensure DMA operations
	 *  never target swapped out memory.
	 *
	 * So first we must initialize DPDK.  "-c 0x1" indicates to only use
	 *  core 0.
	 */

	if (core_mask) {
		sprintf(core_mask_str, "0x%x", core_mask);
	} else {
		sprintf(core_mask_str, "0x1");
	}

	ealargs[1] = spdk_sprintf_alloc("-c %s", core_mask_str);
	if (ealargs[1] == NULL) {
		perror("ealargs spdk_sprintf_alloc");
		ret = -1;
		goto RET;
	}

	printf(" NVFUSE: ealargs[1] = %s\n", ealargs[1]);

	ret = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs);
	if (ret < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		ret = -1;
		goto RET;
	}

RET:
	free(ealargs[1]);
	return ret;
}

static int spdk_open(struct nvfuse_io_manager *io_manager, int flags)
{
	int rc;
	struct spdk_env_opts opts;

	/*
	 * SPDK relies on an abstraction around the local environment
	 * named env that handles memory allocation and PCI device operations.
	 * This library must be initialized first.
	 *
	 */
	spdk_env_opts_init(&opts);
	opts.name = "hello_world";
	opts.core_mask = io_manager->cpu_core_mask_str;
	opts.dpdk_mem_size = 8192;
	opts.shm_id = 1;
	spdk_env_init(&opts);

	printf("Initializing NVMe Controllers\n");
	/*
	 * Start the SPDK NVMe enumeration process.  probe_cb will be called
	 *  for each NVMe controller found, giving our application a choice on
	 *  whether to attach to each controller.  attach_cb will then be
	 *  called for each controller after the SPDK NVMe driver has completed
	 *  initializing the controller we chose to attach.
	 */
	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		spdk_close(io_manager);
		return 1;
	}

#if 1
	io_manager->blk_size = spdk_nvme_ns_get_sector_size(g_namespaces->ns);
	io_manager->total_blkcount = (spdk_nvme_ns_get_num_sectors(g_namespaces->ns) >> 3) << 3;
#else /* Reduced Device Size */
	io_manager->blk_size = spdk_nvme_ns_get_sector_size(g_namespaces->ns);
	io_manager->total_blkcount = (long) 2 * 1024 * 1024 * 1024 * 2;
#endif

	printf(" NVMe: sector size = %d, number of sectors = %ld\n", io_manager->blk_size,
	       io_manager->total_blkcount);
	printf(" NVMe: total capacity = %0.3fTB\n",
	       (double)io_manager->total_blkcount * io_manager->blk_size / 1024 / 1024 / 1024 / 1024);

	io_manager->aio_init(io_manager);

	printf(" spdk init: Ok\n");
#ifdef SPDK_BASIC_TEST
	{
		char *buf = (char *)nvfuse_alloc_aligned_buffer(4096);
		printf(" buf = %p\n", buf);
		while (1) {
			spdk_read_blk(io_manager, 0, 1, buf);
		}
		nvfuse_free_aligned_buffer(buf);
	}
#endif

	return 0;
}

static int spdk_dev_format(struct nvfuse_io_manager *io_manager)
{

	struct ctrlr_entry *ctrlr_entry = g_controllers;
	struct spdk_nvme_format format = {};
	u32 ns_id = 1;
	int ret = 0;

#if 0
	printf(" nvme format: started\n");
	format.lbaf	= 0; /* LBA format (e.g., 512, 512 + 8, 4096, 4096 + 8 */
	format.ms	= 0; /* metadata setting */
	format.pi	= 0; /* protection info */
	format.pil	= 0; /* pi location */
	format.ses	= 0; /* secure erase setting */

	ret = spdk_nvme_ctrlr_format(ctrlr_entry->ctrlr, ns_id, &format);
	if (ret) {
		fprintf(stdout, "nvme format: Failed\n");
		return -1;
	}
	printf(" nvme format: completed\n");
#else
	printf(" nvme format: skipped \n");
#endif

	return 0;
}

void nvfuse_init_spdk(struct nvfuse_io_manager *io_manager, char *filename, char *path, int qdepth)
{
	int len;
	int i;

	printf(" spdk_setup: filename = %s, qdepth = %d \n", filename, qdepth);

	len = strlen(path) + 1;
	io_manager->dev_path = (char *)malloc(len);
	memset(io_manager->dev_path, 0x00, len);
	strcpy(io_manager->dev_path, path);

	len = strlen(filename) + 1;
	io_manager->io_name = (char *)malloc(len);
	memset(io_manager->io_name, 0x00, len);
	strcpy(io_manager->io_name, filename);

	io_manager->io_open = spdk_open;
	io_manager->io_close = spdk_close;
	io_manager->io_read = spdk_read_blk;
	io_manager->io_write = spdk_write_blk;

	io_manager->cjob_head = 0;
	io_manager->cjob_tail = 0;
	io_manager->iodepth = AIO_MAX_QDEPTH;
	io_manager->queue_cur_count = 0;

	for (i = 0; i < io_manager->iodepth; i++) {
		io_manager->cjob[i] = NULL;
	}

	/* SPDK I/O Function Pointers */
	io_manager->aio_init = spdk_init;
	io_manager->aio_cleanup = spdk_cleanup;
	io_manager->aio_prep = spdk_prep;
	io_manager->aio_submit = spdk_submit;
	io_manager->aio_complete = spdk_complete;
	io_manager->aio_getnextcjob = spdk_getnextcjob;
	io_manager->aio_resetnextcjob = spdk_resetnextcjob;
	io_manager->aio_cancel = spdk_cancel;
	io_manager->dev_format = spdk_dev_format;
	io_manager->dev_flush = spdk_flush;

	printf("Initialization complete.\n");
}
#endif
