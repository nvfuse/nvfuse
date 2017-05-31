/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 17/01/2017
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

#include "list.h"

#ifndef _CONTROL_PLANE_H
#define _CONTROL_PLANE_H

#define NAME_SIZE               16
#define MAX_CONTAINERS          4

struct app_manage_node {
	struct list_head list;
	s32 channel_id;
	s8 name[16];
	s32 root_bg_id; /* identify to container */
};

struct control_plane_context {
	/*
	* registered application table
	*/
	struct app_manage_node *app_manage_table;
	s32 app_table_size;
	s32 app_valid_count;
	s64 app_table_generation;
	s32 app_table_cur_log_file;
	s32 app_table_max_log_file;

	/*
	* container list table
	*/
	struct container_reservation *reservation_table;
	s32 reservation_table_size;
	s32 nr_containers;
	s32 free_containers;
	s32 last_alloc_container_id;

	s64 container_generation;
	s32 container_cur_log_file;
	s32 container_max_log_file;

	/* total buffer size and status */
	s32 total_buffer_size; // in pages
	s32 curr_buffer_size; // in pages

	s8 container_name[128];
	s8 container_log_name[128];
	s8 app_log_name[128];
};

struct nvfuse_handle;
struct nvfuse_superblock_common;

/* App Management Functions */
s32 nvfuse_app_manage_table_init();
s32 nvfuse_app_manage_table_add(struct nvfuse_handle *nvh, s32 core_id, s8 *name);
s32 nvfuse_app_manage_table_remove(struct nvfuse_handle *nvh, s32 core_id, s32 destroy_containers);
void nvfuse_app_manage_table_deinit(struct nvfuse_handle *nvh);
s32 nvfuse_store_app_table(struct nvfuse_handle *nvh);
s32 nvfuse_load_app_table(struct nvfuse_handle *nvh);

/* Superblock Copy Functions */
s32 nvfuse_superblock_copy(struct nvfuse_handle *nvh, s8 *appname,
			   struct superblock_copy_cpl *msg, struct nvfuse_superblock_common *sb_common);

/* Buffer Allocation Functions */
s32 nvfuse_control_plane_buffer_init(struct nvfuse_handle *nvh, s32 size);
s32 nvfuse_control_plane_buffer_alloc(struct nvfuse_handle *nvh, s32 size);
s32 nvfuse_control_plane_buffer_free(struct nvfuse_handle *nvh, s32 size);
void nvfuse_control_plane_buffer_deinit(struct nvfuse_handle *nvh);

/* Container Management Functions */
s32 nvfuse_control_plane_container_table_init(struct nvfuse_handle *nvh, s32 num_containers);
s32 nvfuse_control_plane_container_alloc(struct nvfuse_handle *nvh, s32 core_id, s32 type,
		s32 status);
s32 nvfuse_control_plane_container_release(struct nvfuse_handle *nvh, s32 core_id,
		s32 container_id);
void nvfuse_control_plane_container_table_deinit(struct nvfuse_handle *nvh);
s32 nvfuse_control_plane_container_release_by_coreid(struct nvfuse_handle *nvh, s32 core_id,
		s32 clear_owner);

s32 nvfuse_store_container_table(struct nvfuse_handle *nvh);
s32 nvfuse_load_container_table(struct nvfuse_handle *nvh);

/* Container Reservation Functions */
s32 nvfuse_control_plane_reservation_acquire(struct nvfuse_handle *nvh, s32 container_id,
		enum reservation_type type);
s32 nvfuse_control_plane_reservation_release(struct nvfuse_handle *nvh, s32 container_id);

/* Control Plane Init/Deinit Functions */
s32 nvfuse_control_plane_init(struct nvfuse_handle *nvh);
void nvfuse_control_plane_exit(struct nvfuse_handle *nvh);

#endif
