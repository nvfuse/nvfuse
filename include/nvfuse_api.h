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

#if NVFUSE_OS == NVFUSE_OS_LINUX
#include <sys/statvfs.h>
#endif

#include "nvfuse_types.h"

#ifndef _NVFUSE_API_H
#define _NVFUSE_API_H

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
struct dirent {
	int d_ino;
	char d_name[128];
	int d_type;
};
#define DT_DIR 1
#define DT_REG 2
#endif

//#define VERIFY_BEFORE_RM_FILE

s32 nvfuse_writefile_buffered_aio(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf, u32 count, nvfuse_off_t woffset);
s32 nvfuse_gather_bh(struct nvfuse_superblock *sb, s32 fid, const s8 *user_buf, u32 count, nvfuse_off_t woffset, struct list_head *aio_bh_head, s32 *aio_bh_count);

struct nvfuse_handle *nvfuse_create_handle(struct nvfuse_handle *a_nvh, int argc, char **argv);
void nvfuse_destroy_handle(struct nvfuse_handle *nvh, s32 deinit_iom, s32 need_umount);

s32 nvfuse_lookup(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx **file_ictx, struct nvfuse_dir_entry *file_entry,
	const s8 *filename, const s32 cur_dir_ino);

s32 nvfuse_openfile_path(struct nvfuse_handle *nvh, const char *path, int flags, int mode);
s32 nvfuse_openfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, s32 flags, s32 mode);
s32 nvfuse_openfile_ino(struct nvfuse_superblock *sb, inode_t ino, s32 flags);

s32 nvfuse_closefile(struct nvfuse_handle *nvh, s32 fid);

s32 nvfuse_readfile(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset);
s32 nvfuse_readfile_aio(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset);

s32 nvfuse_writefile(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf, u32 count, nvfuse_off_t woffset);

s32 nvfuse_createfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *str, inode_t *new_ino, mode_t mode, dev_t dev);

s32 nvfuse_rmfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename);
s32 nvfuse_rmfile_path(struct nvfuse_handle *nvh, const char *path);

s32 nvfuse_mkdir(struct nvfuse_superblock *sb, const inode_t par_ino, const s8 *dirname, inode_t *new_ino, const mode_t mode);
s32 nvfuse_mkdir_path(struct nvfuse_handle *nvh, const char *path, mode_t mode);

s32 nvfuse_rmdir(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename);
s32 nvfuse_rmdir_path(struct nvfuse_handle *nvh, const char *path);

s32 nvfuse_rename(struct nvfuse_handle *nvh, inode_t par_ino, s8 *name, inode_t new_par_ino, s8 *newname);
s32 nvfuse_mknod(struct nvfuse_handle *nvh, const char *path, mode_t mode, dev_t dev);

s32 nvfuse_symlink(struct nvfuse_handle *nvh, const char *link, inode_t parent, const char *name);
s32 nvfuse_symlink_path(struct nvfuse_handle *nvh, const char *target_name, const char *link_name);

s32 nvfuse_getattr(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf);
s32 nvfuse_fgetattr(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf, s32 fd);
s32 nvfuse_readlink(struct nvfuse_handle *nvh, const char *path, char *buf, size_t size);
s32 nvfuse_access(struct nvfuse_handle *nvh, const char *path, int mask);
struct dirent *nvfuse_readdir(struct nvfuse_handle *nvh, inode_t par_ino, struct dirent *dentry, off_t dir_offset);
s32 nvfuse_opendir(struct nvfuse_handle *nvh, const char *path);
s32 nvfuse_unlink(struct nvfuse_handle *nvh, const char *path);
s32 nvfuse_truncate_path(struct nvfuse_handle *nvh, const char *path, nvfuse_off_t size);
s32 nvfuse_ftruncate(struct nvfuse_handle *nvh, s32 fid, nvfuse_off_t size);
s32 nvfuse_statvfs(struct nvfuse_handle *nvh, const char *path, struct statvfs *buf);
s32 nvfuse_rename_path(struct nvfuse_handle *nvh, const char *from, const char *to);
s32 nvfuse_hardlink_path(struct nvfuse_handle *nvh, const char *from, const char *to);
#if NVFUSE_OS == NVFUSE_OS_LINUX
s32 nvfuse_chmod_path(struct nvfuse_handle *nvh, const char *path, mode_t mode);
s32 nvfuse_chown(struct nvfuse_handle *nvh, const char *path, uid_t uid, gid_t gid);
#endif
s32 nvfuse_fdatasync(struct nvfuse_handle *nvh, int fd);
s32 nvfuse_fsync(struct nvfuse_handle *nvh, int fd);
s32 nvfuse_sync(struct nvfuse_handle *nvh);

s32 nvfuse_fdsync_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);
s32 nvfuse_fsync_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);
s32 _nvfuse_fsync_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);

inode_t nvfuse_get_cwd_ino(struct nvfuse_handle *nvh);
inode_t nvfuse_get_root_ino(struct nvfuse_handle *nvh);
void nvfuse_set_cwd_ino(struct nvfuse_handle *nvh, inode_t cwd_ino);
void nvfuse_set_root_ino(struct nvfuse_handle *nvh, inode_t root_ino);

s32 nvfuse_hardlink(struct nvfuse_superblock *sb, inode_t par_ino, s8 *name, inode_t new_par_ino, s8 *newname);
s32 nvfuse_utimens(struct nvfuse_handle *nvh, const char *path, const struct timespec ts[2]);

s32 nvfuse_shrink_dentry(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, u32 to_entry, u32 from_entry);

s32 nvfuse_fallocate(struct nvfuse_handle *nvh, const char *path, s64 start, s64 length);
s32 nvfuse_fallocate_verify(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, u32 start, u32 max_block);
s32 nvfuse_writefile_directio_prepare(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf, u32 count, nvfuse_off_t woffset);
s32 nvfuse_writefile_directio_core(struct nvfuse_superblock *sb, s32 fid, const s8 *user_buf, u32 count, nvfuse_off_t woffset);
s32 nvfuse_readfile_aio_directio(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset);
s32 nvfuse_readfile_directio_core(struct nvfuse_superblock *sb, u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset, s32 sync_read);

s32 nvfuse_readlink_ino(struct nvfuse_handle *nvh, inode_t ino, char *buf, size_t size);
s32 nvfuse_writefile_core(struct nvfuse_superblock *sb, s32 fid, const s8 *user_buf, u32 count, nvfuse_off_t woffset);
s32 nvfuse_readfile_core(struct nvfuse_superblock *sb, u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset, s32 sync_read);
s32 nvfuse_path_resolve(struct nvfuse_handle *nvh, const char *path, char *filename, struct nvfuse_dir_entry *direntry);
s32 nvfuse_fgetblk(struct nvfuse_superblock *sb, s32 fid, s32 lblk, s32 max_blocks, s32 *num_alloc);

#endif
