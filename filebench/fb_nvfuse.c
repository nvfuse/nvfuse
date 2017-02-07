/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Portions Copyright 2008 Denis Cheng
 */

#include "config.h"
#include "filebench.h"
#include "flowop.h"
#include "threadflow.h" /* For aiolist definition */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <strings.h>

#include "filebench.h"
#include "fsplug.h"

/* nvfuse functions modification */
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"

static int fb_nvfuse_freemem(fb_fdesc_t *fd, off64_t size);
static int fb_nvfuse_open(fb_fdesc_t *, char *, int, int);
static int fb_nvfuse_pread(fb_fdesc_t *, caddr_t, fbint_t, off64_t);
static int fb_nvfuse_read(fb_fdesc_t *, caddr_t, fbint_t);
static int fb_nvfuse_pwrite(fb_fdesc_t *, caddr_t, fbint_t, off64_t);
static int fb_nvfuse_write(fb_fdesc_t *, caddr_t, fbint_t);
static int fb_nvfuse_lseek(fb_fdesc_t *, off64_t, int);
static int fb_nvfuse_truncate(fb_fdesc_t *, off64_t);
static int fb_nvfuse_rename(const char *, const char *);
static int fb_nvfuse_close(fb_fdesc_t *);
static int fb_nvfuse_link(const char *, const char *);
static int fb_nvfuse_symlink(const char *, const char *);
static int fb_nvfuse_unlink(char *);
static ssize_t fb_nvfuse_readlink(const char *, char *, size_t);
static int fb_nvfuse_mkdir(char *, int);
static int fb_nvfuse_rmdir(char *);
static DIR *fb_nvfuse_opendir(char *);
static struct dirent *fb_nvfuse_readdir(DIR *);
static int fb_nvfuse_closedir(DIR *);
static int fb_nvfuse_fsync(fb_fdesc_t *);
static int fb_nvfuse_stat(char *, struct stat64 *);
static int fb_nvfuse_fstat(fb_fdesc_t *, struct stat64 *);
static int fb_nvfuse_access(const char *, int);
static void fb_nvfuse_recur_rm(char *);

static fsplug_func_t fb_nvfuse_funcs =
{
	"locfs",                /* ------- desc ----- imp? --------- link --------- */
	fb_nvfuse_freemem,		/* flush page cache    x   -> fb_lfs_freemem        */
	fb_nvfuse_open,		    /* open                o   -> nvfuse_openfile_path  */
	fb_nvfuse_pread,		/* pread               o   -> nvfuse_readfile       */
	fb_nvfuse_read,		    /* read                o   -> nvfuse_readfile       */
	fb_nvfuse_pwrite,		/* pwrite              o   -> nvfuse_writefile      */
	fb_nvfuse_write,		/* write               o   -> nvfuse_writefile      */
	fb_nvfuse_lseek,		/* lseek               o   -> nvfuse_lseek          */
	fb_nvfuse_truncate,	    /* ftruncate           o   -> nvfuse_ftruncate      */
	fb_nvfuse_rename,		/* rename              o   -> nvfuse_rename_path    */
	fb_nvfuse_close,		/* close               o   -> nvfuse_closefile      */
	fb_nvfuse_link,		    /* link                o   -> nvfuse_hardlink_path  */
	fb_nvfuse_symlink,		/* symlink             o   -> nvfuse_symlink_path   */
	fb_nvfuse_unlink,		/* unlink              o   -> nvfuse_unlink         */
	fb_nvfuse_readlink,	    /* readlink            o   -> nvfuse_readlink       */
	fb_nvfuse_mkdir,		/* mkdir               o   -> nvfuse_mkdir_path     */
	fb_nvfuse_rmdir,		/* rmdir               o   -> nvfuse_rmdir_path     */
	fb_nvfuse_opendir,		/* opendir             o   -> nvfuse_opendir        */
	fb_nvfuse_readdir,		/* readdir             x   -> FIXME                 */
	fb_nvfuse_closedir,	    /* closedir            x   -> FIXME                 */
	fb_nvfuse_fsync,		/* fsync               o   -> nvfuse_fsyncc         */
	fb_nvfuse_stat,		    /* stat                o   -> nvfuse_getattr        */
	fb_nvfuse_fstat,		/* fstat               x   -> FIXME                 */
	fb_nvfuse_access,		/* access              o   -> nvfuse_access         */
	fb_nvfuse_recur_rm		/* recursive rm        o   -> fb_lfs_recur_rm       */
};

void
fb_nvfuse_funcvecinit(void)
{
	fs_functions_vec = &fb_nvfuse_funcs;

	char *argv[] = 	{ 
		"fuse_example",
		"-t spdk",
		"-d 01:00",  /* PCIe Slot Number */
		"-t block",
		"-d /dev/nvme0n1",
		"-f", /* format */
		"-m", /* mount */
		};
	int argc = 2;

	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(NULL, argc, argv);


}

static int
fb_nvfuse_freemem(fb_fdesc_t *fd, off64_t size)
{
    off64_t left;
	int ret = 0;

	for (left = size; left > 0; left -= MMAP_SIZE) {
		off64_t thismapsize;
		caddr_t addr;

		thismapsize = MIN(MMAP_SIZE, left);
		addr = mmap64(0, thismapsize, PROT_READ|PROT_WRITE,
		    MAP_SHARED, fd->fd_num, size - left);
		ret += msync(addr, thismapsize, MS_INVALIDATE);
		(void) munmap(addr, thismapsize);
	}
	return (ret);
}

static int
fb_nvfuse_pread(fb_fdesc_t *fd, caddr_t iobuf, fbint_t iosize, off64_t fileoffset)
{    
    return (nvfuse_readfile(nvh, fd->fd_num, iobuf, iosize, fileoffset));
}

static int
fb_nvfuse_read(fb_fdesc_t *fd, caddr_t iobuf, fbint_t iosize)
{
    return (nvfuse_readfile(nvh, fd->fd_num, iobuf, iosize, 0));
}

static int
fb_nvfuse_open(fb_fdesc_t *fd, char *path, int flags, int perms)
{
	if ((fd->fd_num = nvfuse_openfile_path(nvh, path, flags, perms)) < 0)
		return (FILEBENCH_ERROR);
	else
		return (FILEBENCH_OK);
}

static int
fb_nvfuse_unlink(char *path)
{
    return (nvfuse_unlink(nvh,path));
}

static ssize_t
fb_nvfuse_readlink(const char *path, char *buf, size_t buf_size)
{
    return (nvfuse_readlink(nvh, path, buf, buf_size-1));
}

static int
fb_nvfuse_fsync(fb_fdesc_t *fd)
{
	return (nvfuse_fsync(nvh,fd->fd_num));
}

static int
fb_nvfuse_lseek(fb_fdesc_t *fd, off64_t offset, int whence)
{
    return (nvfuse_lseek(nvh, fd->fd_num, offset, whence));
}

static int
fb_nvfuse_rename(const char *old, const char *new)
{
    return (nvfuse_rename_path(nvh,old, new));
}

static int
fb_nvfuse_close(fb_fdesc_t *fd)
{
    return (nvfuse_closefile(nvh, fd->fd_num));
}

static int
fb_nvfuse_mkdir(char *path, int perm)
{
    return (nvfuse_mkdir_path(nvh,path,perm));
}

static int
fb_nvfuse_rmdir(char *path)
{
    return (nvfuse_rmdir_path(nvh, path));
}

static void
fb_nvfuse_recur_rm(char *path)
{
	char cmd[MAXPATHLEN];

	(void) snprintf(cmd, sizeof (cmd), "rm -rf %s", path);

	/* We ignore system()'s return value */
	if (system(cmd));
	return;
}

static DIR *
fb_nvfuse_opendir(char *path)
{
    return (nvfuse_opendir(nvh, path));
}

static struct dirent *
fb_nvfuse_readdir(DIR *dirp)
{
    /*FIXME*/
//	return (readdir(dirp));
    return NULL;
}

static int
fb_nvfuse_closedir(DIR *dirp)
{
    /*FIXME*/
//	return (closedir(dirp));
    return 0;
}

static int
fb_nvfuse_fstat(fb_fdesc_t *fd, struct stat64 *statbufp)
{
    /*FIXME*/
    return 0;
}

static int
fb_nvfuse_stat(char *path, struct stat64 *statbufp)
{
    return (nvfuse_getattr(nvh, path, statbufp));
}

static int
fb_nvfuse_pwrite(fb_fdesc_t *fd, caddr_t iobuf, fbint_t iosize, off64_t offset)
{
    return (nvfuse_writefile(nvh, fd->fd_num, iobuf, iosize,offset));
}

static int
fb_nvfuse_write(fb_fdesc_t *fd, caddr_t iobuf, fbint_t iosize)
{
    return (nvfuse_writefile(nvh, fd->fd_num, iobuf, iosize));
}

static int
fb_nvfuse_truncate(fb_fdesc_t *fd, off64_t fse_size)
{
    return (nvfuse_ftruncate(nvh, fd->fd_num, fse_size));
}

static int
fb_nvfuse_link(const char *existing, const char *new)
{
    return (nvfuse_hardlink_path(nvh, existing,new));
}

static int
fb_nvfuse_symlink(const char *existing, const char *new)
{
    return (nvfuse_symlink_path(nvh, existing,new));
}

static int
fb_nvfuse_access(const char *path, int amode)
{
    return (nvfuse_access(nvh, path, amode));
}
