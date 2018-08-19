/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 16/01/2017
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
#include <sys/resource.h>
#include <nvfuse_core.h>

#ifndef _NVFUSE_IPC_RING_H
#define _NVFUSE_IPC_RING_H

#define NVFUSE_IPC_MSG_SIZE 128

enum ipc_opcode {
	APP_REGISTER_REQ = 0x1,
	APP_REGISTER_CPL,
	APP_UNREGISTER_REQ,
	APP_UNREGISTER_CPL,
	SUPERBLOCK_COPY_REQ, /* physical memory copy*/
	SUPERBLOCK_COPY_CPL,
	BUFFER_ALLOC_REQ,
	BUFFER_ALLOC_CPL,
	BUFFER_FREE_REQ,
	BUFFER_FREE_CPL,
	CONTAINER_ALLOC_REQ,
	CONTAINER_ALLOC_CPL,
	CONTAINER_RELEASE_REQ,
	CONTAINER_RELEASE_CPL,
	CONTAINER_RESERVATION_ACQUIRE_REQ,
	CONTAINER_RESERVATION_ACQUIRE_CPL,
	CONTAINER_RESERVATION_RELEASE_REQ,
	CONTAINER_RESERVATION_RELEASE_CPL,
	HEALTH_CHECK_REQ,
	HEALTH_CHECK_CPL,
	UNKOWN_CPL,
	NUM_IPC_MSGS,
};

struct app_register_req {
	s32 opcode;     // [4]
	s32 chan_id;
	s8 name[32];    // [24]
	s32 tag1;       // [32]
	s32 tag2;       // [36]
};

struct app_register_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 tag1;
	s32 tag2;
};

/* type of app unregisteration */
enum app_unregister_with_destroying_opcode {
	APP_UNREGISTER_WITHOUT_DESTROYING_CONTAINERS = 0,
	APP_UNREGISTER_WITH_DESTROYING_CONTAINERS = 1,
};

struct app_unregister_req {
	s32 opcode;
	s32 chan_id;
	s32 destroy_containers;
	s32 tag1;
	s32 tag2;
};

struct app_unregister_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 tag1;
	s32 tag2;
};

struct superblock_copy_req {
	s32 opcode;
	s32 chan_id;
	s8 name[32];
	s32 tag1;
	s32 tag2;
};

struct superblock_copy_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	struct nvfuse_superblock_common superblock_common;
	s32 tag1;
	s32 tag2;
};

struct buffer_alloc_req {
	s32 opcode;
	s32 chan_id;
	s32 buffer_size; // requested buffer size in MB unit
	s32 tag1;
	s32 tag2;
};

struct buffer_alloc_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 buffer_size; // allocated buffer size in MB unit
	s32 tag1;
	s32 tag2;
};

struct buffer_free_req {
	s32 opcode;
	s32 chan_id;
	s32 buffer_size;
	s32 tag1;
	s32 tag2;
};

struct buffer_free_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 tag1;
	s32 tag2;
};

/* type of container allocation */

enum container_alloc_type {
	CONTAINER_NEW_ALLOC,
	CONTAINER_ALLOCATED_ALLOC
};

struct container_alloc_req {
	s32 opcode;
	s32 chan_id;
	s32 type;
	s32 tag1;
	s32 tag2;
};

struct container_alloc_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 tag1;
	s32 tag2;
};

struct container_release_req {
	s32 opcode;
	s32 chan_id;
	s32 container_id;
	s32 tag1;
	s32 tag2;
};

struct container_release_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 tag1;
	s32 tag2;
};

struct container_reservation_acquire_req {
	s32 opcode;
	s32 chan_id;
	s32 container_id;
	s32 reservation_type;
	s32 tag1;
	s32 tag2;
};

struct container_reservation_acquire_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 tag1;
	s32 tag2;
};

struct container_reservation_release_req {
	s32 opcode;
	s32 chan_id;
	s32 container_id;
	s32 tag1;
	s32 tag2;
};

struct container_reservation_release_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 tag1;
	s32 tag2;
};

struct health_check_req {
	s32 opcode;
	s32 chan_id;
	s32 tag1;
	s32 tag2;
};

struct health_check_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 tag1;
	s32 tag2;
};

struct unknown_cpl {
	s32 opcode;
	s32 chan_id;
	s32 ret;
	s32 tag1;
	s32 tag2;
};


union nvfuse_ipc_msg {
	struct {
		s32 opcode;
		s32 chan_id;
		s32 ret;
	};
	struct app_register_req app_register_req;
	struct app_register_cpl app_register_cpl;
	struct app_unregister_req app_unregister_req;
	struct app_unregister_cpl app_unregister_cpl;
	struct superblock_copy_req superblock_copy_req;
	struct superblock_copy_cpl superblock_copy_cpl;
	struct buffer_alloc_req buffer_alloc_req;
	struct buffer_alloc_cpl buffer_alloc_cpl;
	struct buffer_free_req buffer_free_req;
	struct buffer_free_cpl buffer_free_cpl;
	struct container_alloc_req container_alloc_req;
	struct container_alloc_cpl container_alloc_cpl;
	struct container_release_req container_release_req;
	struct container_release_cpl container_release_cpl;
	struct container_reservation_acquire_req container_reservation_acquire_req;
	struct container_reservation_acquire_cpl container_reservation_acquire_cpl;
	struct container_reservation_release_req container_reservation_release_req;
	struct container_reservation_release_cpl container_reservation_release_cpl;
	struct health_check_req health_check_req;
	struct health_check_cpl health_check_cpl;
	struct unknown_cpl unknown_cpl;
	s8 bytes[NVFUSE_IPC_MSG_SIZE];
};

enum reservation_status {
	UNLOCKED,
	ACQUIRED, /* */
	WRITE_LOCKED,
	READ_LOCKED,
};

enum reservation_type {
	RESV_READ_LOCK = 1,
	RESV_WRITE_LOCK = 2
};

struct container_reservation {
	s32 owner_core_id;
	s32 status;
	s32 ref_count;
};

/* perf stat relavent macros */
//#define _STAT_MSG_POOL "STAT_MSG_POOL"
//#define _STAT_RX_RING "STAT_RX_RING"

int nvfuse_ipc_init(struct nvfuse_ipc_context *ipc_ctx);
void nvfuse_ipc_exit(struct nvfuse_ipc_context *ipc_ctx);

int nvfuse_primary_ipc_poll(struct nvfuse_handle *nvh);
struct rte_ring *nvfuse_ipc_get_sendq(struct nvfuse_ipc_context *ipc_ctx, int chan_id);
struct rte_ring *nvfuse_ipc_get_recvq(struct nvfuse_ipc_context *ipc_ctx, int chan_id);
struct rte_mempool *nvfuse_ipc_mempool(struct nvfuse_ipc_context *ipc_ctx);
s8 *nvfuse_ipc_opcode_decode(enum ipc_opcode opcode);

void nvfuse_make_unkown_cpl(struct unknown_cpl *req, s32 ret);

void nvfuse_make_app_register_req(struct app_register_req *req);
void nvfuse_make_app_register_cpl(struct app_register_cpl *req, s32 ret);
void nvfuse_make_app_unregister_req(struct app_unregister_req *req);
void nvfuse_make_app_unregister_cpl(struct app_unregister_cpl *req, s32 ret);
void nvfuse_make_superblock_copy_req(struct superblock_copy_req *req);
void nvfuse_make_superblock_copy_cpl(struct superblock_copy_cpl *req, s32 ret);
void nvfuse_make_buffer_alloc_req(struct buffer_alloc_req *req);
void nvfuse_make_buffer_alloc_cpl(struct buffer_alloc_cpl *req, s32 ret);
void nvfuse_make_buffer_free_req(struct buffer_free_req *req);
void nvfuse_make_buffer_free_cpl(struct buffer_free_cpl *req, s32 ret);
void nvfuse_make_container_alloc_req(struct container_alloc_req *req);
void nvfuse_make_container_alloc_cpl(struct container_alloc_cpl *req, s32 ret);
void nvfuse_make_container_release_req(struct container_release_req *req);
void nvfuse_make_container_release_cpl(struct container_release_cpl *req, s32 ret);
void nvfuse_make_container_reservation_acquire_req(struct container_reservation_acquire_req *req);
void nvfuse_make_container_reservation_acquire_cpl(struct container_reservation_acquire_cpl *req,
		s32 ret);
void nvfuse_make_container_reservation_release_req(struct container_reservation_release_req *req);
void nvfuse_make_container_reservation_release_cpl(struct container_reservation_release_cpl *req,
		s32 ret);
void nvfuse_make_health_check_req(struct health_check_req *req);
void nvfuse_make_health_check_cpl(struct health_check_cpl *req, s32 ret);

void nvfuse_try_ring_dequeue(struct rte_ring *recv_ring, union nvfuse_ipc_msg **msg, int timeout);

int nvfuse_send_msg_to_primary_core(struct rte_ring *send_ring, struct rte_ring *recv_ring,
				    union nvfuse_ipc_msg *ipc_msg, s32 opcode);

s32 nvfuse_send_app_unregister_req(struct nvfuse_handle *nvh, s32 destroy_containers);
s32 nvfuse_send_alloc_buffer_req(struct nvfuse_handle *nvh, s32 buffer_size);
s32 nvfuse_send_dealloc_buffer_req(struct nvfuse_handle *nvh, s32 buffer_size);

int nvfuse_get_channel_id(struct nvfuse_ipc_context *ipc_ctx);
int nvfuse_put_channel_id(struct nvfuse_ipc_context *ipc_ctx, int channel);
int perf_stat_ring_create(struct rte_ring **stat_rx_ring, struct rte_mempool **stat_message_pool,
			  enum stat_type type);
int perf_stat_ring_lookup(struct rte_ring **stat_rx_ring, struct rte_mempool **stat_message_pool,
			  enum stat_type type);
int nvfuse_stat_ring_put(struct rte_ring *stat_tx_ring,
			 struct rte_mempool *stat_message_pool,
			 union perf_stat *stat);
void perf_stat_ring_free(struct rte_ring *stat_rx_ring, struct rte_mempool *stat_message_pool);

int nvfuse_stat_ring_get(struct rte_ring *stat_rx_ring,
			 struct rte_mempool *stat_message_pool,
			 union perf_stat *stat);
#endif
