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
s32 nvfuse_lookup(struct nvfuse_superblock *sb, struct nvfuse_inode **file_inode, struct nvfuse_dir_entry *file_entry,
	s8 *filename, s32 cur_dir_ino);

s32 nvfuse_openfile_path(const char *path, int flags, int mode);
s32 nvfuse_openfile(inode_t par_ino, s8 *filename, int flags, int mode);
s32 nvfuse_openfile_ino(struct nvfuse_superblock *sb, inode_t ino, s32 mode);

s32 nvfuse_closefile(s32 fid);

s32 nvfuse_readfile(u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset);
s32 nvfuse_writefile(u32 fid, const s8 *user_buf, u32 count, nvfuse_off_t woffset);

s32 nvfuse_createfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *str, inode_t *new_ino, mode_t mode);

s32 nvfuse_rmfile(inode_t par_ino, s8 *filename);
s32 nvfuse_rmfile_path(const char *path);

s32 nvfuse_mkdir(inode_t par_ino, s8 *str, inode_t *new_ino, mode_t mode); s32 nvfuse_rename(inode_t par_ino, s8 *name, inode_t new_par_ino, s8 *newname);
s32 nvfuse_mkdir_path(const char *path, mode_t mode);


s32 nvfuse_rmdir(inode_t par_ino, s8 *filename);
s32 nvfuse_rmdir_path(const char *path);


s32 nvfuse_rename(inode_t par_ino, s8 *name, inode_t new_par_ino, s8 *newname);
s32 nvfuse_mknod(const char *path, mode_t mode, dev_t dev);

s32 nvfuse_symlink(const char *link, inode_t parent, const char *name);
s32 nvfuse_symlink_path(const char *target_name, const char *link_name);

s32 nvfuse_getattr(const char *path, struct stat *stbuf);
s32 nvfuse_fgetattr(const char *path, struct stat *stbuf, s32 fd);
s32 nvfuse_readlink(const char *path, char *buf, size_t size);
s32 nvfuse_access(const char *path, int mask);
struct dirent *nvfuse_readdir(inode_t par_ino, struct dirent *dentry, off_t dir_offset);
s32 nvfuse_opendir(const char *path);
s32 nvfuse_unlink(const char *path);
s32 nvfuse_truncate_path(const char *path, nvfuse_off_t size);
s32 nvfuse_ftruncate(s32 fid, nvfuse_off_t size);
s32 nvfuse_statvfs(const char *path, struct statvfs *buf);
s32 nvfuse_rename_path(const char *from, const char *to);
s32 nvfuse_hardlink_path(const char *from, const char *to);
#if NVFUSE_OS == NVFUSE_OS_LINUX
s32 nvfuse_chmod_path(const char *path, mode_t mode);
s32 nvfuse_chown(const char *path, uid_t uid, gid_t gid);
#endif
s32 nvfuse_fdatasync(int fd);
s32 nvfuse_fsync(int fd);
#endif
