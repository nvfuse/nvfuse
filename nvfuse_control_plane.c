/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2017 Yongseok Oh <yongseok.oh@sk.com>
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

#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
//#define NDEBUG
#include <assert.h>

#include <rte_common.h>
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_lcore.h>

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_config.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_control_plane.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_mkfs.h"

#if NVFUSE_OS == NVFUSE_OS_LINUX
#include <libaio.h>
#	include <unistd.h>
#	include <sys/types.h>
#endif

/*
* global variable for control plane 
*/
#define LOG_CHUNK_SIZE 4096

s8 *get_container_name(struct nvfuse_handle *nvh, s8 *name)
{
	struct control_plane_context *cp;
	s8 *container_name;

	cp = nvh->nvh_sb.sb_control_plane_ctx;
	container_name = cp->container_name;

	sprintf(container_name, "container_%s", name);

	return container_name;
}

s8 *get_container_log_name(struct nvfuse_handle *nvh, s32 log_number)
{
	struct control_plane_context *cp;
	s8 *log_name;

	cp = nvh->nvh_sb.sb_control_plane_ctx;
	log_name = cp->container_log_name;

	sprintf(log_name, "container_table_%d.file", log_number);

	return log_name;
}

s8 *get_app_table_log_name(struct nvfuse_handle *nvh, s32 log_number)
{
	struct control_plane_context *cp;
	s8 *log_name;

	cp = nvh->nvh_sb.sb_control_plane_ctx;
	log_name = cp->app_log_name;
	
	sprintf(log_name, "app_table_%d.file", log_number);
	return log_name;
}

s32 nvfuse_app_manage_table_init(struct nvfuse_handle *nvh)
{
	struct control_plane_context *cp;
    int i;
    int size;

	cp = nvh->nvh_sb.sb_control_plane_ctx;
	/*
	* app management table
	*/
	cp->app_manage_table = NULL;
	cp->app_table_size = 0;
	cp->app_valid_count = 0;
	cp->app_table_generation = 1;
	cp->app_table_cur_log_file = 0;
	cp->app_table_max_log_file = 2;

    size = DIV_UP(sizeof(struct app_manage_node) * SPDK_NUM_CORES, CLUSTER_SIZE);
	cp->app_table_size = size;
	printf(" %s: App Table Size = %d \n", __FUNCTION__, size);
    cp->app_manage_table = nvfuse_alloc_aligned_buffer(size);
    if (cp->app_manage_table == NULL)
    {
	    fprintf(stderr," ERROR: malloc() \n");
	    return -1;
    }
    memset(cp->app_manage_table, 0x00, size);

    cp->app_valid_count = 0;
    for (i = 0;i < SPDK_NUM_CORES;i++) {
        struct app_manage_node *node;

        node = cp->app_manage_table + i;
        INIT_LIST_HEAD(&node->list);
        node->channel_id = -1;
    }

    return 0;    
}

s32 nvfuse_store_app_table(struct nvfuse_handle *nvh)
{
	struct control_plane_context *cp;
	s32 at_size;
	s32 offset = 0;
	s32 fd;
	s32 chunk_size = LOG_CHUNK_SIZE;
	s8 *buf;
	s8 *filename;
	s32 ret = 0;

	cp = nvh->nvh_sb.sb_control_plane_ctx;

	at_size = cp->app_table_size;	
	printf(" %s: App Table Size = %d bytes \n", __FUNCTION__, at_size);

	/* 4KB memory allocation */
	buf = nvfuse_alloc_aligned_buffer(chunk_size);
	if (buf == NULL) {
		printf(" Error: malloc() \n");
		ret = -1;
		goto RET;
	}

	filename = get_app_table_log_name(nvh, cp->app_table_cur_log_file);

	/* move ptr to next log */
	cp->app_table_cur_log_file = (cp->app_table_cur_log_file + 1) % cp->app_table_max_log_file;

	fd = nvfuse_openfile_path(nvh, filename, O_RDWR | O_CREAT, 0);
	if (fd < 0) {
		fprintf(stderr, " Error: file open = %s \n", filename);
		ret = -1;
		goto RET;
	}

	while (offset < at_size + chunk_size) {

		/* store generation number at the begining of file */
		if (offset == 0) { 
			s64 *p = (s64 *)buf;
			*p = cp->app_table_generation++;
		} else {
			s8 *p = (s8 *)cp->app_manage_table;
			memcpy(buf, p + offset - chunk_size, chunk_size); 
		}

		ret = nvfuse_writefile(nvh, fd, buf, chunk_size, offset);
		if (ret != chunk_size) {
			printf(" Error: file (%s) write() \n", filename);
			ret = -1;
			goto RET;
		}

		offset += chunk_size;
	}

#if 0	/* sync dirty data to SSD */
	nvfuse_fsync(fd);
#else	/* sync asynchronously  */
	nvfuse_check_flush_dirty(&nvh->nvh_sb, 0 /* delayed flush */);
#endif

	/* release memory */
	nvfuse_free_aligned_buffer(buf);
		
	/* close file */
	nvfuse_closefile(nvh, fd);
RET:

	return ret;
}

s32 nvfuse_load_app_table(struct nvfuse_handle *nvh)
{
	struct control_plane_context *cp;
	s8 *buf;
	s32 latest_log_file = 0;
	s64 max_generation = 0;
	s32 chunk_size = LOG_CHUNK_SIZE;
	s32 curr_log = 0;
	s8 *filename;
	s32 fd;
	s64 *curr_generation;
	s32 at_size;
	s32 offset = 0;
	s32 ret;

	cp = nvh->nvh_sb.sb_control_plane_ctx;

	/* 4KB memory allocation */
	buf = nvfuse_alloc_aligned_buffer(chunk_size);
	if (buf == NULL) {
		printf(" Error: malloc() \n");
		goto RET;
	}

	/* scan latest log file among log files */
	for (curr_log = 0; curr_log < cp->app_table_max_log_file; curr_log++) {
		filename = get_app_table_log_name(nvh, curr_log);
		fd = nvfuse_openfile_path(nvh, filename, O_RDONLY, 0);
		if (fd < 0) {
			fprintf(stderr, " Error: file open = %s. \n", filename);
			continue;
		}

		ret = nvfuse_readfile(nvh, fd, buf, chunk_size, 0);
		if (ret != chunk_size) {
			printf(" Error: file %s read().\n", filename);
			goto RET;
		}
		curr_generation = (s64 *)buf;
		if (*curr_generation > max_generation) {
			max_generation = *curr_generation;
			latest_log_file = curr_log;
		}
		/* close file */
		nvfuse_closefile(nvh, fd);
	}

	if (latest_log_file == 0)
	{
		fprintf(stdout, " app table log is not found. \n");
		return 0;
	}

	filename = get_app_table_log_name(nvh, latest_log_file);
	printf(" Latest log file = %s, generation = %ld\n", filename, max_generation);

	at_size = cp->app_table_size;
	printf(" App Table Size = %d bytes \n", at_size);

	/* copy file to app manage table */
	fd = nvfuse_openfile_path(nvh, filename, O_RDWR, 0);
	if (fd < 0) {
		fprintf(stderr, " Error: file open = %s. \n", filename);
		goto RET;
	}

	while (offset < at_size + chunk_size) {

		ret = nvfuse_readfile(nvh, fd, buf, chunk_size, offset);
		if (ret != chunk_size) {
			printf(" Error: file (%s) write() \n", filename);
			goto RET;
		}

		/* store generation number at the begining of file */
		if (offset == 0) { 
			s64 *p = (s64 *)buf;
			cp->app_table_generation = *p;
			cp->app_table_generation++;
		} else {
			s8 *p = (s8 *)cp->app_manage_table;
			memcpy(p + offset - chunk_size, buf, chunk_size); 
		}

		offset += chunk_size;
	}

	/* close file */
	nvfuse_closefile(nvh, fd);

	/* release memory */
	nvfuse_free_aligned_buffer(buf);
RET:
	return 0;
}

void nvfuse_app_manage_table_deinit(struct nvfuse_handle *nvh)
{
	nvfuse_store_app_table(nvh);
	nvfuse_free_aligned_buffer(nvh->nvh_sb.sb_control_plane_ctx->app_manage_table);
}

s32 nvfuse_store_container_table(struct nvfuse_handle *nvh)
{
	struct control_plane_context *cp;
	s32 ct_size;
	s32 offset = 0;
	s32 fd;
	s32 chunk_size = LOG_CHUNK_SIZE;
	s8 *buf;
	s8 *filename;
	s32 ret = 0;

	cp = nvh->nvh_sb.sb_control_plane_ctx;

	ct_size = cp->reservation_table_size;
	printf(" %s: Container Table Size = %d bytes \n", __FUNCTION__, ct_size);

	/* 4KB memory allocation */
	buf = nvfuse_alloc_aligned_buffer(chunk_size);
	if (buf == NULL) {
		printf(" Error: malloc() \n");
		ret = -1;
		goto RET;
	}

	filename = get_container_log_name(nvh, cp->container_cur_log_file);
	cp->container_cur_log_file = (cp->container_cur_log_file + 1) % cp->container_max_log_file;

	fd = nvfuse_openfile_path(nvh, filename, O_RDWR | O_CREAT, 0);
	if (fd < 0) {
		fprintf(stderr, " Error: file open = %s \n", filename);
		ret = -1;
		goto RET;
	}

	while (offset < ct_size + chunk_size) {

		/* store generation number at the begining of file */
		if (offset == 0) { 
			s64 *p = (s64 *)buf;
			*p = cp->container_generation++;
		} else {
			s8 *p = (s8 *)cp->reservation_table;
			memcpy(buf, p + offset - chunk_size, chunk_size); 
		}

		ret = nvfuse_writefile(nvh, fd, buf, chunk_size, offset);
		if (ret != chunk_size) {
			printf(" Error: file (%s) write() \n", filename);
			ret = -1;
			goto RET;
		}

		offset += chunk_size;
	}

#if 0	/* sync dirty data to SSD */
	nvfuse_fsync(fd);
#else	/* sync asynchronously  */
	nvfuse_check_flush_dirty(&nvh->nvh_sb, 0 /* delayed flush */);
#endif

	/* release memory */
	nvfuse_free_aligned_buffer(buf);
		
	/* close file */
	nvfuse_closefile(nvh, fd);
RET:

	return ret;
}

s32 nvfuse_load_container_table(struct nvfuse_handle *nvh)
{
	struct control_plane_context *cp;
	s8 *buf;
	s32 latest_log_file = 0;
	s64 max_generation = 0;
	s32 chunk_size = LOG_CHUNK_SIZE;
	s32 curr_log = 0;
	s8 *filename;
	s32 fd;
	s64 *curr_generation;
	s32 ct_size;
	s32 offset = 0;
	s32 ret;

	cp = nvh->nvh_sb.sb_control_plane_ctx;

	/* 4KB memory allocation */
	buf = nvfuse_alloc_aligned_buffer(chunk_size);
	if (buf == NULL) {
		printf(" Error: malloc() \n");
		goto RET;
	}

	/* scan latest log file among log files */
	for (curr_log = 0; curr_log < cp->container_max_log_file; curr_log++) {
		filename = get_container_log_name(nvh, curr_log);
		fd = nvfuse_openfile_path(nvh, filename, O_RDONLY, 0);
		if (fd < 0) {
			fprintf(stderr, " Error: file open = %s. \n", filename);
			continue;
		}

		ret = nvfuse_readfile(nvh, fd, buf, chunk_size, 0);
		if (ret != chunk_size) {
			printf(" Error: file (%s) read().\n", filename);
			goto RET;
		}
		curr_generation = (s64 *)buf;
		if (*curr_generation > max_generation) {
			max_generation = *curr_generation;
			latest_log_file = curr_log;
		}
		/* close file */
		nvfuse_closefile(nvh, fd);
	}

	if (latest_log_file == 0)
	{
		fprintf(stdout, " app table log is not found. \n");
		return 0;
	}

	filename = get_container_log_name(nvh, latest_log_file);
	printf(" Latest log file = %s, generation = %ld\n", filename, max_generation);

	ct_size = cp->reservation_table_size;
	printf(" Container Table Size = %d bytes \n", ct_size);

	/* copy file to reservation table */
	fd = nvfuse_openfile_path(nvh, filename, O_RDWR, 0);
	if (fd < 0) {
		fprintf(stderr, "Error: file open = %s. \n", filename);
		goto RET;
	}

	while (offset < ct_size + chunk_size) {

		ret = nvfuse_readfile(nvh, fd, buf, chunk_size, offset);
		if (ret != chunk_size) {
			printf(" Error: file write() \n");
			goto RET;
		}

		/* store generation number at the begining of file */
		if (offset == 0) { 
			s64 *p = (s64 *)buf;
			cp->container_generation = *p;
			cp->container_generation++;
		} else {
			s8 *p = (s8 *)cp->reservation_table;
			memcpy(p + offset - chunk_size, buf, chunk_size); 
		}

		offset += chunk_size;
	}

	/* close file */
	nvfuse_closefile(nvh, fd);

	/* release memory */
	nvfuse_free_aligned_buffer(buf);
RET:
	return 0;
}

struct app_manage_node *nvfuse_get_app_node_by_coreid(struct nvfuse_handle *nvh, s32 core_id)
{
	struct control_plane_context *cp;
	struct app_manage_node *node;

	cp = nvh->nvh_sb.sb_control_plane_ctx;
	node = cp->app_manage_table + core_id;
	return node;
}

s32 nvfuse_app_manage_table_add(struct nvfuse_handle *nvh, s32 channel_id, s8 *name)
{
	struct control_plane_context *cp;
	struct app_manage_node *node;
	struct stat st_buf;
	char *dir_name;
	s32 res;

	if (channel_id == 0 || channel_id >= SPDK_NUM_CORES)
	{
		fprintf(stderr, " Invalid core id = %d\n", channel_id);
		return -1;
	}

	cp = nvh->nvh_sb.sb_control_plane_ctx;
	
	node = nvfuse_get_app_node_by_coreid(nvh, channel_id);
	node->channel_id = channel_id;
	strcpy(node->name, name);
	
	/* the first container is reserved for the container management. */
	printf(" Add app: channel_id = %d name = %s root_seg = %d\n", channel_id, name, node->root_seg_id);

	dir_name = get_container_name(nvh, name);
	res = nvfuse_getattr(nvh, dir_name, &st_buf);
	if (res) /* if container isn't in FS, it will be created as a directory */
	{
		/* allocation of root container (e.g., segment) */
		node->root_seg_id = nvfuse_control_plane_container_alloc(nvh, channel_id, CONTAINER_NEW_ALLOC, UNLOCKED);

		/* work around: container directory and related objects to be located in allocated container */
		nvh->nvh_sb.sb_last_allocated_ino = node->root_seg_id * nvh->nvh_sb.sb_no_of_inodes_per_seg;
		printf(" Create container direcotory %s in container %d\n", dir_name, node->root_seg_id);
		res = nvfuse_mkdir_path(nvh, (const char *)dir_name, 0644);
		if (res < 0)
		{
			printf(" Error: create dir = %s \n", dir_name);
			return res;
		}
		/* reset */
		nvh->nvh_sb.sb_last_allocated_ino = 0;

		printf(" Add app: created dir = %s\n", dir_name);
	} 
	else
	{
		fprintf(stderr, " directory %s is already created.\n", dir_name);
	}

	return 0;
}

s32 nvfuse_destroy_containers_for_app_unregistration(struct nvfuse_handle *nvh, s32 core_id)
{
	struct control_plane_context *cp;
	struct container_reservation *cr;
	struct nvfuse_superblock *sb;
	struct nvfuse_buffer_head *dbitmap_bh, *ibitmap_bh, *ss_bh;
	struct nvfuse_segment_summary *ss;
	struct app_manage_node *node;

	char *dir_name;
	s32 container_id;
	s32 res;

	sb = &nvh->nvh_sb;
	cp = nvh->nvh_sb.sb_control_plane_ctx;

	node = nvfuse_get_app_node_by_coreid(nvh, core_id);

	/* rmdir container_yy */
	dir_name = get_container_name(nvh, node->name);
	res = nvfuse_rmdir_path(nvh, (const char *)dir_name);
	if (res < 0)
	{
		printf(" Error: rmdir = %s \n", dir_name);
		return res;
	}

	/* reset all bitmap tables of containers */
	for (container_id = 0; container_id < cp->nr_containers; container_id++)
	{
		cr = &cp->reservation_table[container_id];

		if (cr->owner_core_id != core_id)
			continue;

		ss_bh = nvfuse_get_bh(sb, NULL, SS_INO, container_id, READ, NVFUSE_TYPE_META);
		ss = (struct nvfuse_segment_summary *)ss_bh->bh_buf;

		/* format segment (container) summary with initial value */
		nvfuse_make_segment_summary(ss, container_id, 
				container_id * sb->sb_no_of_blocks_per_seg, sb->sb_no_of_blocks_per_seg);
		nvfuse_release_bh(sb, ss_bh, 0, DIRTY);

		/* clear data bitmap tables */
		dbitmap_bh = nvfuse_get_bh(sb, NULL, DBITMAP_INO, container_id, READ, NVFUSE_TYPE_META);
		memset(dbitmap_bh->bh_buf, 0x00, CLUSTER_SIZE);
		nvfuse_release_bh(sb, dbitmap_bh, 0, DIRTY);

		/* clear inode bitmap tables */
		ibitmap_bh = nvfuse_get_bh(sb, NULL, IBITMAP_INO, container_id, READ, NVFUSE_TYPE_META);
		memset(ibitmap_bh->bh_buf, 0x00, CLUSTER_SIZE);
		nvfuse_release_bh(sb, ibitmap_bh, 0, DIRTY);
		nvfuse_get_dirty_count(sb);

		nvfuse_check_flush_dirty(sb, 0 /* delayed flush */);
	}

	nvfuse_check_flush_dirty(sb, 1 /* force */);
}

s32 nvfuse_app_manage_table_remove(struct nvfuse_handle *nvh, s32 core_id, s32 destroy_containers)
{
	struct control_plane_context *cp;
	struct app_manage_node *node;
	s32 res;

	if (core_id == 0 || core_id >= SPDK_NUM_CORES)
	{
		fprintf(stderr, " Invalid core id = %d\n", core_id);
		return -1;
	}

	cp = nvh->nvh_sb.sb_control_plane_ctx;

	node = nvfuse_get_app_node_by_coreid(nvh, core_id);
	if (node->channel_id != core_id)
	{
		fprintf(stderr, " Core (%d) is not registered.\n", core_id);
		return -1;
	}

	if (destroy_containers == APP_UNREGISTER_WITH_DESTROYING_CONTAINERS)
	{
		fprintf(stderr, " Destroying containers when unregistering app is not supported\n");

		/* invalidate container summary, ibitmap table , and dbitmap table*/
		nvfuse_destroy_containers_for_app_unregistration(nvh, core_id);
		
		nvfuse_control_plane_container_release_by_coreid(nvh, core_id, 1 /* clear ownership */ );

		
		printf(" Remove app permanently: core_id = %d name = %s\n", core_id, node->name);

		node->channel_id = -1;
		memset(node->name, 0x0, NAME_SIZE);
		node->root_seg_id = 0;
	}
	else
	{
		/* status set to UNLOCKED */
		nvfuse_control_plane_container_release_by_coreid(nvh, core_id, 0 /* keep ownership */);

		printf(" Just un-register app: core_id = %d name = %s\n", core_id, node->name);
		printf(" Later, this information can be recovered through registration of app.\n");
	}

	/* logging container allocation table */
	nvfuse_store_container_table(nvh);

	/* logging app allocation table */
	nvfuse_store_app_table(nvh);

	return 0;
}

s32 nvfuse_superblock_copy(struct nvfuse_handle *nvh, s8 *appname, struct superblock_copy_cpl *msg, struct nvfuse_superblock_common *sb_common)
{
    struct nvfuse_dir_entry dir_entry;
    struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
    struct nvfuse_superblock *sb;
    char *path;
    char filename[128];
    int res;
    s32 container_root_ino;

    path = get_container_name(nvh, appname);

    res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
    if (res < 0)
    {
        printf(" No such superblock for container %s\n", appname);
        return -1;
    }
    
    sb = nvfuse_read_super(nvh);
    if(nvfuse_lookup(sb, NULL, &dir_entry, filename, dir_entry.d_ino) < 0){
       printf(" No such superblock for container %s\n", appname);
        return -1;
    }    
	nvfuse_release_super(sb);

    memcpy(&msg->superblock_common, sb_common, sizeof(struct nvfuse_superblock_common));
    
    msg->superblock_common.sb_root_ino = dir_entry.d_ino;
    msg->superblock_common.asb.asb_root_seg_id = dir_entry.d_ino / sb->sb_no_of_inodes_per_seg;

    msg->superblock_common.asb.asb_free_inodes = 0;
    msg->superblock_common.asb.asb_free_blocks = 0;
    msg->superblock_common.asb.asb_no_of_used_blocks = 0;    

    printf(" Container root inode = %d\n", dir_entry.d_ino);

    return 0;
}

s32 nvfuse_control_plane_buffer_init(struct nvfuse_handle *nvh, s32 size)
{
	struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;
    if (size == 0)
    {
        fprintf( stderr, " Invalid buffer size = %d pages.\n", size);
        return -1;
    }
    
    cp->curr_buffer_size = cp->total_buffer_size = size;

    return 0;
}

void nvfuse_control_plane_buffer_deinit(struct nvfuse_handle *nvh)
{

}

s32 nvfuse_control_plane_buffer_alloc(struct nvfuse_handle *nvh, s32 size)
{
	struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;
    s32 allocated_size;
    
    if (cp->curr_buffer_size < size)
    {
        fprintf( stderr, " buffers are not sufficient. \n");
        return 0;
    }
	
    cp->curr_buffer_size -= size;
    allocated_size = size;

	printf(" Remaining buffers = %.3f%%\n", 
		(double)cp->curr_buffer_size * 100 / cp->total_buffer_size);

    assert(allocated_size);

    return allocated_size;
}

s32 nvfuse_control_plane_buffer_free(struct nvfuse_handle *nvh, s32 size)
{
    struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;

	if (size == 0 || cp->curr_buffer_size + size > cp->total_buffer_size)
    {
        return -1;
    }

    cp->curr_buffer_size += size;
    assert(cp->curr_buffer_size <= cp->total_buffer_size);

	printf(" Remaining buffers = %.3f%%\n", 
		(double)cp->curr_buffer_size * 100 / cp->total_buffer_size);

    return 0;
}

s32 nvfuse_control_plane_container_table_init(struct nvfuse_handle *nvh, s32 num_containers)
{
	struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;
	s32 size;

	cp = nvh->nvh_sb.sb_control_plane_ctx;

	/*
	* container list table
	*/
	cp->reservation_table = NULL;
	cp->reservation_table_size = 0;
	cp->last_alloc_container_id = 0;

	cp->container_generation = 1;
	cp->container_cur_log_file = 0;
	cp->container_max_log_file = 2;


	cp->nr_containers = num_containers;
	cp->free_containers = num_containers;

	size = DIV_UP(sizeof(struct container_reservation) * cp->nr_containers, CLUSTER_SIZE);
	cp->reservation_table_size = size;
	printf(" %s: Container Table Size = %d \n", __FUNCTION__, size);

	cp->reservation_table = nvfuse_alloc_aligned_buffer(size);
	if (cp->reservation_table == NULL)
	{
		fprintf( stderr, " Error: malloc() \n");
		return -1;
	}

	memset(cp->reservation_table, 0x00, size);

	/* Reserved for control plane */
	cp->reservation_table[0].owner_core_id = ~0;
	cp->free_containers --;

#if 1 
	{
		int i;
		int count = 0; 

		for (i = 0;i < cp->nr_containers; i++)
		{
			if (cp->reservation_table[i].owner_core_id == 0)
			{
				count++;
			}
		}            
		assert(cp->free_containers == count);
		printf(" free_containers is validated = %d %d \n", cp->free_containers, count);
	}
#endif
	printf(" Primary process: init container table, total containers = %d\n", cp->nr_containers);

	return 0;
}

void nvfuse_pcontrin_plane_print_container_table(struct nvfuse_handle *nvh)
{
	struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;
	struct container_reservation *reservation_table = cp->reservation_table;
    int count = 0; 
	int i;

    for (i = 0;i < cp->nr_containers; i++)
    {
        if (reservation_table[i].owner_core_id == 0)
        {
            count++;
        }
        else
        {
            printf(" valid = %d, owner = %d, status = %d, ref = %d \n", i,
            reservation_table[i].owner_core_id,
            reservation_table[i].status,
            reservation_table[i].ref_count
            );
        }
    }
    printf(" free_containers is validated = %d %d \n", cp->free_containers, count);
    assert(cp->free_containers == count);
}

s32 nvfuse_control_plane_container_alloc(struct nvfuse_handle *nvh, s32 core_id, s32 type, s32 status)
{
    struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;
	struct container_reservation *reservation_table = cp->reservation_table;
	s32 container_id;

    assert(core_id != 0);

    if (type == CONTAINER_NEW_ALLOC)
    {
	    if (cp->free_containers == 0)
		    return 0;

	    container_id = (cp->last_alloc_container_id + 1) % cp->nr_containers;

	    while(reservation_table[container_id].owner_core_id != 0 ||
		 	  reservation_table[container_id].status != UNLOCKED) {
			container_id = (container_id + 1) % cp->nr_containers;
		}
	    
	    assert(container_id != 0);
	    assert(reservation_table[container_id].owner_core_id == 0);
		assert(reservation_table[container_id].status == UNLOCKED);

	    reservation_table[container_id].owner_core_id = core_id;
	    reservation_table[container_id].status = status;
	    reservation_table[container_id].ref_count = 0;
	    cp->free_containers--;
	    
	    cp->last_alloc_container_id = container_id;
    }
    else // type == CONTAINER_ALLOCATED_ALLOC
    {
	    s32 max_try_count = cp->nr_containers;

	    container_id = (cp->last_alloc_container_id + 1) % cp->nr_containers;
	    while(reservation_table[container_id].owner_core_id != core_id || 
			  reservation_table[container_id].status != UNLOCKED)
	    {
			container_id = (container_id + 1) % cp->nr_containers;
			if (--max_try_count == 0)
			{				
				container_id = 0;
				printf(" No such containers for core %d\n\n", core_id);
				goto RET;
			}
	    }	    

	    assert(container_id != 0);
	    assert(reservation_table[container_id].owner_core_id == core_id);
		assert(reservation_table[container_id].status == UNLOCKED);
	    
	    reservation_table[container_id].status = status;
	    reservation_table[container_id].ref_count = 0;	    
	    
	    cp->last_alloc_container_id = container_id;
    }

    //fprintf( stdout, " allocated container id = %d, remained containers = %d \n", container_id, free_containers);
RET:

    return container_id;
}

s32 nvfuse_control_plane_container_release(struct nvfuse_handle *nvh, s32 core_id, s32 container_id)
{
	struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;
	struct container_reservation *reservation_table = cp->reservation_table;

	if (reservation_table[container_id].owner_core_id != core_id)
	{
		fprintf(stderr, " ERROR: owner does not match %d\n", core_id);
		return -1;
	}

	if (reservation_table[container_id].ref_count)
	{
		fprintf(stderr, " ERROR: container is reserved by other cores (refcount = %d).\n", 
				reservation_table[container_id].ref_count);
		return -1;
	}

	reservation_table[container_id].owner_core_id = 0;
	reservation_table[container_id].status = UNLOCKED;
	reservation_table[container_id].ref_count = 0;
	cp->free_containers++;

	/* logging container allocation table */
	nvfuse_store_container_table(nvh);

	return 0;
}

s32 nvfuse_control_plane_container_release_by_coreid(struct nvfuse_handle *nvh, s32 core_id, s32 clear_owner)
{
	struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;
	struct container_reservation *reservation_table = cp->reservation_table;
	s32 container_id;

	for (container_id = 1; container_id < cp->nr_containers; container_id++)
	{
		if (reservation_table[container_id].owner_core_id != core_id)
			continue;

		if (reservation_table[container_id].ref_count)
		{
			fprintf(stderr, " ERROR: container is reserved by other cores (refcount = %d).\n", 
					reservation_table[container_id].ref_count);
			return -1;
		}

		if (clear_owner)
			reservation_table[container_id].owner_core_id = 0;

		reservation_table[container_id].status = UNLOCKED;
		reservation_table[container_id].ref_count = 0;

		if (clear_owner)
			cp->free_containers++;
	}

	return 0;
}

s32 nvfuse_control_plane_reservation_acquire(struct nvfuse_handle *nvh, s32 container_id, enum reservation_type type)
{
	struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;
	struct container_reservation *reservation_table = cp->reservation_table;

    switch (type)
    {
        case RESV_WRITE_LOCK:
            if (reservation_table[container_id].status == UNLOCK)
            {
                reservation_table[container_id].status = WRITE_LOCKED;
                reservation_table[container_id].ref_count++;
            }
            else
            {
                fprintf(stderr, " Error: current container (%d) is locked with %d\n", container_id, 
                                    reservation_table[container_id].status);
                return -1;
            }
            break;
        case RESV_READ_LOCK:
            if (reservation_table[container_id].status == READ_LOCKED || 
                reservation_table[container_id].status == UNLOCKED)
            {
                reservation_table[container_id].status = READ_LOCKED;
                reservation_table[container_id].ref_count++;
            }
            else
            {
                fprintf(stderr, " Error: current container (%d) is locked with %d\n", container_id, 
                                    reservation_table[container_id].status);
                return -1;
            }
            
            break;
        default:    
            fprintf( stderr, " Invalid reservation type = %d \n", type);
            return -1;
    }

    return 0;
}

s32 nvfuse_control_plane_reservation_release(struct nvfuse_handle *nvh, s32 container_id)
{
	struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;
	struct container_reservation *reservation_table = cp->reservation_table;

    switch (reservation_table[container_id].status)
    {
        case WRITE_LOCKED:
            reservation_table[container_id].status = UNLOCKED;
            reservation_table[container_id].ref_count--;
            assert(reservation_table[container_id].ref_count == 0);
            break;
        case READ_LOCKED:            
            reservation_table[container_id].ref_count--;
            if (reservation_table[container_id].ref_count == 0)
                reservation_table[container_id].status = UNLOCKED;
            break;
        default:    
            fprintf( stderr, " Invalid reservation status = %d \n", 
            reservation_table[container_id].status);
            return -1;
    }
    
    return 0;
}

s32 nvfuse_control_plane_health_check(s32 core_id)
{

    return 0;
}

void nvfuse_control_plane_container_table_deinit(struct nvfuse_handle *nvh)
{
	struct control_plane_context *cp = nvh->nvh_sb.sb_control_plane_ctx;

	nvfuse_store_container_table(nvh);
	nvfuse_free_aligned_buffer(cp->reservation_table); 
}

s32 nvfuse_control_plane_init(struct nvfuse_handle *nvh)
{
    struct control_plane_context *cp;
	s32 nr_buffers = nvh->nvh_sb.sb_control_plane_buffer_size;
	s32 nr_segments = nvh->nvh_sb.sb_segment_num;
	s32 ret = 0; 

	cp = nvfuse_malloc(sizeof(struct control_plane_context));
	if (cp == NULL)
		return -1;
	
	nvh->nvh_sb.sb_control_plane_ctx = cp;

    ret = nvfuse_app_manage_table_init(nvh);
    if (ret < 0)
    {
        fprintf(stderr, " Error: app_manage_table_init()\n");
    }

    ret = nvfuse_control_plane_buffer_init(nvh, nr_buffers);
    if (ret < 0)
    {
        fprintf(stderr, " Error: control_plane_buffer_init()\n");
    }

	ret = nvfuse_control_plane_container_table_init(nvh, nr_segments); /* excluding 0 container */
    if (ret < 0)
    {
        fprintf(stderr, " Error: control_plane_container_table_init()\n");
    }
    return ret;
}

void nvfuse_control_plane_exit(struct nvfuse_handle *nvh)
{
    nvfuse_control_plane_container_table_deinit(nvh);
    nvfuse_control_plane_buffer_deinit(nvh);
    nvfuse_app_manage_table_deinit(nvh);
	/* free memory */
	nvfuse_free(nvh->nvh_sb.sb_control_plane_ctx);
}
