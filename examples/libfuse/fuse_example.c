/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 30/10/2016
*/
/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#define FUSE_USE_VERSION 26

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fuse.h>
#include <ulockmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif
#include <sys/file.h> /* flock(2) */

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"

#if NVFUSE_OS == NVFUSE_OS_LINUX
#define EXAM_USE_RAMDISK	0
#define EXAM_USE_FILEDISK	0
#define EXAM_USE_UNIXIO		1
#define EXAM_USE_SPDK		0
#else
#define EXAM_USE_RAMDISK	0
#define EXAM_USE_FILEDISK	1
#define EXAM_USE_UNIXIO		0
#define EXAM_USE_SPDK		0
#endif

/* nvfuse handle */
struct nvfuse_handle *nvh;

#define INIT_IOM	1
#define MOUNT		1
#define DEINIT_IOM	1
#define UMOUNT		1

/* initialization of NVFUSE library */
int nvfuse_init(int format)
{
	int ret = 0;

#	if (EXAM_USE_RAMDISK == 1)
	nvh = nvfuse_create_handle(NULL, INIT_IOM, IO_MANAGER_RAMDISK, format, MOUNT);
#	elif (EXAM_USE_FILEDISK == 1)
	nvh = nvfuse_create_handle(NULL, INIT_IOM, IO_MANAGER_FILEDISK, format, MOUNT);
#	elif (EXAM_USE_UNIXIO == 1)
	nvh = nvfuse_create_handle(NULL, INIT_IOM, IO_MANAGER_UNIXIO, format, MOUNT);
#	elif (EXAM_USE_SPDK == 1)
	nvh = nvfuse_create_handle(NULL, INIT_IOM, IO_MANAGER_SPDK, format, MOUNT);
#	endif
	
	if (nvh == NULL)
		ret = -1;
RET:;
	return ret;
}

/* de-initialization of NVFUSE library */
int nvfuse_deinit()
{
	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);
	printf(" Finalizing ... \n"); 
}


static int xmp_getattr(const char *path, struct stat *stbuf)
{
	int res;

	printf(" Getattr path = %s\n", path);
	res = nvfuse_getattr(nvh, path, stbuf);
	if (res < 0) {
		return res;
	}

	return 0;
}

static int xmp_fgetattr(const char *path, struct stat *stbuf,
			struct fuse_file_info *fi)
{
	int res;

	(void) path;

	printf(" Fgetattr \n");
	res = nvfuse_fgetattr(nvh, path, stbuf, fi->fh);
	if (res < 0)
		return errno;

	return 0;
}

static int xmp_access(const char *path, int mask)
{
	int res;

	printf(" Access %s\n", path);
	res = nvfuse_access(nvh, path, mask);
	if (res < 0) {
		printf(" ret %d\n", res);
		return res;
	}

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	int res;

	printf(" Readlink path = %s\n", path);
	res = nvfuse_readlink(nvh, path, buf, size - 1);
	if (res < 0)
		return res;

	buf[res] = '\0';
	return 0;
}

struct xmp_dirp {
	inode_t dp;
	struct dirent *entry;
	off_t offset;
};

static int xmp_opendir(const char *path, struct fuse_file_info *fi)
{
	int res;
	struct xmp_dirp *d = malloc(sizeof(struct xmp_dirp));

	printf(" Opendir \n");
	if (d == NULL)
		return -ENOMEM;

	d->dp = nvfuse_opendir(nvh, path);
	if (d->dp == 0) {
		res = -errno;
		free(d);
		return res;
	}
	d->offset = 0;
	d->entry = NULL;

	fi->fh = (unsigned long) d;
	return 0;
}

static inline struct xmp_dirp *get_dirp(struct fuse_file_info *fi)
{
	return (struct xmp_dirp *) (uintptr_t) fi->fh;
}

static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
	struct xmp_dirp *d = get_dirp(fi);

	printf(" Readdir offset = %lu\n", offset);
	(void) path;
	if (offset != d->offset) {
		//seekdir(d->dp, offset); // FIXME: current implementation doesn't consider both seekdir() and telldir().
		d->entry = NULL;
		d->offset = offset;
	}

	while (1) {
		struct dirent dentry;
		struct stat st;
		off_t nextoff;

		if (!d->entry) {
			d->entry = nvfuse_readdir(nvh, d->dp, &dentry, d->offset);
			if (!d->entry)
				break;
		}

		memset(&st, 0, sizeof(st));
		st.st_ino = d->entry->d_ino;
		st.st_mode = d->entry->d_type << 12;
		//nextoff = telldir(d->dp);
		nextoff = d->offset + 1;
		if (filler(buf, d->entry->d_name, &st, nextoff))
			break;

		d->entry = NULL;
		d->offset = nextoff;
	}

	return 0;
}

static int xmp_releasedir(const char *path, struct fuse_file_info *fi)
{
	struct xmp_dirp *d = get_dirp(fi);
	(void) path;

	printf(" Releasedir \n");

	free(d);
	return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res = -1;

	printf(" Mknod \n");
	if (S_ISFIFO(mode)) {
		//res = mkfifo(path, mode);
		printf(" Error: mkfifo isn't supported in current implementation\n");
	} else {
		res = nvfuse_mknod(nvh, path, mode, rdev);
	}

	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	int res;

	printf(" Mkdir \n");
	res = nvfuse_mkdir_path(nvh, path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_unlink(const char *path)
{
	int res;

	printf(" Unlink\n");
	res = nvfuse_unlink(nvh, path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rmdir(const char *path)
{
	int res;

	printf(" Rmdir \n");
	res = nvfuse_rmdir_path(nvh, path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_symlink(const char *target, const char *link)
{
	int res;

	printf(" Symlink target = %s link = %s \n", target, link);
	res = nvfuse_symlink_path(nvh, target, link);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char *from, const char *to)
{
	int res;

	printf(" Rename \n");
	res = nvfuse_rename_path(nvh, from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char *from, const char *to)
{
	int res;

	printf(" Link \n");
	res = nvfuse_hardlink_path(nvh, from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char *path, mode_t mode)
{
	int res;

	printf(" Chmod \n");
	res = nvfuse_chmod_path(nvh, path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid)
{
	int res;

	printf(" Chown\n");
	res = nvfuse_chown(nvh, path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char *path, off_t size)
{
	int res;

	printf(" Truncate \n");
	res = nvfuse_truncate_path(nvh, path, size);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_ftruncate(const char *path, off_t size,
			 struct fuse_file_info *fi)
{
	int res;

	(void) path;

	printf(" Ftruncate \n");
	res = nvfuse_ftruncate(nvh, fi->fh, size);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2])
{
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int xmp_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int fd;

	printf(" Create \n");
	fd = nvfuse_openfile_path(nvh, path, fi->flags, mode);
	if (fd == -1)
		return -errno;

	fi->fh = fd;
	return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	int fd;

	printf(" Open \n");
	fd = nvfuse_openfile_path(nvh, path, fi->flags, 0);
	if (fd == -1)
		return -errno;

	fi->fh = fd;
	return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	int res;

	printf(" Read fd = %d size = %d offset = %d\n", (int)fi->fh, (int)size, (int)offset);
	(void) path;
	res = nvfuse_readfile(nvh, fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	return res;
}

static int xmp_read_buf(const char *path, struct fuse_bufvec **bufp,
			size_t size, off_t offset, struct fuse_file_info *fi)
{
	struct fuse_bufvec *src;

	(void) path;

	printf(" Readbuf fd = %d offset = %d\n", (int)fi->fh, (int)offset);
	src = malloc(sizeof(struct fuse_bufvec));
	if (src == NULL)
		return -ENOMEM;

	*src = FUSE_BUFVEC_INIT(size);

	src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	src->buf[0].fd = fi->fh;
	src->buf[0].pos = offset;

	*bufp = src;

	return 0;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	int res;

	(void) path;
	printf(" Write fd = %d size = %d offset = %d\n", (int)fi->fh, (int)size, (int)offset);
	res = nvfuse_writefile(nvh, fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	return res;
}

static int xmp_write_buf(const char *path, struct fuse_bufvec *buf,
		     off_t offset, struct fuse_file_info *fi)
{
	struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));

	(void) path;

	printf(" Writebuf fd = %d offset = %d\n", (int)fi->fh, (int)offset);
	dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	dst.buf[0].fd = fi->fh;
	dst.buf[0].pos = offset;

	return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	int res;

	printf(" Statfs \n");
	res = nvfuse_statvfs(nvh, path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

#if 0
static int xmp_flush(const char *path, struct fuse_file_info *fi)
{
	int res;

	(void) path;
	/* This is called from every close on an open file, so call the
	   close on the underlying filesystem.	But since flush may be
	   called multiple times for an open file, this must not really
	   close the file.  This is important if used on a network
	   filesystem like NFS which flush the data/metadata on close() */
	res = close(dup(fi->fh));
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	printf(" Release \n");
	nvfuse_closefile(nvh, fi->fh);

	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	int res;
	(void) path;

	printf(" Fsync \n");
	if (isdatasync)
		res = nvfuse_fdatasync(nvh, fi->fh);
	else
		res = nvfuse_fsync(nvh, fi->fh);

	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
	int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
	int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

#ifdef HAVE_LOCK
static int xmp_lock(const char *path, struct fuse_file_info *fi, int cmd,
		    struct flock *lock)
{
	(void) path;

	return ulockmgr_op(fi->fh, cmd, lock, &fi->lock_owner,
			   sizeof(fi->lock_owner));
}

static int xmp_flock(const char *path, struct fuse_file_info *fi, int op)
{
	int res;
	(void) path;

	res = flock(fi->fh, op);
	if (res == -1)
		return -errno;

	return 0;
}
#endif /* HAVE_LOCK */

void *xmp_init(struct fuse_conn_info *conn)
{
	int ret;
	int format = 1;

	ret = nvfuse_init(format);
	if (ret < 0) {
		printf(" Error: nvfuse_init()\n");
		return NULL;
	}
	return NULL;
}

void xmp_destroy(void *data)
{
	int ret;
	ret = nvfuse_deinit();
	if (ret < 0) {
		printf(" Error: nvfuse_deinit()\n");
	}
}

static struct fuse_operations xmp_oper = {
	.init		= xmp_init,
	.destroy	= xmp_destroy,
	.getattr	= xmp_getattr,
	.fgetattr	= xmp_fgetattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.opendir	= xmp_opendir,
	.readdir	= xmp_readdir,
	.releasedir	= xmp_releasedir,
	.mknod		= xmp_mknod,
	.mkdir		= xmp_mkdir,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
	.ftruncate	= xmp_ftruncate,
#ifdef HAVE_UTIMENSAT
	.utimens	= xmp_utimens,
#endif
	.create		= xmp_create,
	.open		= xmp_open,
	.read		= xmp_read,
	//.read_buf	= xmp_read_buf,
	.write		= xmp_write,
	//.write_buf	= xmp_write_buf,
	.statfs		= xmp_statfs,
	//.flush		= xmp_flush,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
#ifdef HAVE_SETXATTR
	.setxattr	= xmp_setxattr,
	.getxattr	= xmp_getxattr,
	.listxattr	= xmp_listxattr,
	.removexattr	= xmp_removexattr,
#endif
#ifdef HAVE_LOCK
	.lock		= xmp_lock,
	.flock		= xmp_flock,
#endif
	.flag_nullpath_ok = 0,
#if HAVE_UTIMENSAT
	.flag_utime_omit_ok = 1,
#endif
};

/* main function */
int main(int argc, char *argv[])
{
	int ret;

	if (argc < 2) {
		printf("\n");
		printf("Usage: \n");
		printf("	#./fuse_example /mnt_point\n");
		printf("\n");
		return -1;
	}

	printf(" FUSE_USE_VERSION = %d \n", FUSE_USE_VERSION);

	umask(0);
	ret = fuse_main(argc, argv, &xmp_oper, NULL);
	if (ret < 0) {
		printf(" Error: fuse_main()\n");
		return ret;
	}

	return 0;
}
