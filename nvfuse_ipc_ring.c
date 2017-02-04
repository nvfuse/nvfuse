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
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/queue.h>
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
#include <cmdline_rdline.h>
#include <cmdline_parse.h>
#include <cmdline_socket.h>
#include <cmdline_parse_string.h>
#include <cmdline.h>
//#include "mp_commands.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"

#include "nvfuse_ipc_ring.h"

static s8 *_STAT_MSG_POOL[] = {"STAT_MSG_POOL_DEVICE", "STAT_MSG_POOL_AIO", "STAT_MSG_POOL_RT"};
static s8 *_STAT_RX_RING[] = {"STAT_RX_RING_DEVICE", "STAT_RX_RING_AIO", "STAT_RX_RINGL_RT"};

#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1

struct rte_ring *nvfuse_ipc_get_sendq(struct nvfuse_ipc_context *ipc_ctx, int chan_id)
{
	struct rte_ring *queue;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) 
	{	/* primary to secondary */
		queue = ipc_ctx->send_ring[chan_id];
	}
	else
	{	/* secondary to primary */
		queue = ipc_ctx->send_ring[chan_id];
	}
	return queue;
}

struct rte_ring *nvfuse_ipc_get_recvq(struct nvfuse_ipc_context *ipc_ctx, int chan_id)
{
	struct rte_ring *queue;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) 
	{	/* secondary to primary */
		queue = ipc_ctx->recv_ring[chan_id];
	}
	else
	{ 	/* primary to secondary */	
		queue = ipc_ctx->recv_ring[chan_id];
	}
	return queue;
}
struct rte_mempool *nvfuse_ipc_mempool(struct nvfuse_ipc_context *ipc_ctx)
{
	return ipc_ctx->message_pool;
}

s8 *nvfuse_ipc_opcode_decode(enum ipc_opcode opcode)
{
	switch (opcode) {
	    case APP_REGISTER_REQ:
			return "APP_REGISTER_REQ";
    	case APP_REGISTER_CPL:
			return "APP_REGISTER_CPL";
    	case APP_UNREGISTER_REQ:
			return "APP_UNREGISTER_REQ";
    	case APP_UNREGISTER_CPL:
			return "APP_UNREGISTER_CPL";
    	case SUPERBLOCK_COPY_REQ:
			return "SUPERBLOCK_COPY_REQ";
    	case SUPERBLOCK_COPY_CPL:
			return "SUPERBLOCK_COPY_CPL";
    	case BUFFER_ALLOC_REQ:
			return "BUFFER_ALLOC_REQ";
    	case BUFFER_ALLOC_CPL:
			return "BUFFER_ALLOC_CPL";
    	case BUFFER_FREE_REQ:
			return "BUFFER_FREE_REQ";
    	case BUFFER_FREE_CPL:
			return "BUFFER_FREE_CPL";
    	case CONTAINER_ALLOC_REQ:
			return "CONTAINER_ALLOC_REQ";
    	case CONTAINER_ALLOC_CPL:
			return "CONTAINER_ALLOC_CPL";
    	case CONTAINER_RELEASE_REQ:
			return "CONTAINER_RELEASE_REQ";
    	case CONTAINER_RELEASE_CPL:
			return "CONTAINER_RELEASE_CPL";
    	case CONTAINER_RESERVATION_ACQUIRE_REQ:
			return "CONTAINER_RESERVATION_ACQUIRE_REQ";
    	case CONTAINER_RESERVATION_ACQUIRE_CPL:
			return "CONTAINER_RESERVATION_ACQUIRE_CPL";
    	case CONTAINER_RESERVATION_RELEASE_REQ:
			return "CONTAINER_RESERVATION_RELEASE_REQ";
    	case CONTAINER_RESERVATION_RELEASE_CPL:
			return "CONTAINER_RESERVATION_RELEASE_CPL";
    	case HEALTH_CHECK_REQ:
			return "HEALTH_CHECK_REQ";
    	case HEALTH_CHECK_CPL:
			return "HEALTH_CHECK_CPL";
	}
}

void nvfuse_make_app_register_req(struct app_register_req *req)
{
	req->opcode = APP_REGISTER_REQ;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_app_register_cpl(struct app_register_cpl *req, s32 ret)
{
	printf(" called: %s\n", __FUNCTION__);
	req->opcode = APP_REGISTER_CPL;	
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_health_check_req(struct health_check_req *req)
{
	req->opcode = HEALTH_CHECK_REQ;	
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_health_check_cpl(struct health_check_cpl *req, s32 ret)
{
	//printf(" called: %s\n", __FUNCTION__);
	req->opcode = HEALTH_CHECK_CPL;	
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_unkown_cpl(struct unknown_cpl *req, s32 ret)
{
	printf(" called: %s\n", __FUNCTION__);
	req->opcode = UNKOWN_CPL;	
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_app_unregister_req(struct app_unregister_req *req)
{
	req->opcode = APP_UNREGISTER_REQ;	
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_app_unregister_cpl(struct app_unregister_cpl *req, s32 ret)
{
	req->opcode = APP_UNREGISTER_CPL;	
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_superblock_copy_req(struct superblock_copy_req *req)
{
	req->opcode = SUPERBLOCK_COPY_REQ;	
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_superblock_copy_cpl(struct superblock_copy_cpl *req, s32 ret)
{
	req->opcode = SUPERBLOCK_COPY_CPL;	
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_buffer_alloc_req(struct buffer_alloc_req *req)
{
	req->opcode = BUFFER_ALLOC_REQ;	
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_buffer_alloc_cpl(struct buffer_alloc_cpl *req, s32 ret)
{
	req->opcode = BUFFER_ALLOC_CPL;	
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_buffer_free_req(struct buffer_free_req *req)
{
	req->opcode = BUFFER_FREE_REQ;	
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_buffer_free_cpl(struct buffer_free_cpl *req, s32 ret)
{
	req->opcode = BUFFER_FREE_CPL;	
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_container_alloc_req(struct container_alloc_req *req)
{
	req->opcode = CONTAINER_ALLOC_REQ;		
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_container_alloc_cpl(struct container_alloc_cpl *req, s32 ret)
{
	req->opcode = CONTAINER_ALLOC_CPL;	
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_container_release_req(struct container_release_req *req)
{
	req->opcode = CONTAINER_RELEASE_REQ;		
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_container_release_cpl(struct container_release_cpl *req, s32 ret)
{
	req->opcode = CONTAINER_RELEASE_CPL;
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_container_reservation_acquire_req(struct container_reservation_acquire_req *req)
{
	req->opcode = CONTAINER_RESERVATION_ACQUIRE_REQ;	
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_container_reservation_acquire_cpl(struct container_reservation_acquire_cpl *req, s32 ret)
{
	req->opcode = CONTAINER_RESERVATION_ACQUIRE_CPL;
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_container_reservation_release_req(struct container_reservation_release_req *req)
{
	req->opcode = CONTAINER_RESERVATION_RELEASE_REQ;	
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_make_container_reservation_release_cpl(struct container_reservation_release_cpl *req, s32 ret)
{
	req->opcode = CONTAINER_RESERVATION_ACQUIRE_CPL;	
	req->chan_id = req->chan_id;
	req->ret = ret;
	req->tag1 = 0;
	req->tag2 = 0;
}

void nvfuse_try_ring_dequeue(struct rte_ring *recv_ring, union nvfuse_ipc_msg **msg, int timeout)
{
	void *ptr;
	int count = 0;

	do {
		if (rte_ring_dequeue(recv_ring, &ptr) < 0) {
			rte_pause();
			continue;
		}
		break;
	} while (1);

	//printf(".\n");
	*msg = (union nvfuse_ipc_msg *)ptr;	
}

int nvfuse_put_channel_id(struct nvfuse_ipc_context *ipc_ctx, int channel_id)
{
	char *ipc_msg;

	if (rte_mempool_get(ipc_ctx->message_pool, (void **)&ipc_msg) < 0)
	{
			rte_panic("Failed to get message buffer\n");
			return -1;
	}
	
	memset(ipc_msg, 0x00, NVFUSE_IPC_MSG_SIZE);
	*(s32 *)ipc_msg = channel_id;
	if (rte_ring_enqueue(ipc_ctx->id_gen, ipc_msg) < 0) {
		printf("Failed to send message - message discarded\n");		
		return -1;
	}

	return 0;
}

int nvfuse_get_channel_id(struct nvfuse_ipc_context *ipc_ctx)
{
	s32 *my_id;
	s32 val;
	do {
		if (rte_ring_dequeue(ipc_ctx->id_gen, (void **)&my_id) < 0) {
			rte_pause();
			continue;
		}
		break;
	} while (1);
	
	val = *my_id;

	rte_mempool_put(ipc_ctx->message_pool, my_id);

	return val;
}

/* perf stat ring queue */
int perf_stat_ring_create(struct rte_ring **stat_rx_ring, struct rte_mempool **stat_message_pool, enum stat_type type)
{
	const unsigned flags = 0;
	const unsigned ring_size = 16384; /* FIXME: needed to find optimal value */
	const unsigned pool_size = 8192;
	const unsigned pool_cache = 64;
	const unsigned priv_data_sz = 0;
	const unsigned string_size = PERF_STAT_SIZE;

	//printf(" stat ring size = %d \n", ring_size);
	assert(type < NUM_STAT_TYPE);
	
	*stat_rx_ring = rte_ring_create(_STAT_RX_RING[type], ring_size, rte_socket_id(), flags);
	*stat_message_pool = rte_mempool_create(_STAT_MSG_POOL[type], pool_size,
				string_size, pool_cache, priv_data_sz,
				NULL, NULL, NULL, NULL,
				rte_socket_id(), flags);
	
	if (*stat_rx_ring == NULL)
		rte_exit(EXIT_FAILURE, "Problem getting stat rx ring\n");
	if (*stat_message_pool == NULL)
		rte_exit(EXIT_FAILURE, "Problem getting stat msg pool\n");

	return 0;
}

/* perf stat ring queue */
int perf_stat_ring_lookup(struct rte_ring **stat_rx_ring, struct rte_mempool **stat_message_pool, enum stat_type type)
{	
	assert(type < NUM_STAT_TYPE);

	*stat_rx_ring = rte_ring_lookup(_STAT_RX_RING[type]);
	*stat_message_pool = rte_mempool_lookup(_STAT_MSG_POOL[type]);
	
	if (*stat_rx_ring == NULL)
		rte_exit(EXIT_FAILURE, "Problem getting stat rx ring\n");
	if (*stat_message_pool == NULL)
		rte_exit(EXIT_FAILURE, "Problem getting stat msg pool\n");

	return 0;
}

/* perf stat ring queue */
int nvfuse_stat_ring_put(struct rte_ring *stat_tx_ring, 
						struct rte_mempool *stat_message_pool,
						union perf_stat *stat)
{
	char *ipc_msg;

	if (rte_mempool_get(stat_message_pool, (void **)&ipc_msg) < 0)
	{
			rte_panic("Failed to get message buffer\n");
			return -1;
	}
	
	memcpy(ipc_msg, stat, sizeof(union perf_stat));
	
	if (rte_ring_enqueue(stat_tx_ring, ipc_msg) < 0) {
		printf("Failed to send message - message discarded\n");		
		return -1;
	}

	return 0;
}

/* perf stat ring queue */
int nvfuse_stat_ring_get(struct rte_ring *stat_rx_ring, 
						struct rte_mempool *stat_message_pool,
						union perf_stat *stat)
{
	char *ipc_msg;	

	do {
		if (rte_ring_dequeue(stat_rx_ring, (void **)&ipc_msg) < 0) {
			rte_pause();
			continue;
		}
		break;
	} while (1);
	
	memcpy(stat, ipc_msg, sizeof(union perf_stat));

	rte_mempool_put(stat_message_pool, ipc_msg);

	return 0;
}

int nvfuse_ipc_init(struct nvfuse_ipc_context *ipc_ctx)
{
	const unsigned flags = 0;
	const unsigned ring_size = 16384;
	const unsigned pool_size = 1024;
	const unsigned pool_cache = 32;
	const unsigned priv_data_sz = 0;

    int i;
	int ret;
	
	printf(" ipc init \n");
	printf(" rte_socket_id() = %d \n", rte_socket_id());
	printf(" rte_lcore_id() = %d \n", rte_lcore_id());
	
	if (!spdk_process_is_primary() && !rte_lcore_id())
	{
		rte_exit(EXIT_FAILURE, " Secondary process id cannot be zero.!!!\n");
	}

    for (i = 0;i < SPDK_NUM_CORES; i++)
    {
        sprintf(ipc_ctx->_PRI_2_SEC[i], "%s_%d", NVFUSE_PRI_TO_SEC_NAME, i);
        sprintf(ipc_ctx->_SEC_2_PRI[i], "%s_%d", NVFUSE_SEC_TO_PRI_NAME, i);
    }
	strcpy(ipc_ctx->_MSG_POOL, NVFUSE_MSG_POOL_NAME);
	strcpy(ipc_ctx->ID_GEN, "ID_GEN");

	if (rte_eal_process_type() == RTE_PROC_PRIMARY)
	{
        for (i = 0;i < SPDK_NUM_CORES; i++)
        {
            ipc_ctx->send_ring[i] = rte_ring_create(ipc_ctx->_PRI_2_SEC[i], ring_size, rte_socket_id(), flags);
			if (ipc_ctx->send_ring[i] == NULL)
				rte_exit(EXIT_FAILURE, "Problem getting sending ring\n");            
			
			if (i == 0) 
			{
				ipc_ctx->recv_ring[i] = rte_ring_create(ipc_ctx->_SEC_2_PRI[i], ring_size, rte_socket_id(), flags);
				if (ipc_ctx->recv_ring[i] == NULL)
					rte_exit(EXIT_FAILURE, "Problem getting receiving ring\n");
			} 
			else
			{ /* sharing ring queue among all secondary processes */
				ipc_ctx->recv_ring[i] = ipc_ctx->recv_ring[0];
			}		

			printf(" send ring = %p, recv ring = %p\n", ipc_ctx->send_ring[i], ipc_ctx->recv_ring[i]);
        }	
		
		ipc_ctx->message_pool = rte_mempool_create(ipc_ctx->_MSG_POOL, pool_size,
				NVFUSE_IPC_MSG_SIZE, pool_cache, priv_data_sz,
				NULL, NULL, NULL, NULL,
				rte_socket_id(), flags);

		ipc_ctx->id_gen = rte_ring_create(ipc_ctx->ID_GEN, ring_size, rte_socket_id(), flags);
		if (ipc_ctx->id_gen == NULL)
			rte_exit(EXIT_FAILURE, "Problem getting id_gen ring\n");

		for (i = 1; i < SPDK_NUM_CORES; i++)
		{
			ret = nvfuse_put_channel_id(ipc_ctx, i);
			if (ret < 0) {
				rte_exit(EXIT_FAILURE, "Problem putting channel id\n");
				return -1;
			}
		}
				
        printf(" IPC init in primary core\n");
	} else { /* in case of secondary process */
		for (i = 0;i < SPDK_NUM_CORES; i++)
        {			
			if (i == 0) 
			{		
				ipc_ctx->send_ring[i] = rte_ring_lookup(ipc_ctx->_SEC_2_PRI[i]);
				if (ipc_ctx->send_ring[i] == NULL)
					rte_exit(EXIT_FAILURE, "Problem getting sending ring\n");
			} 
			else
			{	/* sharing ring queue among all secondary processes */
				ipc_ctx->send_ring[i] = ipc_ctx->send_ring[0];
			}
			
			ipc_ctx->recv_ring[i] = rte_ring_lookup(ipc_ctx->_PRI_2_SEC[i]);
			if (ipc_ctx->recv_ring[i] == NULL)
				rte_exit(EXIT_FAILURE, "Problem getting receiving ring\n");	
						
			printf(" send ring = %p, recv ring = %p\n", ipc_ctx->send_ring[i], ipc_ctx->recv_ring[i]);
		}
		
		ipc_ctx->id_gen = rte_ring_lookup(ipc_ctx->ID_GEN);
		if (ipc_ctx->id_gen == NULL)
			rte_exit(EXIT_FAILURE, "Problem getting receiving ring\n");	

		ipc_ctx->message_pool = rte_mempool_lookup(ipc_ctx->_MSG_POOL);
		/* moved to nvfuse_create_handle or nvfuse_mount */
		#if 1
		ipc_ctx->my_channel_id = nvfuse_get_channel_id(ipc_ctx);
		printf(" Obtained Channel ID = %d \n", ipc_ctx->my_channel_id);
		#else
		ipc_ctx->my_channel_id = 0;
		#endif
	}

	if (ipc_ctx->message_pool == NULL)
		rte_exit(EXIT_FAILURE, "Problem getting message pool\n");
	
	if (spdk_process_is_primary())	
	{
		for (i = 0;i < NUM_STAT_TYPE; i++)
			ret = perf_stat_ring_create(&ipc_ctx->stat_ring[i], &ipc_ctx->stat_pool[i], i);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Problem getting message pool\n");
	}
	else
	{
		for (i = 0;i < NUM_STAT_TYPE; i++)
			ret = perf_stat_ring_lookup(&ipc_ctx->stat_ring[i], &ipc_ctx->stat_pool[i], i);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Problem getting message pool\n");
	}

	printf(" IPC initialized successfully for %s\n", 
			rte_eal_process_type() == RTE_PROC_PRIMARY ? "Primary Core":"Secondary Core");
	return 0;
}

void nvfuse_ipc_exit(struct nvfuse_ipc_context *ipc_ctx)
{	 
	int i;

	printf(" ipc deinit ...\n");
	nvfuse_put_channel_id(ipc_ctx, ipc_ctx->my_channel_id);
	printf(" Release channel = %d \n", ipc_ctx->my_channel_id);

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) 
	{
        for (i = 0;i < SPDK_NUM_CORES; i++)
        {			
            rte_ring_free(ipc_ctx->send_ring[i]);
			if (i == 0)
            	rte_ring_free(ipc_ctx->recv_ring[i]);			
        }

		rte_mempool_free(ipc_ctx->message_pool);       
	}
}

int nvfuse_send_msg_to_primary_core(struct rte_ring *send_ring, struct rte_ring *recv_ring, 									
									union nvfuse_ipc_msg *ipc_msg, s32 opcode)
{	
	int ret;
	int cpl_opcode;
	
	switch (opcode)
	{
		case APP_REGISTER_REQ:
			nvfuse_make_app_register_req(&ipc_msg->app_register_req);
			break;
		case APP_UNREGISTER_REQ:
			nvfuse_make_app_unregister_req(&ipc_msg->app_unregister_req);
			break;
		case SUPERBLOCK_COPY_REQ:
			nvfuse_make_superblock_copy_req(&ipc_msg->superblock_copy_req);
			break;
		case BUFFER_ALLOC_REQ:			
			nvfuse_make_buffer_alloc_req(&ipc_msg->buffer_alloc_req);
			break;		
		case BUFFER_FREE_REQ:
			nvfuse_make_buffer_free_req(&ipc_msg->buffer_free_req);
			break;		
		case CONTAINER_ALLOC_REQ:
			nvfuse_make_container_alloc_req(&ipc_msg->container_alloc_req);
			break;		
		case CONTAINER_RELEASE_REQ:
			nvfuse_make_container_release_req(&ipc_msg->container_release_req);
			break;		
		case CONTAINER_RESERVATION_ACQUIRE_REQ:
			nvfuse_make_container_reservation_acquire_req(&ipc_msg->container_reservation_acquire_req);			
			break;
		case CONTAINER_RESERVATION_RELEASE_REQ:
			nvfuse_make_container_reservation_release_req(&ipc_msg->container_reservation_release_req);
			break;
		case HEALTH_CHECK_REQ:
			nvfuse_make_health_check_req(&ipc_msg->health_check_req);
			break;
		default:
			fprintf(stderr, " Error: invalid opcode = %d (%s)", 
						opcode, nvfuse_ipc_opcode_decode(opcode));
	}
	
	switch (ipc_msg->opcode) {
			case CONTAINER_ALLOC_REQ:
			case HEALTH_CHECK_REQ:
			case BUFFER_ALLOC_REQ:
				break;
			default:
			printf(" %ld send req (%p:%d:%s) to primary core\n", 
			spdk_get_ticks(),
			ipc_msg, ipc_msg->opcode, 
			nvfuse_ipc_opcode_decode(ipc_msg->opcode));
	}

	if (rte_ring_enqueue(send_ring, ipc_msg) < 0) {
		printf("Failed to send message - message discarded\n");		
		return -1;
	}
	
	//rte_ring_dump(stdout, recv_ring);
	nvfuse_try_ring_dequeue(recv_ring, &ipc_msg, 0);
	
	switch (ipc_msg->opcode) {
			case CONTAINER_ALLOC_CPL:
			case HEALTH_CHECK_CPL:
			case BUFFER_ALLOC_CPL:
				break;
			default:
			printf(" %ld recv cpl (%d:%s, ret = %d) from primary core\n", 
			spdk_get_ticks(),
			ipc_msg->opcode, nvfuse_ipc_opcode_decode(ipc_msg->opcode), ipc_msg->ret);
			printf("\n");
			fflush(stdout);
	}
	//rte_ring_dump(stdout, recv_ring);	
	ret = ipc_msg->ret;	
	return ret;
}

s32 nvfuse_send_app_unregister_req(struct nvfuse_handle *nvh, s32 destroy_containers)
{
	struct rte_ring *send_ring, *recv_ring;
	struct rte_mempool *mempool;		
	union nvfuse_ipc_msg *ipc_msg;
	s32 ret;

	/* INITIALIZATION OF TX/RX RING BUFFERS */
	send_ring = nvfuse_ipc_get_sendq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);
	recv_ring = nvfuse_ipc_get_recvq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);

	/* initialization of memory pool */
	mempool = nvfuse_ipc_mempool(&nvh->nvh_ipc_ctx);

	/*
	 * UNREGISTRATION OF HOST ID 
	 */
	if (rte_mempool_get(mempool, (void **)&ipc_msg) < 0)
	{
		rte_panic("Failed to get message buffer\n");
		return -1;
	}

	memset(ipc_msg->bytes, 0x00, NVFUSE_IPC_MSG_SIZE);
	ipc_msg->chan_id = nvh->nvh_ipc_ctx.my_channel_id;
	ipc_msg->app_unregister_req.destroy_containers = destroy_containers; 

	/* SEND APP_UNREGISTER_REQ TO PRIMARY CORE */
	ret = nvfuse_send_msg_to_primary_core(send_ring, recv_ring, ipc_msg, APP_UNREGISTER_REQ);
	if (ret)
	{
		fprintf(stderr, " Failed to unreigster app from control plane \n");
	}
	rte_mempool_put(mempool, ipc_msg);

	return ret;
}

s32 nvfuse_send_alloc_buffer_req(struct nvfuse_handle *nvh, s32 buffer_size)
{
	struct nvfuse_superblock *sb = &nvh->nvh_sb;
	struct rte_ring *send_ring, *recv_ring;
	struct rte_mempool *mempool;		
	union nvfuse_ipc_msg *ipc_msg;
	s32 ret;	

	/* INITIALIZATION OF TX/RX RING BUFFERS */
	send_ring = nvfuse_ipc_get_sendq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);
	recv_ring = nvfuse_ipc_get_recvq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);

	/* initialization of memory pool */
	mempool = nvfuse_ipc_mempool(&nvh->nvh_ipc_ctx);

	/*
	* ALLOCATION OF BUFFER
	*/
	if (rte_mempool_get(mempool, (void *)&ipc_msg) < 0)
	{
		rte_panic("Failed to get message buffer\n");
		return -1;
	}
	
	memset(ipc_msg->bytes, 0x00, NVFUSE_IPC_MSG_SIZE);
	ipc_msg->chan_id = nvh->nvh_ipc_ctx.my_channel_id;
	ipc_msg->buffer_alloc_req.buffer_size = buffer_size; // in 4K page unit
	// necessary to keep this value somewhere
	{
		u64 start_tsc = spdk_get_ticks();
		/* SEND BUFFER_ALLOC_REQ TO PRIMARY CORE */
		ret = nvfuse_send_msg_to_primary_core(send_ring, recv_ring, ipc_msg, BUFFER_ALLOC_REQ);
		if (ret == 0)
		{
			//printf("Failed to get buffer\n");
			ret = -1;
		}
		else
		{
			buffer_size = ret;
			//printf(" allocated buffer size = %d pages\n", buffer_size);
		}

		sb->perf_stat_ipc.stat_ipc.total_tsc[BUFFER_ALLOC_REQ] += (spdk_get_ticks() - start_tsc);
		sb->perf_stat_ipc.stat_ipc.total_count[BUFFER_ALLOC_REQ]++;
	}	
	
	rte_mempool_put(mempool, ipc_msg);	
	
	return ret;
}

s32 nvfuse_send_dealloc_buffer_req(struct nvfuse_handle *nvh, s32 buffer_size)
{
	struct nvfuse_superblock *sb = &nvh->nvh_sb;
	struct rte_ring *send_ring, *recv_ring;
	struct rte_mempool *mempool;		
	union nvfuse_ipc_msg *ipc_msg;
	s32 ret;

	/* INITIALIZATION OF TX/RX RING BUFFERS */
	send_ring = nvfuse_ipc_get_sendq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);
	recv_ring = nvfuse_ipc_get_recvq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);

	/* initialization of memory pool */
	mempool = nvfuse_ipc_mempool(&nvh->nvh_ipc_ctx);

	/*
	* ALLOCATION OF BUFFER
	*/
	if (rte_mempool_get(mempool, (void *)&ipc_msg) < 0)
	{
		rte_panic("Failed to get message buffer\n");
		return -1;
	}
	
	memset(ipc_msg->bytes, 0x00, NVFUSE_IPC_MSG_SIZE);
	ipc_msg->chan_id = nvh->nvh_ipc_ctx.my_channel_id;
	ipc_msg->buffer_free_req.buffer_size = buffer_size; // in 4K page unit
	{
		u64 start_tsc = spdk_get_ticks();

		/* SEND BUFFER_DEALLOC_REQ TO PRIMARY CORE */
		ret = nvfuse_send_msg_to_primary_core(send_ring, recv_ring, ipc_msg, BUFFER_FREE_REQ);
		if (ret < 0)
		{
			rte_panic("Failed to dealloc buffer\n");		
		}

		sb->perf_stat_ipc.stat_ipc.total_tsc[BUFFER_FREE_REQ] += (spdk_get_ticks() - start_tsc);
		sb->perf_stat_ipc.stat_ipc.total_count[BUFFER_FREE_REQ]++;
	}

	rte_mempool_put(mempool, ipc_msg);
	return ret;
}