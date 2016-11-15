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
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>

#include "nvfuse_config.h"
#if NVFUSE_OS == NVFUSE_OS_LINUX
#include <dirent.h>
#include <sys/uio.h>
#endif 

#include "nvfuse_core.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_indirect.h"
#include "nvfuse_bp_tree.h"
#include "nvfuse_malloc.h"
#include "nvfuse_api.h"
#include "nvfuse_dirhash.h"

struct nvfuse_handle *nvfuse_create_handle(struct nvfuse_handle *a_nvh, s32 init_iom, s32 io_manager_type, s32 need_format, s32 need_mount)
{
	struct nvfuse_handle *nvh;
	struct nvfuse_io_manager *io_manager;
	s32 ret;

	if (a_nvh == NULL) 
	{
		nvh = malloc(sizeof(struct nvfuse_handle));
		if (nvh == NULL)
		{
			printf(" Error: nvfuse_create_handle due to lack of free memory \n");
			return NULL;
		}
		memset(nvh, 0x00, sizeof(struct nvfuse_handle));
	}
	else 
	{
		nvh = a_nvh;
	}

	/* Initialize IO Manager */
	if (init_iom) {
		io_manager = &nvh->nvh_iom;
		switch (io_manager_type)
		{
#if NVFUSE_OS == NVFUSE_OS_WINDOWS
		case IO_MANAGER_RAMDISK:
			nvfuse_init_memio(io_manager, "RANDISK", "RAM");
			break;
		case IO_MANAGER_FILEDISK:
			nvfuse_init_fileio(io_manager, "FILE", DISK_FILE_PATH);
			break;
#endif
#if NVFUSE_OS == NVFUSE_OS_LINUX
		case IO_MANAGER_UNIXIO:
			nvfuse_init_unixio(io_manager, "SSD", "/dev/sdb", AIO_MAX_QDEPTH);
			break;
#	ifdef SPDK_ENABLED
		case IO_MANAGER_SPDK:
			nvfuse_init_spdk(io_manager, "SPDK", "spdk:namespace0", AIO_MAX_QDEPTH);
			break;
#	endif
#endif
		default:
			printf(" Error: Invalid IO Manager Type = %d", io_manager_type);
			return NULL;
		}
		/* open I/O manager */
		io_manager->io_open(io_manager, 0);
		printf(" total blks = %ld \n", (long)io_manager->total_blkcount);
	}
	
	/* file system format */
	if (need_format)
	{
		ret = nvfuse_format(nvh);
		if (ret < 0) {
			printf(" Error: format() \n");
			return NULL;
		}
	}
	
	/* file system mount */
	if (need_mount) {
		ret = nvfuse_mount(nvh);
		if (ret < 0) {
			printf(" Error: mount() \n");
			return NULL;
		}
	}	
	return nvh;
}

void nvfuse_destroy_handle(struct nvfuse_handle *nvh, s32 deinit_iom, s32 need_umount)
{
	struct nvfuse_io_manager *io_manager;
	int ret;

	io_manager = &nvh->nvh_iom;

	/* umount file system */
	if (need_umount) 
	{
		ret = nvfuse_umount(nvh);
		if (ret < 0) {
			printf(" Error: umount() \n");
		}
	}
	
	/* close I/O manager */
	if (deinit_iom)
	{
		io_manager->io_close(io_manager);
	}
	
}
/* 
* namespace lookup function with given file name and paraent inode number
* result: found=0, not found=-1 
*/
s32 nvfuse_lookup(struct nvfuse_superblock *sb, 
		struct nvfuse_inode **file_inode, 
		struct nvfuse_dir_entry *file_entry, 
		s8 *filename, 
		s32 cur_dir_ino) 
{
	struct nvfuse_inode *dir_inode = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_dir_entry *dir;
	u32 read_bytes = 0;
	u32 dir_size = 0;
	s32 start = 0;
	u32 offset = 0;
	s32 res = -1;

	dir_inode = nvfuse_read_inode(sb, cur_dir_ino, READ);
	if (dir_inode == NULL)
		return res;

#if NVFUSE_USE_DIR_INDEXING == 1	
	res = nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset);
	if (res < 0) {
		goto RES;
	}
#endif 

	res = -1;

	dir_size = dir_inode->i_size;
	start = offset * DIR_ENTRY_SIZE;

	if ((start & (CLUSTER_SIZE - 1))) {
		dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, start / CLUSTER_SIZE, READ);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir += (offset % DIR_ENTRY_NUM);
	}

	for (read_bytes = start; read_bytes < dir_size; read_bytes += DIR_ENTRY_SIZE) {

		if (!(read_bytes&(CLUSTER_SIZE - 1))) {
			if (dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0, 0);
			dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, read_bytes >> CLUSTER_SIZE_BITS, READ);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED) {
			if (!strcmp(dir->d_filename, filename)) {
				if (file_inode) {
					*file_inode = nvfuse_read_inode(sb, dir->d_ino, READ);
				}
				if (file_entry) {
					memcpy(file_entry, dir, DIR_ENTRY_SIZE);
				}

				res = 0;

				goto RES;
			}
		}
		dir++;
	}

RES:;

	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, 0);
	nvfuse_release_bh(sb, dir_bh, 0, 0);

	return res;
}

inode_t nvfuse_get_root_ino(struct nvfuse_handle *nvh)
{
	return nvh->nvh_root_ino;
}

void nvfuse_set_root_ino(struct nvfuse_handle *nvh, inode_t root_ino)
{
	nvh->nvh_root_ino = root_ino;
}

inode_t nvfuse_get_cwd_ino(struct nvfuse_handle *nvh)
{
	return nvh->nvh_cwd_ino;
}

void nvfuse_set_cwd_ino(struct nvfuse_handle *nvh, inode_t cwd_ino)
{
	nvh->nvh_cwd_ino = cwd_ino;
}

s32 nvfuse_path_resolve(struct nvfuse_handle *nvh, const char *path, char *filename, struct nvfuse_dir_entry *direntry)
{
	s8 dirname[FNAME_SIZE];
	int res;	

	if (path[0] == '/') {
		nvfuse_filename(path, filename);
		nvfuse_dirname(path, dirname);
		res = nvfuse_path_open(nvh, dirname, filename, direntry);
	} else {
		nvfuse_filename(path, filename);
		res = nvfuse_path_open2(nvh, (s8 *)path, (s8 *)filename, direntry);
	}
	
	return res;
}

s32 nvfuse_opendir(struct nvfuse_handle *nvh, const char *path)
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	struct nvfuse_dir_entry de;
	struct nvfuse_inode *inode;
	struct nvfuse_dir_entry dir_entry;
	unsigned int par_ino;	
	int res;
	s8 *filename;	
		
	filename = (s8 *)nvfuse_malloc(CLUSTER_SIZE);
	if (filename == NULL) {
		printf(" nvfuse_malloc error \n");
		return -1;
	}

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	par_ino = dir_entry.d_ino;

	if (strcmp(filename,"")) {
		if(nvfuse_lookup(sb, NULL, &dir_entry, filename, dir_entry.d_ino)< 0)
		{
			return NVFUSE_ERROR;
		}
		par_ino = dir_entry.d_ino;
	}
		
	nvfuse_free(filename);

	return par_ino;
}

struct dirent *nvfuse_readdir(struct nvfuse_handle *nvh, inode_t par_ino, struct dirent *dentry, off_t dir_offset) 
{
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_buffer_head *dir_bh;
	struct nvfuse_dir_entry *dir;
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s32 read_bytes;
	u32 dir_size;
	s32 res = 0;
	struct dirent *return_dentry = NULL;

	dir_inode = nvfuse_read_inode(sb, par_ino, READ);
	dir_size = dir_offset * DIR_ENTRY_SIZE;

	if (dir_inode->i_size <= (dir_size)) {
		res = NVFUSE_ERROR;
		goto RES;
	}

	dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, (dir_offset * DIR_ENTRY_SIZE) >> CLUSTER_SIZE_BITS, READ);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;

	dir += (dir_offset % (CLUSTER_SIZE / DIR_ENTRY_SIZE));

	if (nvfuse_dir_is_invalid(dir)) {
		return_dentry = NULL;
	} else {
		dentry->d_ino = dir->d_ino;
		strcpy(dentry->d_name, dir->d_filename);

		inode = nvfuse_read_inode(sb, dir->d_ino, 0);
		if (inode->i_type == NVFUSE_TYPE_DIRECTORY) 
			dentry->d_type = DT_DIR;
		else 
			dentry->d_type = DT_REG;

		nvfuse_relocate_write_inode(sb, inode, inode->i_ino, CLEAN);

		return_dentry = dentry;
	}

	nvfuse_release_bh(sb, dir_bh, 0, 0);
RES:;

	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, CLEAN);
	nvfuse_release_super(sb);

	return return_dentry;
}

s32 nvfuse_openfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, s32 flags, s32 mode) 
{
	struct nvfuse_dir_entry dir_temp;
	struct nvfuse_inode *inode;	
	struct nvfuse_file_table *ft;
	s32 fid = -1;
	s32 res = 0;

	if (!strcmp(filename, ""))
		return error_msg("open file error, invalid file name");

	res = nvfuse_lookup(sb, NULL, &dir_temp, filename, par_ino);
	if (res < 0) { // no such file 
		if (flags & O_RDWR || flags & O_CREAT) {
			res = nvfuse_createfile(sb, par_ino, filename, 0, mode);
			if (res == NVFUSE_SUCCESS) {
				res = nvfuse_lookup(sb, NULL, &dir_temp, filename, par_ino);
			}
		}
		else {
			res = nvfuse_lookup(sb, NULL, &dir_temp, filename, par_ino);
			printf("cannot read or create file \n");
			fid = -1;
			goto RES;
		}
	}

	inode = nvfuse_read_inode(sb, dir_temp.d_ino, READ);
	if (inode->i_type != NVFUSE_TYPE_FILE) {
		inode = nvfuse_read_inode(sb, dir_temp.d_ino, READ);
		printf("This is not a file");
		fid = -1;
		goto RES;
	}

	fid = nvfuse_allocate_open_file_table(sb);
	if (fid<0) {
		goto RES;
	}

	ft = sb->sb_file_table + fid;

	pthread_mutex_lock(&ft->ft_lock);
	ft->used = TRUE;
	ft->ino = dir_temp.d_ino;
	ft->size = inode->i_size;
	ft->rwoffset = 0;

	if (O_APPEND & flags)
		nvfuse_seek(sb, ft, inode->i_size, SEEK_SET);
	else
		nvfuse_seek(sb, ft, 0, SEEK_SET);
	pthread_mutex_unlock(&ft->ft_lock);

	nvfuse_relocate_write_inode(sb, inode, inode->i_ino, 0 /*clean*/);

RES:;
	nvfuse_release_super(sb);

	return (fid);
}

s32 nvfuse_openfile_path(struct nvfuse_handle *nvh, const char *path, int flags, int mode)
{
	int fd;
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_superblock *sb;
	s8 filename[FNAME_SIZE];
	s32 res;

	memset(&dir_entry, 0x00, sizeof(struct nvfuse_dir_entry));

	nvfuse_lock();

	sb = nvfuse_read_super(nvh);
	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf("invalid path\n");
		fd = -1;
	} else {
		fd = nvfuse_openfile(sb, dir_entry.d_ino, filename, flags, mode);
	}
		
	nvfuse_release_super(sb);
	nvfuse_unlock();

	return fd;
}

s32 nvfuse_openfile_ino(struct nvfuse_superblock *sb, inode_t ino, s32 flags)
{
	struct nvfuse_dir_entry dir_temp;
	struct nvfuse_inode *inode;
	struct nvfuse_file_table *ft;
	s32 fid = -1;
	s32 res = 0;

	fid = nvfuse_allocate_open_file_table(sb);
	if (fid<0) {
		printf(" debug ");
		return NVFUSE_ERROR;
	}

	inode = nvfuse_read_inode(sb, ino, READ);

	if (inode->i_type != NVFUSE_TYPE_FILE) {
		return error_msg("This is not a file");
	}

	ft = sb->sb_file_table + fid;
	pthread_mutex_lock(&ft->ft_lock);
	ft->used = TRUE;
	ft->ino = ino;
	ft->size = inode->i_size;
	//	ft->status = inode->i_type;
	ft->rwoffset = 0;

	if (O_APPEND & flags)
		nvfuse_seek(sb, ft, inode->i_size, SEEK_SET);
	else
		nvfuse_seek(sb, ft, 0, SEEK_SET);

	pthread_mutex_unlock(&ft->ft_lock);

	nvfuse_relocate_write_inode(sb, inode, inode->i_ino, CLEAN);
	return(fid);
}

s32 nvfuse_closefile(struct nvfuse_handle *nvh, s32 fid) 
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	struct nvfuse_file_table *ft;

	pthread_mutex_lock(&sb->sb_file_table_lock);
	ft = sb->sb_file_table + fid;

	pthread_mutex_lock(&ft->ft_lock);
	//	ft->status = 0;
	ft->ino = 0;
	//ft->attr = 0;
	//ft->date = 0;
	ft->size = 0;
	ft->used = 0;
	ft->rwoffset = 0;
	ft->prefetch_cur = 0;
	pthread_mutex_unlock(&ft->ft_lock);

	pthread_mutex_unlock(&sb->sb_file_table_lock);

	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_readfile(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset) 
{
	struct nvfuse_inode *inode;
	struct nvfuse_buffer_head *bh;
	struct nvfuse_file_table *of;
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s32  i, offset, remain, rcount = 0;
	s32 ra = 0;

	of = &(sb->sb_file_table[fid]);
	pthread_mutex_lock(&of->ft_lock);

	inode = nvfuse_read_inode(sb, of->ino, READ);

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
	if (roffset) {
		of->rwoffset = roffset;
	}
#else
	of->rwoffset = roffset;
#endif
	
	while (count > 0 && of->rwoffset < inode->i_size) {

		bh = nvfuse_get_bh(sb, inode->i_ino, of->rwoffset >> CLUSTER_SIZE_BITS, READ);
		if (bh == NULL) {
			printf(" read error \n");
			goto RES;
		}

		offset = of->rwoffset & (CLUSTER_SIZE - 1);
		remain = CLUSTER_SIZE - offset;

		if (remain > count)
			remain = count;

		memcpy(buffer + rcount, &bh->bh_buf[offset], remain);

		rcount += remain;
		of->rwoffset += remain;
		count -= remain;
		nvfuse_release_bh(sb, bh, 0, 0);
	}

	nvfuse_relocate_write_inode(sb, inode, inode->i_ino, 0/*clean*/);
	nvfuse_release_super(sb);

	pthread_mutex_unlock(&of->ft_lock);

	if (ra) {
		struct timeval start, end, result;

		/* send prefetch signal*/
		pthread_mutex_lock(&sb->sb_prefetch_lock);
		sb->sb_prefetch_cur = fid;
		pthread_cond_signal(&sb->sb_prefetch_cond);
		pthread_mutex_unlock(&sb->sb_prefetch_lock);
	}
RES:

	return rcount;
}

s32 nvfuse_writefile(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf, u32 count, nvfuse_off_t woffset) 
{
	struct nvfuse_inode *inode;
	struct nvfuse_file_table *of;
	struct nvfuse_buffer_head *bh = NULL;
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s32 offset = 0, remain = 0, wcount = 0;
	lbno_t lblock = 0;
	int ret;

	of = &(sb->sb_file_table[fid]);
	pthread_mutex_lock(&of->ft_lock);

#if NVFUSE_OS == NVFUSE_OS_WINDOWS 
	if (woffset) {
		of->rwoffset = woffset;
	}
#else
	of->rwoffset = woffset;
#endif

	while (count > 0) {

		inode = nvfuse_read_inode(sb, of->ino, WRITE);

		lblock = of->rwoffset >> CLUSTER_SIZE_BITS;
		offset = of->rwoffset & (CLUSTER_SIZE - 1);
		remain = CLUSTER_SIZE - offset;
		if (remain > count)
			remain = count;

		if (count && inode->i_size <= of->rwoffset)
		{
			ret = nvfuse_get_block(sb, inode, inode->i_size >> CLUSTER_SIZE_BITS, NULL, 1);
			if (ret)
			{
				printf(" data block allocation fails.");
				return NVFUSE_ERROR;
			}
		}

		/*read modify write or partial write */
		if (remain != CLUSTER_SIZE)
			bh = nvfuse_get_bh(sb, inode->i_ino, lblock, READ);
		else
			bh = nvfuse_get_bh(sb, inode->i_ino, lblock, WRITE);

		memcpy(&bh->bh_buf[offset], user_buf + wcount, remain);

		wcount += remain;
		of->rwoffset += remain;
		count -= remain;

		if (of->rwoffset > of->size)
			of->size = of->rwoffset;

		inode->i_type = NVFUSE_TYPE_FILE;
		inode->i_size = of->size;

		nvfuse_relocate_write_inode(sb, inode, inode->i_ino, DIRTY);
		nvfuse_release_bh(sb, bh, 0, DIRTY);
	}

	pthread_mutex_unlock(&of->ft_lock);
	nvfuse_release_super(sb);

	nvfuse_check_flush_segment(sb);

	return wcount;
}

s32 nvfuse_createfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *str, inode_t *new_ino, mode_t mode) 
{
	struct nvfuse_dir_entry *dir;
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode *new_inode, *dir_inode;
	struct nvfuse_buffer_head *dir_bh = NULL;
	s32 j = 0, i = 0;
	s32 new_entry = 0, flag = 0;
	s32 search_lblock = 0, search_entry = 0;
	s32 offset;
	s32 dir_num;
	s32 num_block;
	u32 dir_hash;

	if (strlen(str) < 1 || strlen(str) >= FNAME_SIZE) {
		printf("create file : name  = %s, %d\n", str, (int)strlen(str));
		return -1;
	}

	if (!nvfuse_lookup(sb, NULL, NULL, str, par_ino))
		return error_msg(" exist file or directory\n");

	dir_inode = nvfuse_read_inode(sb, par_ino, WRITE);

	search_lblock = (dir_inode->i_links_count - 1) / DIR_ENTRY_NUM;
	search_entry = (dir_inode->i_links_count - 1) % DIR_ENTRY_NUM;

retry:

	dir_num = (dir_inode->i_size / DIR_ENTRY_SIZE);
	if (dir_num == dir_inode->i_links_count) {
		search_entry = -1;
		num_block = 0;
	}
	else {
		if (search_entry == DIR_ENTRY_NUM - 1) {
			search_entry = -1;
		}
		num_block = dir_num / DIR_ENTRY_NUM;
	}

	// find an empty dentry
	for (i = 0; i < num_block; i++) {
		dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, search_lblock, READ);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;

		for (new_entry = 0; new_entry < DIR_ENTRY_NUM; new_entry++) {
			search_entry++;
			if (search_entry == DIR_ENTRY_NUM) {
				search_entry = 0;
			}
						
			if (nvfuse_dir_is_invalid(dir + search_entry)) {
				flag = 1;
				dir_inode->i_ptr = search_lblock * DIR_ENTRY_NUM + search_entry;
				dir_inode->i_links_count++;
				goto find;
			}
		}
		nvfuse_release_bh(sb, dir_bh, 0, 0);
		dir_bh = NULL;
		search_entry = -1;
		search_lblock++;
		if (search_lblock == dir_inode->i_size >> CLUSTER_SIZE_BITS)
			search_lblock = 0;
	}

	dir_num = (dir_inode->i_size / DIR_ENTRY_SIZE);
	num_block = dir_num / DIR_ENTRY_NUM;
	search_lblock = num_block;

	if (!flag) // allocate new direcct block 
	{
		nvfuse_release_bh(sb, dir_bh, 0, 0);
		nvfuse_get_block(sb, dir_inode, dir_inode->i_size >> CLUSTER_SIZE_BITS, NULL, 1);
		dir_bh = nvfuse_get_new_bh(sb, dir_inode->i_ino, dir_inode->i_size >> CLUSTER_SIZE_BITS);
		nvfuse_release_bh(sb, dir_bh, INSERT_HEAD, dir_bh->bh_dirty);
		dir_inode->i_size += CLUSTER_SIZE;
		goto retry;
	}

find:

	assert(dir_inode->i_links_count == dir_inode->i_ptr + 1);

	new_inode = nvfuse_alloc_new_inode(sb);
	new_inode->i_type = NVFUSE_TYPE_FILE;
	new_inode->i_size = 0;
	new_inode->i_mode = mode;
	new_inode->i_gid = 0;
	new_inode->i_uid = 0;
	new_inode->i_links_count = 1;
	new_inode->i_atime = time(NULL);
	new_inode->i_ctime = time(NULL);
	new_inode->i_mtime = time(NULL);

	if (new_ino)
		*new_ino = new_inode->i_ino;

	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
	dir[search_entry].d_flag = DIR_USED;
	dir[search_entry].d_ino = new_inode->i_ino;
	dir[search_entry].d_version = new_inode->i_version;
	strcpy(dir[search_entry].d_filename, str);
	
#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_set_dir_indexing(sb, dir_inode, str, dir_inode->i_ptr);
#endif 

	nvfuse_relocate_write_inode(sb, new_inode, new_inode->i_ino, DIRTY);
	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, DIRTY);
	nvfuse_release_bh(sb, dir_bh, 0/*tail*/, DIRTY);
	nvfuse_check_flush_segment(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_shrink_dentry(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, u32 to_entry, u32 from_entry)
{
	struct nvfuse_buffer_head *dir_bh_from;
	struct nvfuse_dir_entry *dir_from;

	struct nvfuse_buffer_head *dir_bh_to;
	struct nvfuse_dir_entry *dir_to;

	if (to_entry == from_entry) 
		return -1;
		
	assert(to_entry < from_entry);

	dir_bh_from = nvfuse_get_bh(sb, inode->i_ino, from_entry * DIR_ENTRY_SIZE / CLUSTER_SIZE, READ);
	dir_from = (struct nvfuse_dir_entry *)dir_bh_from->bh_buf;
	dir_from += (from_entry % DIR_ENTRY_NUM);	
	assert(dir_from->d_flag != DIR_DELETED);
	
	dir_bh_to = nvfuse_get_bh(sb, inode->i_ino, to_entry * DIR_ENTRY_SIZE / CLUSTER_SIZE, READ);
	dir_to = (struct nvfuse_dir_entry *)dir_bh_to->bh_buf;
	dir_to += (to_entry % DIR_ENTRY_NUM);
	assert(dir_to->d_flag == DIR_DELETED);

	memcpy(dir_to, dir_from, DIR_ENTRY_SIZE);
	
	dir_from->d_flag = DIR_DELETED; /* FIXME: zeroing ?*/
	//printf(" shrink_dentry: deleted = %s \n", dir_from->d_filename);

	nvfuse_release_bh(sb, dir_bh_from, 0, DIRTY);
	nvfuse_release_bh(sb, dir_bh_to, 0, DIRTY);

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_del_dir_indexing(sb, inode, dir_to->d_filename);
	nvfuse_set_dir_indexing(sb, inode, dir_to->d_filename, to_entry);
	
	/*{
		u32 offset;
		nvfuse_get_dir_indexing(sb, inode, dir_to->d_filename, &offset);
		if (offset != to_entry) {
			printf(" b+tree inconsistency \n");
		}
	}*/
#endif 	

	return 0;
}

s32 nvfuse_rmfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename)
{
	struct nvfuse_inode *dir_inode, *inode = NULL;
	struct nvfuse_dir_entry *dir;
	struct nvfuse_buffer_head *dir_bh = NULL;
	
	u32 read_bytes = 0;
	lbno_t lblock = 0;
	u32 start = 0;
	u32 offset = 0;
	u64 dir_size = 0;
	u32 found_entry;
	
	dir_inode = nvfuse_read_inode(sb, par_ino, WRITE);
	dir_size = dir_inode->i_size;

	if (!strcmp(filename, "9999")) {
		dir_size = dir_inode->i_size;
	}
#if NVFUSE_USE_DIR_INDEXING == 1
	if (nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset) < 0) {
		printf(" fixme: filename (%s) is not in the index.\n", filename);
		offset = 0;
	}
#endif

	start = offset * DIR_ENTRY_SIZE;
	if ((start & (CLUSTER_SIZE - 1))) {
		dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, start / CLUSTER_SIZE, READ);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir += (offset % DIR_ENTRY_NUM);
	}

	for (read_bytes = start; read_bytes < dir_size; read_bytes += DIR_ENTRY_SIZE) {
		if (!(read_bytes & (CLUSTER_SIZE - 1))) {
			if (dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0/*tail*/, 0/*dirty*/);
			lblock = read_bytes >> CLUSTER_SIZE_BITS;
			dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, read_bytes >> CLUSTER_SIZE_BITS, READ);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED) {
			if (!strcmp(dir->d_filename, filename)) {
				inode = nvfuse_read_inode(sb, dir->d_ino, WRITE);
				found_entry = read_bytes / DIR_ENTRY_SIZE;
				break;
			}
		}
		dir++;
	}

	if (inode == NULL || inode->i_ino == 0) {
		printf(" file (%s) is not found this directory\n", filename);
		nvfuse_release_bh(sb, dir_bh, 0/*tail*/, CLEAN);
		return NVFUSE_ERROR;
	}

	if (inode->i_type == NVFUSE_TYPE_DIRECTORY) {
		return error_msg(" rmfile() is supported for a file.");
	}

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_del_dir_indexing(sb, dir_inode, filename);
#endif 

	inode->i_links_count--;
	if (inode->i_links_count == 0) {	
		nvfuse_free_inode_size(sb, inode, 0/*size*/);
		nvfuse_relocate_delete_inode(sb, inode);
	}
	else {
		nvfuse_relocate_write_inode(sb, inode, inode->i_ino, 1/*dirty*/);
	}
		
	dir_inode->i_links_count--;
	dir_inode->i_ptr = dir_inode->i_links_count - 1;
	dir->d_flag = DIR_DELETED;

	/* Shrink directory entry that last entry is moved to delete entry. */
	nvfuse_shrink_dentry(sb, dir_inode, found_entry, dir_inode->i_links_count);
	
	if (dir_inode->i_links_count * DIR_ENTRY_SIZE % CLUSTER_SIZE == 0) {
		nvfuse_truncate_blocks(sb, dir_inode, (u64)dir_inode->i_links_count * DIR_ENTRY_SIZE);
		dir_inode->i_size -= CLUSTER_SIZE;
	}

	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, 1/*dirty*/);
	nvfuse_release_bh(sb, dir_bh, 0/*tail*/, DIRTY);

	nvfuse_check_flush_segment(sb);
	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_rmfile_path(struct nvfuse_handle *nvh, const char *path)
{
	int res;
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s8 filename[FNAME_SIZE];

	nvfuse_lock();

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf("invalid path\n");
		res = -1;
	} else {		
		res = nvfuse_rmfile(sb, dir_entry.d_ino, filename);
	}
	
	nvfuse_unlock();
	return res;
}

s32 nvfuse_unlink(struct nvfuse_handle *nvh, const char *path)
{
	return nvfuse_rmfile_path(nvh, path);
}

s32 nvfuse_rmdir(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename) 
{
	struct nvfuse_dir_entry *dir;
	struct nvfuse_inode *dir_inode = NULL, *inode = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;	
	s32 j, count = 0;
	s32 start = 0;
	lbno_t lblock = 0;
	u64 dir_size;
	u32 size, read_bytes, offset = 0;
	u32 found_entry;

	dir_inode = nvfuse_read_inode(sb, par_ino, WRITE);

	dir_size = dir_inode->i_size;

#if NVFUSE_USE_DIR_INDEXING == 1
	if (nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset) < 0) {
		printf(" dir (%s) is not in the index.\n", filename);
		offset = 0;
	}
#endif

	dir_size = dir_inode->i_size;
	start = offset * DIR_ENTRY_SIZE;
	if ((start & (CLUSTER_SIZE - 1))) {
		dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, start / CLUSTER_SIZE, READ);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir += (offset % DIR_ENTRY_NUM);
	}

	for (read_bytes = start; read_bytes < dir_size; read_bytes += DIR_ENTRY_SIZE) {
		if (!(read_bytes & (CLUSTER_SIZE - 1))) {
			if (dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0, 0);

			lblock = read_bytes >> CLUSTER_SIZE_BITS;
			dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, lblock, READ);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED) {
			if (!strcmp(dir->d_filename, filename)) {
				inode = nvfuse_read_inode(sb, dir->d_ino, WRITE);
				found_entry = read_bytes / DIR_ENTRY_SIZE;
				break;
			}
		}
		dir++;
	}
		
	if (inode == NULL || inode->i_ino == 0) {
		printf(" dir (%s) is not found this directory\n", filename);
		nvfuse_release_bh(sb, dir_bh, 0/*tail*/, CLEAN);
		return NVFUSE_ERROR;
	}

	if (inode->i_type == NVFUSE_TYPE_FILE) {
		return error_msg("rmdir is suppoted for a dir.");
	}

	if (strcmp(dir->d_filename, filename)) {
		printf(" filename is different\n");
		return NVFUSE_ERROR;
	}

	if (inode->i_links_count > 2)
	{
		printf(" rmdir error. dir has link count = %d\n", inode->i_links_count);
		return NVFUSE_ERROR;
	}

	dir->d_flag = DIR_DELETED;
	dir_inode->i_links_count--;
	dir_inode->i_ptr = dir_inode->i_links_count - 1;

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_del_dir_indexing(sb, dir_inode, filename);
#endif 
	/* delete allocated b+tree inode */
	{
		struct nvfuse_inode *bpinode = nvfuse_read_inode(sb, inode->i_bpino, 0);
		nvfuse_free_inode_size(sb, bpinode, 0);
		nvfuse_relocate_delete_inode(sb, bpinode);
	}

	/* Current Directory inode Deletion*/
	nvfuse_free_inode_size(sb, inode, 0);
	nvfuse_relocate_delete_inode(sb, inode);
	
	/* Shrink directory entry that last entry is moved to delete entry. */
	nvfuse_shrink_dentry(sb, dir_inode, found_entry, dir_inode->i_links_count);
	
	/* Free block reclaimation is necessary but test is required. */
	if (dir_inode->i_links_count * DIR_ENTRY_SIZE % CLUSTER_SIZE == 0) {
		nvfuse_truncate_blocks(sb, dir_inode, (u64)dir_inode->i_links_count * DIR_ENTRY_SIZE);
		dir_inode->i_size -= CLUSTER_SIZE;
	}

	/* Parent Directory Modification */
	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, DIRTY);
	nvfuse_release_bh(sb, dir_bh, 0/*tail*/, DIRTY);

	nvfuse_check_flush_segment(sb);
	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_rmdir_path(struct nvfuse_handle *nvh, const char *path)
{
	int res = 0;
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_superblock *sb;
	s8 filename[FNAME_SIZE];
		
	nvfuse_lock();
	
	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf("invalid path\n");
		res = -1;
	}
	else {
		sb = nvfuse_read_super(nvh);
		res = nvfuse_rmdir(sb, dir_entry.d_ino, filename);
		nvfuse_release_super(sb);
	}
	
	nvfuse_unlock();
	return res;
}

s32 nvfuse_mkdir(struct nvfuse_superblock *sb, inode_t par_ino, s8 *str, inode_t *new_ino, mode_t mode)
{
	struct nvfuse_dir_entry *dir;
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode *new_inode = NULL, *dir_inode = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;
	s32 i = 0, j, flag = 0;
	lbno_t lblock = 0;
	s32 new_entry = 0;
	s32 search_lblock = 0, search_entry = 0;
	s32 offset = 0;
	u32 dir_hash;
	s32 dir_num;
	s32 num_block;
	s32 ret;
	
	if (strlen(str) < 1 || strlen(str) >= FNAME_SIZE) {
		printf(" mkdir : name  = %s, %d\n", str, (int)strlen(str));
		ret = NVFUSE_ERROR;
		goto RET;
	}

	if (!nvfuse_lookup(sb, NULL, NULL, str, par_ino)) {
		printf(" exist file or directory\n");
		ret = NVFUSE_ERROR;
		goto RET;
	}

	dir_inode = nvfuse_read_inode(sb, par_ino, WRITE);

	// last update entry pointer
	search_lblock = (dir_inode->i_links_count - 1) / DIR_ENTRY_NUM;
	search_entry = (dir_inode->i_links_count - 1) % DIR_ENTRY_NUM;
	
	assert(dir_inode->i_ptr + 1 == dir_inode->i_links_count);

retry:

	dir_num = (dir_inode->i_size / DIR_ENTRY_SIZE);

	if (dir_num == dir_inode->i_links_count) {
		search_entry = -1;
		num_block = 0;
	}
	else {
		if (search_entry == DIR_ENTRY_NUM - 1) {
			search_entry = -1;
		}
		num_block = dir_num / DIR_ENTRY_NUM;
	}

	for (i = 0; i < num_block; i++) {
		dir_bh = nvfuse_get_bh(sb, dir_inode->i_ino, search_lblock, READ);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;

		for (new_entry = 0; new_entry < DIR_ENTRY_NUM; new_entry++) {
			search_entry++;
			if (search_entry == DIR_ENTRY_NUM) {
				search_entry = 0;
			}
			if (nvfuse_dir_is_invalid(dir + search_entry)) {
				flag = 1;
				goto find;
			}
		}
		nvfuse_release_bh(sb, dir_bh, 0, 0);
		dir_bh = NULL;
		search_entry = -1;
		search_lblock++;
		if (search_lblock == dir_inode->i_size >> CLUSTER_SIZE_BITS)
			search_lblock = 0;
	}

	dir_num = (dir_inode->i_size / DIR_ENTRY_SIZE);
	num_block = dir_num / DIR_ENTRY_NUM;
	search_lblock = num_block;

	if (dir_inode->i_links_count != dir_num) {
		printf(" dir link count differs from dir_num\n");
	}

	if (!flag) {
		nvfuse_release_bh(sb, dir_bh, 0, CLEAN);
		//new dentiry allocation		
		nvfuse_get_block(sb, dir_inode, dir_inode->i_size >> CLUSTER_SIZE_BITS, NULL, 1);
		dir_bh = nvfuse_get_new_bh(sb, dir_inode->i_ino, dir_inode->i_size >> CLUSTER_SIZE_BITS);
		nvfuse_release_bh(sb, dir_bh, 0, dir_bh->bh_dirty);
		dir_inode->i_size += CLUSTER_SIZE;
		nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, DIRTY);
		goto retry;
	}

find:
	dir_inode->i_ptr = search_lblock * DIR_ENTRY_NUM + search_entry;
	dir_inode->i_links_count++;
	
	assert(dir_inode->i_ptr + 1 == dir_inode->i_links_count);

	new_inode = nvfuse_alloc_new_inode(sb);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
	dir[search_entry].d_flag = DIR_USED;
	dir[search_entry].d_ino = new_inode->i_ino;
	dir[search_entry].d_version = new_inode->i_version;
	strcpy(dir[search_entry].d_filename, str);


#if NVFUSE_USE_DIR_INDEXING == 1		
	nvfuse_set_dir_indexing(sb, dir_inode, str, dir_inode->i_ptr);
#endif

	nvfuse_relocate_write_inode(sb, dir_inode, dir_inode->i_ino, DIRTY);
	nvfuse_release_bh(sb, dir_bh, 0, DIRTY);

	ret = nvfuse_get_block(sb, new_inode, new_inode->i_size >> CLUSTER_SIZE_BITS, NULL, 1);
	if (ret)
	{
		printf(" data block allocation fails.");
		return NVFUSE_ERROR;
	}
	dir_bh = nvfuse_get_bh(sb, new_inode->i_ino, 0, WRITE);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;

	strcpy(dir[0].d_filename, "."); // current dir
	dir[0].d_ino = new_inode->i_ino;
	dir[0].d_flag = DIR_USED;

	strcpy(dir[1].d_filename, ".."); // parent dir
	dir[1].d_ino = dir_inode->i_ino;
	dir[1].d_flag = DIR_USED;


	if (new_ino)
		*new_ino = new_inode->i_ino;

	new_inode->i_type = NVFUSE_TYPE_DIRECTORY;
	new_inode->i_size = CLUSTER_SIZE;
	new_inode->i_ptr = 1;
	new_inode->i_mode = (mode & 0777) | S_IFDIR;
	new_inode->i_gid = 0;
	new_inode->i_uid = 0;
	new_inode->i_links_count = 2;
	new_inode->i_atime = time(NULL);
	new_inode->i_ctime = time(NULL);
	new_inode->i_mtime = time(NULL);

	/* create bptree related nodes for new directory's denties */
	nvfuse_create_bptree(sb, new_inode);

	nvfuse_relocate_write_inode(sb, new_inode, new_inode->i_ino, DIRTY);
	nvfuse_release_bh(sb, dir_bh, 0, DIRTY);

	nvfuse_check_flush_segment(sb);
	
	ret = NVFUSE_SUCCESS;

RET:	

	return NVFUSE_SUCCESS;
}

s32 nvfuse_rename(struct nvfuse_handle *nvh, inode_t par_ino, s8 *name, inode_t new_par_ino, s8 *newname) 
{
	struct nvfuse_superblock *sb;
	struct nvfuse_inode *inode;
	inode_t ino = 0;

	sb = nvfuse_read_super(nvh);

	nvfuse_rm_direntry(sb, par_ino, name, &ino);

	if (!nvfuse_lookup(sb, &inode, NULL, newname, new_par_ino)) {
		if (inode->i_type == NVFUSE_TYPE_DIRECTORY) {
			nvfuse_relocate_write_inode(sb, inode, inode->i_ino, DIRTY);
			nvfuse_rmdir(sb, new_par_ino, newname);
		}
		else {
			nvfuse_relocate_write_inode(sb, inode, inode->i_ino, DIRTY);
			nvfuse_rmfile(sb, new_par_ino, newname);
		}
	}

	nvfuse_link(sb, new_par_ino, newname, ino);

	nvfuse_check_flush_segment(sb);
	nvfuse_release_super(sb);

	return 0;
}

s32 nvfuse_rename_path(struct nvfuse_handle *nvh, const char *from, const char *to)
{
	struct nvfuse_dir_entry old_dir_entry;
	s8 old_filename[FNAME_SIZE];

	struct nvfuse_dir_entry new_dir_entry;
	s8 new_filename[FNAME_SIZE];
	s32 res; 

	res = nvfuse_path_resolve(nvh, from, old_filename, &old_dir_entry);
	if (res < 0)
		return res;
	
	res = nvfuse_path_resolve(nvh, to, new_filename, &new_dir_entry);
	if (res < 0)
		return res;

	return nvfuse_rename(nvh, old_dir_entry.d_ino, old_filename, new_dir_entry.d_ino, new_filename);
}

s32 nvfuse_hardlink(struct nvfuse_superblock *sb, inode_t par_ino, s8 *name, inode_t new_par_ino, s8 *newname)
{
	struct nvfuse_inode *inode;
	struct nvfuse_dir_entry direntry;
	inode_t ino = 0;
	s32 res;

	printf(" src name = %s dst name = %s \n", name, newname);

	if(nvfuse_lookup(sb, NULL, &direntry, name, par_ino) < 0) 
	{
		printf(" link: source inode doesn't exist\n");
		return -1;
	}

	ino = direntry.d_ino;

	if (!nvfuse_lookup(sb, &inode, NULL, newname, new_par_ino)) {
		printf(" link: link exists %s \n", newname);
		return -1;
	}

	res = nvfuse_link(sb, new_par_ino, newname, ino);
	if (res) {
		printf(" Error: link() \n");
	}

	nvfuse_check_flush_segment(sb);
	
	return 0;
}

s32 nvfuse_hardlink_path(struct nvfuse_handle *nvh, const char *from, const char *to)
{
	struct nvfuse_dir_entry from_dir_entry;
	s8 from_filename[FNAME_SIZE];

	struct nvfuse_dir_entry to_dir_entry;
	s8 to_filename[FNAME_SIZE];

	struct nvfuse_superblock *sb;
	s32 res;

	printf(" hardlink: from = %s to = %s\n", from, to);

	res = nvfuse_path_resolve(nvh, from, from_filename, &from_dir_entry);
	if (res < 0)
		return res;
	
	res = nvfuse_path_resolve(nvh, to, to_filename, &to_dir_entry);
	if (res < 0)
		return res;

	sb = nvfuse_read_super(nvh);

	res = nvfuse_hardlink(sb, from_dir_entry.d_ino, from_filename, to_dir_entry.d_ino, to_filename);
	
	nvfuse_release_super(sb);

	return res;
}

s32 nvfuse_mknod(struct nvfuse_handle *nvh, const char *path, mode_t mode, dev_t dev) 
{
	int res = 0;
	struct nvfuse_dir_entry dir_entry;
	s8 filename[FNAME_SIZE];
	struct nvfuse_superblock *sb;

	nvfuse_lock();
	
	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;
	
	if (dir_entry.d_ino == 0) {
		printf("invalid path\n");
		res = -1;
	}
	else {
		sb = nvfuse_read_super(nvh);
		res = nvfuse_createfile(sb, dir_entry.d_ino, filename, 0, mode);
		nvfuse_release_super(sb);
	}

	nvfuse_unlock();

	return 0;
}

s32 nvfuse_mkdir_path(struct nvfuse_handle *nvh, const char *path, mode_t mode)
{
	int res = 0;
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_superblock *sb;
	s8 filename[FNAME_SIZE];
	
	nvfuse_lock();

	sb = nvfuse_read_super(nvh);

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf("invalid path\n");
		res = -1;
	}
	else {
		res = nvfuse_mkdir(sb, dir_entry.d_ino, filename, 0, mode);
	}
	
	nvfuse_release_super(sb);
	nvfuse_unlock();

	return res;
}

s32 nvfuse_truncate_path(struct nvfuse_handle *nvh, const char *path, nvfuse_off_t size)
{
	int res;
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_superblock *sb;
	s8 filename[FNAME_SIZE];

	sb = nvfuse_read_super(nvh);

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf("invalid path\n");
		res = -1;
	}
	else {
		res = nvfuse_truncate(sb, dir_entry.d_ino, filename, size);
	}

	return res;
}

s32 nvfuse_ftruncate(struct nvfuse_handle *nvh, s32 fid, nvfuse_off_t size) 
{
	struct nvfuse_superblock *sb;
	struct nvfuse_file_table *ft;
	struct nvfuse_inode *inode;
	int res = 0;

	sb = nvfuse_read_super(nvh);

	ft = sb->sb_file_table + fid;

	inode = nvfuse_read_inode(sb, ft->ino, READ);
	nvfuse_free_inode_size(sb, inode, size);
	inode->i_size = size;
	nvfuse_relocate_write_inode(sb, inode, inode->i_ino, DIRTY);
	
	nvfuse_check_flush_segment(sb);
	nvfuse_sync(sb);	

	return res;
}

s32 nvfuse_symlink(struct nvfuse_handle *nvh, const char *link, inode_t parent, const char *name)
{
	struct nvfuse_superblock *sb;
	struct nvfuse_inode *inode;	
	int res = 0;
	int ino = 0;
	int fid = 0;
	unsigned int bytes = 0;

	sb = nvfuse_read_super(nvh);

	printf(" symlink : \"%s\", parent #%d, name \"%s\" \n",
		link, (int)parent, name);
	
	nvfuse_lock();

	res = nvfuse_createfile(sb, parent, (char *)name, &ino, 0777 | S_IFLNK);
	if (res != NVFUSE_SUCCESS) {
		printf(" create file error \n");
		return res;
	}

	fid = nvfuse_openfile_ino(sb, ino, O_WRONLY);

	bytes = nvfuse_writefile(nvh, fid, link, strlen(link) + 1, 0);

	if (bytes != strlen(link) + 1) {
		printf(" symlink error \n");
		return -1;
	}

	inode = nvfuse_read_inode(sb, ino, 0);

	nvfuse_relocate_write_inode(sb, inode, inode->i_ino, 0);
	
	nvfuse_closefile(nvh, fid);

	nvfuse_release_super(sb);

	nvfuse_unlock();

	return 0;
}

s32 nvfuse_symlink_path(struct nvfuse_handle *nvh, const char *target_name, const char *link_name)
{
	struct nvfuse_dir_entry dir_entry;
	s8 filename[FNAME_SIZE];
	s32 res;
	
	res = nvfuse_path_resolve(nvh, link_name, filename, &dir_entry);
	if (res < 0)
		return res;

	return nvfuse_symlink(nvh, target_name, dir_entry.d_ino, filename);
}

s32 nvfuse_readlink_ino(struct nvfuse_handle *nvh, inode_t ino, char *buf, size_t size)
{
	int rc;
	unsigned int bytes; 
	int fid; 
	int res = 0;
	struct nvfuse_inode *inode; 
	struct nvfuse_superblock *sb;
	
	sb = nvfuse_read_super(nvh);

	printf(" readlink : ino  = %d\n", (int) ino);
		
	if(ino == 0)
		ino = ROOT_INO;

	nvfuse_lock();
	inode = nvfuse_read_inode(sb, ino, READ);

	printf("op_readlink: inode contents: i_mode=0%o, i_links_count=%d \n",
		inode->i_mode, inode->i_links_count);

	nvfuse_relocate_write_inode(sb, inode, inode->i_ino, CLEAN);
		
	fid = nvfuse_openfile_ino(sb, ino, O_RDONLY);
	
	bytes = nvfuse_readfile(nvh, fid, buf, size, 0);
	if (bytes != size) {
		printf(" error : read bytes = %d \n", bytes);
		return -1;
	}

	printf(" read link = %s \n", buf);
	nvfuse_closefile(nvh, fid);
	
	nvfuse_release_super(sb);

	nvfuse_unlock();
	
	//free (buf);

	return bytes;
}

s32 nvfuse_getattr(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char dirname[FNAME_SIZE];
	char filename[FNAME_SIZE];
	int res;

	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {

		res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
		if (res < 0)
			return res;

		if (dir_entry.d_ino == 0) {
			printf("invalid path\n");
			res = -ENOENT;
			goto RET;
		} else {
			sb = nvfuse_read_super(nvh);
			if(nvfuse_lookup(sb, &inode, &dir_entry, filename, dir_entry.d_ino) < 0){
				res = -ENOENT;
				goto RET;
			}

			//if (inode->i_mode & S_IFLNK)
			//	stbuf->st_mode = S_IFLNK | inode->i_mode;
			//else 
			if(inode->i_type == NVFUSE_TYPE_FILE)
				stbuf->st_mode = S_IFREG | inode->i_mode;
			else
				stbuf->st_mode = S_IFDIR | inode->i_mode;

			stbuf->st_nlink = inode->i_links_count;

			stbuf->st_size = inode->i_size;
			stbuf->st_atime = inode->i_atime;
			stbuf->st_mtime = inode->i_mtime;
			stbuf->st_ctime = inode->i_ctime;

			stbuf->st_gid = inode->i_gid;
			stbuf->st_uid = inode->i_uid;

			nvfuse_relocate_write_inode(sb, inode, inode->i_ino, CLEAN);
			nvfuse_release_super(sb);

			res = 0;
		}
	}

RET:
	return res;
}

s32 nvfuse_fgetattr(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf, s32 fd)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	struct nvfuse_file_table *ft;
	char dirname[FNAME_SIZE];
	char filename[FNAME_SIZE];
	int res;

	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {
		sb = nvfuse_read_super(nvh);

		ft = sb->sb_file_table + fd;
		inode = nvfuse_read_inode(sb, ft->ino, READ);

		//if (inode->i_mode & S_IFLNK)
		//	stbuf->st_mode = S_IFLNK | inode->i_mode;
		//else 
		if(inode->i_type == NVFUSE_TYPE_FILE)
			stbuf->st_mode = S_IFREG | inode->i_mode;
		else
			stbuf->st_mode = S_IFDIR | inode->i_mode; 

		stbuf->st_nlink = inode->i_links_count;
		stbuf->st_size = inode->i_size;
		stbuf->st_atime = inode->i_atime;
		stbuf->st_mtime = inode->i_mtime;
		stbuf->st_ctime = inode->i_ctime;

		nvfuse_relocate_write_inode(sb, inode, inode->i_ino, CLEAN);
		nvfuse_release_super(sb);

		res = 0;
	}

RET:
	return res;
}
#if NVFUSE_OS == NVFUSE_OS_LINUX
s32 nvfuse_access(struct nvfuse_handle *nvh, const char *path, int mask)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char dirname[FNAME_SIZE];
	char filename[FNAME_SIZE];
	int res;

	printf(" nvfuse_access = %s (%d)\n", path, (int)strlen(path));
	if (strcmp(path, "/") == 0) {
		res = 0;
	} else {

		res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
		if (res < 0)
			return res;

		if (dir_entry.d_ino == 0) {
			printf("invalid path\n");
			res = -1;
			goto RET;
		} else {
			sb = nvfuse_read_super(nvh);
			if(nvfuse_lookup(sb, &inode, &dir_entry, filename, dir_entry.d_ino) < 0){
				res = -1;
				goto RET;
			}

			if(inode->i_type == NVFUSE_TYPE_FILE)
			{
				if (mask & F_OK)
					res = 0;
				if (mask & R_OK)
					res = 0;
				if (mask & W_OK)
					res = 0;
			}
			else
			{
				if (mask & F_OK)
					res = 0;
				if (mask & R_OK)
					res = 0;
				if (mask & W_OK)
					res = -1;
			}


			nvfuse_relocate_write_inode(sb, inode, inode->i_ino, CLEAN);
			nvfuse_release_super(sb);
		}
	}

RET:
	return res;
}
#endif

s32 nvfuse_readlink(struct nvfuse_handle *nvh, const char *path, char *buf, size_t size)
{
	struct nvfuse_dir_entry parent_dir, cur_dir;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char filename[FNAME_SIZE];
	int res;
	int bytes;

	sb = nvfuse_read_super(nvh);

	if (strcmp(path, "/") == 0) {
		res = -1;
	} else {
		res = nvfuse_path_resolve(nvh, path, filename, &parent_dir);
		if (res < 0)
			goto RET;

		if(nvfuse_lookup(sb, NULL, &cur_dir, filename, parent_dir.d_ino) < 0){
			res = -1;
			goto RET;
		}

		if (cur_dir.d_ino == 0) {
			printf("invalid path\n");
			res = -1;
			goto RET;
		} else {
			bytes = nvfuse_readlink_ino(nvh, cur_dir.d_ino, buf, size);
			res = bytes;
		}
	}

RET:
	nvfuse_release_super(sb);
	return res;
}
#if NVFUSE_OS == NVFUSE_OS_LINUX
s32 nvfuse_statvfs(struct nvfuse_handle *nvh, const char *path, struct statvfs *buf)
{
    int res;
    struct nvfuse_superblock *sb;

    if ((buf == NULL)) return -1;
    buf->f_bsize = CLUSTER_SIZE;    /* file system block size */
    //buf->f_frsize = 0;   /* fragment size */

    sb = nvfuse_read_super(nvh);

    buf->f_blocks = (fsblkcnt_t)sb->sb_no_of_blocks;   /* size of fs in f_frsize units */
    buf->f_bfree = (fsblkcnt_t)sb->sb_no_of_blocks  - sb->sb_no_of_used_blocks;    /* # free blocks */
    buf->f_bavail = (fsblkcnt_t)sb->sb_no_of_blocks  - sb->sb_no_of_used_blocks;/* # free blocks for non-root */
    buf->f_files = sb->sb_max_inode_num - sb->sb_free_inodes;    /* # inodes */
    buf->f_ffree = sb->sb_free_inodes;    /* # free inodes */
    buf->f_favail = sb->sb_free_inodes;   /* # free inodes for non-root */
    buf->f_flag = 0;     /* mount flags */

    buf->f_namemax = FNAME_SIZE-1;  /* maximum filename length */

    nvfuse_release_super(sb);

    return 0;
}

s32 nvfuse_chmod_path(struct nvfuse_handle *nvh, const char *path, mode_t mode)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char dirname[FNAME_SIZE];
	char filename[FNAME_SIZE];
	s32 mask;
	int res;

	if (strcmp(path, "/") == 0) {
		res = -1;
	} else {

		res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
		if (res < 0)
			return res;

		if (dir_entry.d_ino == 0) {
			printf("invalid path\n");
			res = -1;
			goto RET;
		} else {
			sb = nvfuse_read_super(nvh);
			if(nvfuse_lookup(sb, &inode, &dir_entry, filename, dir_entry.d_ino) < 0){
				res = -1;
				goto RET;
			}

			mask = S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
			inode->i_mode = (inode->i_mode & ~mask) | (mode & mask);

			nvfuse_relocate_write_inode(sb, inode, inode->i_ino, DIRTY);
			nvfuse_release_super(sb);
		}
	}

RET:
	return res;
}

s32 nvfuse_chown(struct nvfuse_handle *nvh, const char *path, uid_t uid, gid_t gid)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char dirname[FNAME_SIZE];
	char filename[FNAME_SIZE];
	int res;

	if (strcmp(path, "/") == 0) {
		res = -1;
	} else {

		res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
		if (res < 0)
			return res;

		if (dir_entry.d_ino == 0) {
			printf("invalid path\n");
			res = -1;
			goto RET;
		} else {
			sb = nvfuse_read_super(nvh);
			if(nvfuse_lookup(sb, &inode, &dir_entry, filename, dir_entry.d_ino) < 0){
				res = -1;
				goto RET;
			}

			inode->i_uid = uid;
			inode->i_gid = gid;

			nvfuse_relocate_write_inode(sb, inode, inode->i_ino, DIRTY);
			nvfuse_release_super(sb);
		}
	}

RET:
	return res;
}
#endif

s32 nvfuse_fdatasync(struct nvfuse_handle *nvh, int fd)
{
	struct nvfuse_superblock *sb;
	sb = nvfuse_read_super(nvh);
	nvfuse_sync(sb);
	nvfuse_release_super(sb);
	return 0;
}

s32 nvfuse_fsync(struct nvfuse_handle *nvh, int fd)
{
	struct nvfuse_superblock *sb;
	sb = nvfuse_read_super(nvh);
	nvfuse_sync(sb);
	nvfuse_release_super(sb);
	return 0;
}

s32 nvfuse_fallocate(struct nvfuse_handle *nvh, const char *path, off_t start, off_t length) 
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char dirname[FNAME_SIZE];
	char filename[FNAME_SIZE];
	int res;
	u32 curr_block;
	
	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf("invalid path\n");
		res = -1;
		goto RET;
	} else {
		sb = nvfuse_read_super(nvh);
		if(nvfuse_lookup(sb, &inode, &dir_entry, filename, dir_entry.d_ino) < 0){
			res = -1;
			goto RET;
		}

		for (curr_block = start;curr_block < start+length; curr_block)
		{
			res = nvfuse_get_block(sb, inode, curr_block, NULL, 1 /*create*/);
			if (res < 0) {
				printf(" Error: nvfuse_get_block()\n");
				res = -1;
				goto RET;
			}
		}

		nvfuse_relocate_write_inode(sb, inode, inode->i_ino, DIRTY);
		nvfuse_sync(sb);
		nvfuse_release_super(sb);
	}
RET:;
    return res;
}
