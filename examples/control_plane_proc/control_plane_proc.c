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
#include <signal.h>

//NDEBUG
#include <assert.h>

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

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_control_plane.h"
#include "nvfuse_aio.h"

#define DEINIT_IOM	1
#define UMOUNT		1

enum primary_status {
	PROC_RUNNING,
	PROC_STOP,
};

s32 status = PROC_STOP;

int primary_poll_core(struct nvfuse_handle *nvh, union nvfuse_ipc_msg *ipc_msg)
{
	struct nvfuse_superblock *sb = &nvh->nvh_sb;
	int ret;

	switch (ipc_msg->opcode) {
		case APP_REGISTER_REQ:
			ret = nvfuse_app_manage_table_add(nvh, ipc_msg->chan_id, ipc_msg->app_register_req.name);
			nvfuse_make_app_register_cpl(&ipc_msg->app_register_cpl, ret);
			break;

		case APP_UNREGISTER_REQ:
			ret = nvfuse_app_manage_table_remove(nvh, ipc_msg->chan_id, 
					ipc_msg->app_unregister_req.destroy_containers);
			nvfuse_make_app_unregister_cpl(&ipc_msg->app_unregister_cpl, ret);
			break;

		case SUPERBLOCK_COPY_REQ:
			{				
				s8 *appname = ipc_msg->superblock_copy_req.name;
				ret = nvfuse_superblock_copy(nvh, appname, &ipc_msg->superblock_copy_cpl, 
						(struct nvfuse_superblock_common *)sb);
			}
			nvfuse_make_superblock_copy_cpl(&ipc_msg->superblock_copy_cpl, ret);
			break;

		case BUFFER_ALLOC_REQ:
			ret = nvfuse_control_plane_buffer_alloc(nvh, ipc_msg->buffer_alloc_req.buffer_size);
			nvfuse_make_buffer_alloc_cpl(&ipc_msg->buffer_alloc_cpl, ret);
			break;

		case BUFFER_FREE_REQ:
			ret = nvfuse_control_plane_buffer_free(nvh, ipc_msg->buffer_free_req.buffer_size);
			nvfuse_make_buffer_free_cpl(&ipc_msg->buffer_free_cpl, ret);
			break;

		case CONTAINER_ALLOC_REQ:
			ret = nvfuse_control_plane_container_alloc(nvh, ipc_msg->chan_id, ipc_msg->container_alloc_req.type, ACQUIRED);
			nvfuse_make_container_alloc_cpl(&ipc_msg->container_alloc_cpl, ret);
			break;

		case CONTAINER_RELEASE_REQ:
			ret = nvfuse_control_plane_container_release(nvh, ipc_msg->chan_id, 
					ipc_msg->container_release_req.container_id);
			nvfuse_make_container_release_cpl(&ipc_msg->container_release_cpl, ret);
			break;

		case CONTAINER_RESERVATION_ACQUIRE_REQ:
			ret = nvfuse_control_plane_reservation_acquire(
					nvh,
					ipc_msg->container_reservation_acquire_req.container_id,
					ipc_msg->container_reservation_acquire_req.reservation_type);

			nvfuse_make_container_reservation_acquire_cpl(&ipc_msg->container_reservation_acquire_cpl, ret);
			break;

		case CONTAINER_RESERVATION_RELEASE_REQ:
			ret = nvfuse_control_plane_reservation_release(
					nvh,
					ipc_msg->container_reservation_release_req.container_id);
			nvfuse_make_container_reservation_release_cpl(&ipc_msg->container_reservation_release_cpl, ret);
			break;

		case HEALTH_CHECK_REQ:
			ret = nvfuse_control_plane_health_check(ipc_msg->chan_id);
			nvfuse_make_health_check_cpl(&ipc_msg->health_check_cpl, ret);
			break;

		default:
			fprintf(stderr, " Invalid opcode received from app (%d:%s)\n",
					ipc_msg->opcode,
					nvfuse_ipc_opcode_decode(ipc_msg->opcode));
			ret = -1;
			nvfuse_make_unkown_cpl(&ipc_msg->unknown_cpl, ret);
	}

}

int primary_poll(struct nvfuse_handle *nvh)
{
	struct nvfuse_superblock *sb = &nvh->nvh_sb;
	struct rte_ring *send_ring[SPDK_NUM_CORES], *recv_ring;
	struct rte_mempool *mempool;
	union nvfuse_ipc_msg *ipc_msg;
	int i;
	int ret;

	printf(" Process performs as primary process.\n");
	printf(" sizeof(ipc_msg) = %d bytes\n", sizeof(union nvfuse_ipc_msg));

	ret = nvfuse_control_plane_init(nvh);
	if (ret < 0)
	{
		return ret;
	}

	/* Recover app manage table from its log */
	nvfuse_load_app_table(nvh);
	
	/* Recover Allocation/Reservation table from its log */
	nvfuse_load_container_table(nvh);

	/* initialization of tx/rx ring buffers */
	for (i = 0;i < SPDK_NUM_CORES;i++)
	{
		send_ring[i] = nvfuse_ipc_get_sendq(&nvh->nvh_ipc_ctx, i);			
	}
	recv_ring = nvfuse_ipc_get_recvq(&nvh->nvh_ipc_ctx, 0);

	/* initialization of memory pool */
	mempool = nvfuse_ipc_mempool(nvfuse_ipc_mempool);

	/* set as PROC_RUNNING */
	status = PROC_RUNNING;
	
	while (status == PROC_RUNNING) {
		ipc_msg = NULL;

		if (rte_ring_dequeue(recv_ring, &ipc_msg) < 0) {
			//rte_pause();
			continue;
		}
		
		switch (ipc_msg->opcode) {
			case CONTAINER_ALLOC_REQ:
			case CONTAINER_RELEASE_REQ:
			case HEALTH_CHECK_REQ:
			case BUFFER_ALLOC_REQ:
				break;
			default:
				printf(" %ld Resv msg (%p:%d:%s) from secondary.\n",
						spdk_get_ticks(),
						ipc_msg,
						ipc_msg->opcode, 
						nvfuse_ipc_opcode_decode(ipc_msg->opcode));
		}

		/* event msg handler */
		ret = primary_poll_core(nvh, ipc_msg);

		switch (ipc_msg->opcode) {
			case CONTAINER_ALLOC_CPL:
			case CONTAINER_RELEASE_CPL:
			case HEALTH_CHECK_CPL:
			case BUFFER_ALLOC_CPL:
				break;
			default:
			printf(" %ld Send cpl (opcode = %d:%s) to core = %d via channel = %d\n\n",
			spdk_get_ticks(),
			ipc_msg->opcode, nvfuse_ipc_opcode_decode(ipc_msg->opcode), ipc_msg->chan_id, 
			ipc_msg->chan_id);
		}

		if (rte_ring_enqueue(send_ring[ipc_msg->chan_id], ipc_msg) < 0) {
			printf("Failed to send message - message discarded\n");
			rte_mempool_put(mempool, ipc_msg);
		}
	}

	nvfuse_control_plane_exit(nvh);	
}

void sig_handler(int signum)
{
	printf(" signal handler has been invoked.\n");
	status = PROC_STOP;
	signal(SIGINT, SIG_DFL);
}

int main(int argc, char *argv[])
{
	struct nvfuse_handle *nvh;	
	struct nvfuse_io_manager io_manager;
	struct nvfuse_ipc_context ipc_ctx;
	struct nvfuse_params params;
	int ret;

	/* register signal handler (e.g., Ctrl + C) */
	signal(SIGINT, (void *)sig_handler);

	ret = nvfuse_parse_args(argc, argv, &params);
	if (ret < 0)
		return -1;

	ret = nvfuse_configure_spdk(&io_manager, &ipc_ctx, params.cpu_core_mask, NVFUSE_MAX_AIO_DEPTH);
	if (ret < 0)
		return -1;

	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(&io_manager, &ipc_ctx, &params);
	if (nvh == NULL)
	{
		fprintf(stderr, "Error: nvfuse_create_handle()\n");
		return -1;
	}

	if (spdk_process_is_primary())
	{
		primary_poll(nvh);
	}
	else
	{
	    printf(" Warning: This process is not a primary process!.\n");
	}
	
	rte_malloc_dump_stats(stdout, NULL);

	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);
	
	nvfuse_deinit_spdk(&io_manager, &ipc_ctx);
	printf(" Primary process has been stopped \n");

	return 0;
}
