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
//#define NDEBUG
#include <assert.h>
#include <errno.h>

#include "nvfuse_config.h"
#if NVFUSE_OS == NVFUSE_OS_LINUX
#include <dirent.h>
#include <sys/uio.h>
#include <sys/sysmacros.h>
#endif

#ifdef SPDK_ENABLED
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include <rte_lcore.h>
#include <rte_memcpy.h>
#endif

#include "nvfuse_core.h"
#include "nvfuse_dep.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_inode_cache.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_indirect.h"
#include "nvfuse_bp_tree.h"
#include "nvfuse_malloc.h"
#include "nvfuse_api.h"
#include "nvfuse_dirhash.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_debug.h"
#include "nvfuse_reactor.h"

void nvfuse_core_usage(char *cmd)
{
	printf("\n Usage: \n\n");
	printf(" %s [nvfuse core options] [application options]\n", cmd);
	printf("\n");
	printf(" Options for NVFUSE core:\n");
	printf("\t-f: nvfuse format for primary process \n");
	printf("\t-m: nvfuse mount for primary process \n");
	printf("\t-q: driver qdepth \n");
	printf("\t-b: buffer size (in MB) for primary process\n");
	printf("\t-c: CPU core mask (e.g., 0x1 (default), 0x2, 0x4)\n");
	printf("\t-a: application name (e.g., rocksdb, fiebenc, redis)\n");
	printf("\t-p: pre-allocation of buffers and containers\n");
	printf("\t-o: configuration file (e.g., TransportID PCIe 01:00.0) \n");
}

void nvfuse_core_usage_example(char *cmd)
{
	printf(" Example:\n");
	printf(" #sudo %s -f -m -q 256 -b 2048 -c 0x4 -a rocksdb\n", cmd);
}

s8 *nvfuse_get_core_options()
{
	return "a:c:fmq:s:b:p:o:";
}

s32 nvfuse_is_core_option(s8 option)
{
	s8 *core_option_str = nvfuse_get_core_options();

	while (*core_option_str) {
		if (':' == *core_option_str) {
			core_option_str++;
			continue;
		}

		if (option == *core_option_str++)
			return 1;
	}

	return 0;
}

void nvfuse_distinguish_core_and_app_options(int argc, char **argv,
		int *core_argcp, char **core_argv,
		int *app_argcp, char **app_argv)
{
	int i;
	int core_argc = 0;
	int app_argc = 0;

	/*printf(" argc = %d \n", argc);
	for (i = 0;i < argc; i++)
	{
		printf("[%d] %s\n", i, argv[i]);
	}*/

	app_argv[0] = argv[0];
	app_argc++;
	core_argv[0] = argv[0];
	core_argc++;

	for (i = 1; i < argc;) {
		if (argv[i][0] == '-') {
			if (nvfuse_is_core_option(argv[i][1])) {
				core_argv[core_argc++] = argv[i++];
				while (i < argc && argv[i][0] != '-')
					core_argv[core_argc++] = argv[i++];
			} else {
				app_argv[app_argc++] = argv[i++];
				while (i < argc && argv[i][0] != '-')
					app_argv[app_argc++] = argv[i++];
			}
		} else {
			i++;
		}
	}

	*core_argcp = core_argc;
	*app_argcp = app_argc;

	/*printf(" App Args \n");
	for (i = 0;i < app_argc; i++)
	{
		printf("%d %s\n", i, app_argv[i]);
	}

	printf(" Core Args \n");
	for (i = 0;i < core_argc; i++)
	{
		printf("%d %s\n", i, core_argv[i]);
	}*/
}

s32 nvfuse_configure_spdk(struct nvfuse_ipc_context *ipc_ctx, struct nvfuse_params *params, s32 qdepth)
{
	sprintf(params->cpu_core_mask_str, "%d", params->cpu_core_mask);

	reactor_get_opts(params->config_file, params->cpu_core_mask_str, &params->opts);

#ifdef NVFUSE_USE_CEPH_SPDK
	spdk_app_init(&params->opts);
#endif

	/* open I/O manager */
	//nvfuse_open(target, 0);

	//dprintf_info(SPDK, " total blks = %ld \n", (long)io_manager->total_blkcount);

	return 0;
}

static char *ltrim(char *str)
{
	char buf[128];
	char *ptr;
	char *dst;

	memset(buf, 0x0, 128);

	dst = buf;
	ptr = (char *)str;

	while (ptr < str + strlen(str)) {
		if (!isspace(*ptr)) {
			break;
		}
		ptr++;
	}

	while (ptr < str + strlen(str)) {
		*dst = *ptr;
		dst++;
		ptr++;
	}

	strcpy(str, buf);

	return (char *)str;
}

s32 nvfuse_parse_args(int argc, char **argv, struct nvfuse_params *params)
{
	char *config_file = NULL;
	s32 cpu_core_mask = 0x1; /* 1 core set to as default */
	s8 *appname = NULL; /* e.g., rocksdb */
	s32 need_format = 0;
	s32 need_mount = 0;
	s32 qdepth = AIO_MAX_QDEPTH;
	s32 dev_size = 0; /* in MB units */
	s32 buffer_size = 0; /* in MB units */
	s32 preallocation = 0;
	s8 op;
	s8 *cmd;

	cmd = argv[0];

	if (argc == 1) {
		dprintf_error(API, " Invalid argc = %d \n", argc);
		goto PRINT_USAGE;
	}

	/* optind must be reset before using getopt() */
	optind = 0;
	/* f (format) and m (mount) don't have any argument. */
	while ((op = getopt(argc, argv, nvfuse_get_core_options())) != -1) {
		//printf("op = %c\n", op);
		switch (op) {
		case ' ':
			continue;
		case 'c':
			cpu_core_mask = atoi(optarg);
			break;
		case 'f':
			need_format = 1;
			break;
		case 'm':
			need_mount = 1;
			break;
		case 'a':
			appname = optarg;
			break;
		case 'o':
			config_file = optarg;
			break;
		case 'q':
			qdepth = atoi(optarg);
			if (qdepth == 0 || qdepth > AIO_MAX_QDEPTH) {
				dprintf_error(API, "Invalid qdepth value = %d (max qdepth = %d)\n", qdepth, AIO_MAX_QDEPTH);
			}
			break;
		case 's':
			dev_size = atoi(optarg);
			if (dev_size == 0) {
				dprintf_error(API, "Invalid dev size = %d MB)\n", dev_size);
			}
			break;
		case 'b':
			buffer_size = atoi(optarg);
			if (buffer_size == 0) {
				dprintf_error(API, "Invalid buffer size = %d MB)\n", buffer_size);
			}
			break;
		case 'p':
			preallocation = 1;
			break;
		default:
			dprintf_error(API, " Invalid op code %c in getopt()\n", op);
			goto PRINT_USAGE;
		}
	}

	if (!spdk_process_is_primary() && appname == NULL) {
		dprintf_error(API, "\n Application name with -a option is required.\n");
		goto PRINT_USAGE;
	}

	/* configure parametres received from cmmand line */
	if (appname)
		strcpy(params->appname, appname);
	else
		strcpy(params->appname, "primary");

	config_file = ltrim(config_file);
	if (config_file)
		strcpy(params->config_file, config_file);
	else
		goto PRINT_USAGE;

	params->cpu_core_mask	= cpu_core_mask;
	params->buffer_size		= buffer_size;
	params->qdepth			= qdepth;
	params->need_format		= need_format; /* no allowed for secondary processes */
	params->need_mount		= need_mount;
	params->preallocation	= preallocation;
#if 1
	dprintf_info(API, " appname = %s\n", params->appname);
	dprintf_info(API, " cpu core mask = %x\n", params->cpu_core_mask);
	dprintf_info(API, " qdepth = %d \n", params->qdepth);
	dprintf_info(API, " buffer size = %d MB\n", params->buffer_size);
	dprintf_info(API, " need format = %d \n", params->need_format);
	dprintf_info(API, " need mount = %d \n", params->need_mount);
	dprintf_info(API, " preallocation = %d \n", params->preallocation);
	dprintf_info(API, " config file = %s \n", params->config_file);
#endif

	return 0;

PRINT_USAGE:
	nvfuse_core_usage(cmd);
	nvfuse_core_usage_example(cmd);
	return -1;
}

#ifdef NVFUSE_USE_CEPH_SPDK
static inline u32 spdk_bdev_get_block_size(struct spdk_bdev *bdev)
{
	return bdev->blocklen;
}

static inline u64 spdk_bdev_get_num_blocks(struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

static inline s8 * spdk_bdev_get_name(struct spdk_bdev *bdev)
{
	return bdev->name;
}
#endif

struct nvfuse_handle *nvfuse_create_handle(struct nvfuse_ipc_context *ipc_ctx, struct nvfuse_params *params)
{
	struct nvfuse_handle *nvh;
	struct spdk_bdev *bdev;
	s32 ret;

	/* allocation of nvfuse handle */
	nvh = spdk_dma_zmalloc(sizeof(struct nvfuse_handle), 0, NULL);
	if (nvh == NULL) {
		dprintf_error(MEMALLOC, " Error: nvfuse_create_handle due to lack of free memory \n");
		return NULL;
	}

	nvh->nvh_target = reactor_construct_targets();
	bdev = nvh->nvh_target->bdev;
	printf(" blocklen = %ub blockcnt = %lu\n", spdk_bdev_get_block_size(bdev), spdk_bdev_get_num_blocks(bdev));

	nvh->blk_size = spdk_bdev_get_block_size(bdev);
	nvh->total_blkcount = (spdk_bdev_get_num_blocks(bdev) >> 3) << 3; 
	dprintf_info(SPDK, " NVMe: sector size = %d, number of sectors = %ld\n", nvh->blk_size, nvh->total_blkcount);
	dprintf_info(SPDK, " NVMe: total capacity = %0.3fTB\n",
		   (double)nvh->total_blkcount * nvh->blk_size / 1024 / 1024 / 1024 / 1024);

    /* copy instance of ipc_ctx */
    memcpy(&nvh->nvh_ipc_ctx, ipc_ctx, sizeof(struct nvfuse_ipc_context));

    /* copy instance of params */
    memcpy(&nvh->nvh_params, params, sizeof(struct nvfuse_params));

	crc32c_intel_probe();

	/* file system format */
	if (nvh->nvh_params.need_format) {
		if (spdk_process_is_primary()) {
			ret = nvfuse_format(nvh);
			if (ret < 0) {
				dprintf_error(FORMAT, "format() \n");
				return NULL;
			}
		} else {
			dprintf_info(IPC, "format skipped due to secondary process!\n");
		}
	}

	/* file system mount */
	if (nvh->nvh_params.need_mount) {
		memset(&nvh->nvh_sb, 0x00, sizeof(struct nvfuse_superblock));
		nvh->nvh_sb.sb_nvh = nvh; /* temporary */
		ret = nvfuse_mount(nvh);
		if (ret < 0) {
			dprintf_error(MOUNT, "mount() \n");
			return NULL;
		}
	}

	/* ipc init */
	ret = nvfuse_ipc_init(ipc_ctx);
	if (ret < 0) {
		dprintf_error(IPC, " ipc_init()\n");
		return NULL;
	}

	nvh->nvh_sb.sb_control_plane_buffer_size = nvh->nvh_params.buffer_size *
			(NVFUSE_MEGA_BYTES / CLUSTER_SIZE);
	nvh->nvh_sb.sb_is_primary_process = spdk_process_is_primary() ? 1 : 0;

	return nvh;
}

void nvfuse_deinit_spdk(struct nvfuse_ipc_context *ipc_ctx)
{
//	nvfuse_close(target);
}

void nvfuse_destroy_handle(struct nvfuse_handle *nvh, s32 deinit_iom, s32 need_umount)
{
	int ret;

	/* umount file system */
	if (need_umount) {
		ret = nvfuse_umount(nvh);
		if (ret < 0) {
			dprintf_error(MOUNT, "umount() \n");
		}
	}

	spdk_dma_free(nvh);

	nvfuse_ipc_exit(&nvh->nvh_ipc_ctx);
}

struct nvfuse_dir_entry * nvfuse_lookup_linear(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *dir_ictx, 
						struct nvfuse_inode *dir_inode, const s8 *filename, struct nvfuse_buffer_head **dir_bh_return)
{
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_dir_entry *dir = NULL;
	struct nvfuse_dir_entry *dir_last;
	u32 dentry_blk;
	u32 dentry_num;

	for (dentry_blk = 0; dentry_blk < NVFUSE_SIZE_TO_BLK(dir_inode->i_size); dentry_blk++) {
		/* get dir block buffer */
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, dentry_blk, READ,
					   NVFUSE_TYPE_META);
		if (dir_bh == NULL) {
			dprintf_error(BUFFER, "nvfuse_get_bh() \n");
			break;
		}

		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;

		if (dentry_blk < NVFUSE_SIZE_TO_BLK(dir_inode->i_size) - 1) {
			dentry_num = DIR_ENTRY_NUM;
		} else {
			dentry_num = dir_inode->i_links_count % DIR_ENTRY_NUM;
			if (dentry_num == 0)
				dentry_num = DIR_ENTRY_NUM;
		}

		dir_last = dir + dentry_num;

		while (dir < dir_last) {
			/* directory entry is found */
			if (dir->d_flag == DIR_USED && !strcmp(dir->d_filename, filename))
				goto FOUND;
		}

		nvfuse_release_bh(sb, dir_bh, HEAD, NVF_CLEAN);
	}

	/* not found */
	dir = NULL;

FOUND:

	*dir_bh_return = dir_bh;

	return dir;
}

struct nvfuse_dir_entry * nvfuse_lookup_bptree(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *dir_ictx,
												struct nvfuse_inode *dir_inode, const s8 *filename, 
												struct nvfuse_buffer_head **dir_bh_return)
{
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_dir_entry *dir = NULL;
	u32 dentry_idx = 0;
	s32 res;
	
	res = nvfuse_get_dir_indexing(sb, dir_inode, (char *)filename, &dentry_idx);
	if (res < 0) {
		goto NOT_FOUND;
	}

	/* dir entry found */
	if (dentry_idx) {
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, 
								NVFUSE_DENTRY_TO_BLK(dentry_idx), 
								READ, NVFUSE_TYPE_META);
		if (dir_bh == NULL) {
			goto NOT_FOUND;
		}
		dir = ((struct nvfuse_dir_entry *)dir_bh->bh_buf) + (dentry_idx % DIR_ENTRY_NUM);

		/* directory entry is found */
		if (dir->d_flag == DIR_USED && !strcmp(dir->d_filename, filename)) {
			goto FOUND;
		} else {
			/* FIXME: how can we handle this exception case? */
			dprintf_error(BPTREE, "No such file or directory = %s", filename);
			dprintf_error(BPTREE, "B+tree has inconsistency state \n");
			assert(0);
		}
	} else {
		/* in case of collision */
		/* TODO: how to replace tricky code? */
		dir = (struct nvfuse_dir_entry *)1;
	}

NOT_FOUND:

FOUND:
	*dir_bh_return = dir_bh;
	return dir;
}

/*
* namespace lookup function with given file name and paraent inode number
* result: found=0, not found=-1
*/
s32 nvfuse_lookup(struct nvfuse_superblock *sb,
		  struct nvfuse_inode_ctx **file_ictx,
		  struct nvfuse_dir_entry *file_entry,
		  const s8 *filename,
		  const s32 cur_dir_ino)
{
	struct nvfuse_inode_ctx *dir_ictx;
	struct nvfuse_inode *dir_inode = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_dir_entry *dir = NULL;
	s32 res = -1;

	dir_ictx = nvfuse_read_inode(sb, NULL, cur_dir_ino);
	if (dir_ictx == NULL)
		return res;

	dir_inode = dir_ictx->ictx_inode;

#if NVFUSE_USE_DIR_INDEXING == 1
	/* b+tree based index search */
	if (dir_inode->i_bpino) {
		dir = nvfuse_lookup_bptree(sb, dir_ictx, dir_inode, filename, &dir_bh);
		/* not found dentry */
		if (!dir) {
			res = -1;
			goto RES;
		/* found dentry */
		} else {
			if (dir_bh)
				goto FOUND;
			else
				goto LINEAR_SEARCH;
		}

		/* try linear search */
	} else {
		/* b+tree is not allocated? */
		res = -1;
		goto RES;
	}
#endif

LINEAR_SEARCH:

	/* naiive linear search */
	dir = nvfuse_lookup_linear(sb, dir_ictx, dir_inode, filename, &dir_bh);
	if (dir) 
		goto FOUND;
	else
		goto RES;

FOUND:

	if (file_ictx) {
		*file_ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
	}

	if (file_entry) {
		rte_memcpy(file_entry, dir, DIR_ENTRY_SIZE);
	}

	assert(dir->d_ino > 0 && dir->d_ino < sb->sb_no_of_inodes_per_bg * sb->sb_bg_num);
	res = 0;

RES:

	nvfuse_release_bh(sb, dir_bh, HEAD, NVF_CLEAN);
	nvfuse_release_inode(sb, dir_ictx, NVF_CLEAN);

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

s32 nvfuse_path_resolve(struct nvfuse_handle *nvh, const char *path, char *filename,
			struct nvfuse_dir_entry *direntry)
{
	s8 dirname[FNAME_SIZE];
	int res;

	if (path[0] == '/') {
		nvfuse_filename(path, filename);
		nvfuse_dirname(path, dirname);
		/* absolute directory path */
		res = nvfuse_path_open(nvh, dirname, filename, direntry);
	} else {
		nvfuse_filename(path, filename);
		/* relative directory path */
		res = nvfuse_path_open2(nvh, (s8 *)path, (s8 *)filename, direntry);
	}

	return res;
}

s32 nvfuse_opendir(struct nvfuse_handle *nvh, const char *path)
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	struct nvfuse_dir_entry dir_entry;
	unsigned int par_ino;
	int res;
	s8 filename[FNAME_SIZE];

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	par_ino = dir_entry.d_ino;

	if (strcmp(filename, "")) {
		if (nvfuse_lookup(sb, NULL, &dir_entry, filename, dir_entry.d_ino) < 0) {
			return NVFUSE_ERROR;
		}
		par_ino = dir_entry.d_ino;
	}

	return par_ino;
}

struct dirent *nvfuse_readdir(struct nvfuse_handle *nvh, inode_t par_ino, struct dirent *dentry,
			      off_t dir_offset)
{
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_buffer_head *dir_bh;
	struct nvfuse_dir_entry *dir;
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s64 dir_size;
	struct dirent *return_dentry = NULL;

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;
	dir_size = (s64)dir_offset * DIR_ENTRY_SIZE;

	if (dir_inode->i_size <= dir_size) {
		goto RES;
	}

	dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino,
			       NVFUSE_SIZE_TO_BLK((s64)dir_offset * DIR_ENTRY_SIZE), READ, NVFUSE_TYPE_META);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;

	dir += (dir_offset % DIR_ENTRY_NUM);

	if (nvfuse_dir_is_invalid(dir)) {
		return_dentry = NULL;
	} else {
		dentry->d_ino = dir->d_ino;
		strcpy(dentry->d_name, dir->d_filename);

		ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
		inode = ictx->ictx_inode;

		if (inode->i_type == NVFUSE_TYPE_DIRECTORY)
			dentry->d_type = DT_DIR;
		else
			dentry->d_type = DT_REG;

		nvfuse_release_inode(sb, ictx, NVF_CLEAN);

		return_dentry = dentry;
	}

	nvfuse_release_bh(sb, dir_bh, 0, 0);
RES:
	;

	nvfuse_release_inode(sb, dir_ictx, NVF_CLEAN);
	nvfuse_release_super(sb);

	return return_dentry;
}

s32 nvfuse_openfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, s32 flags,
		    s32 mode)
{
	struct nvfuse_dir_entry dir_temp;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_file_table *ft;
	s32 fid = -1;
	s32 res = 0;
	inode_t ino = 0;

	if (!strcmp(filename, "")) {
		dprintf_error(API, "invalid file name (length = %d)\n", (int)strlen(filename));
		return -1;
	}

	res = nvfuse_lookup(sb, NULL, &dir_temp, filename, par_ino);
	if (res < 0) {
		// no such file
		if (flags & O_RDWR || flags & O_CREAT) {
			res = nvfuse_createfile(sb, par_ino, filename, &ino, mode, 0);
			if (res < 0)
				return res;
#if 0
			if (res == NVFUSE_SUCCESS) {
				res = nvfuse_lookup(sb, NULL, &dir_temp, filename, par_ino);
				if (res < 0) {
					//printf(" debug \n");
					//res = nvfuse_lookup(sb, NULL, &dir_temp, filename, par_ino);
					fid = -1;
					goto RES;
				}
			}
#endif
		} else {
			res = nvfuse_lookup(sb, NULL, &dir_temp, filename, par_ino);
			//printf("cannot read or create file \n");
			fid = -1;
			goto RES;
		}
	} else {
		ino = dir_temp.d_ino;
	}

	ictx = nvfuse_read_inode(sb, NULL, ino);
	inode = ictx->ictx_inode;
	if (inode->i_type != NVFUSE_TYPE_FILE) {
		printf("This is not a file");
		fid = -1;
		goto RES;
	}

	fid = nvfuse_allocate_open_file_table(sb);
	if (fid < 0) {
		goto RES;
	}

	ft = nvfuse_get_file_table(sb, fid);
	ft->used = TRUE;
	ft->ino = inode->i_ino;
	ft->size = inode->i_size;
	ft->rwoffset = 0;
	ft->flags = flags;

	if (O_APPEND & flags)
		nvfuse_seek(sb, ft, inode->i_size, SEEK_SET);
	else
		nvfuse_seek(sb, ft, 0, SEEK_SET);

	nvfuse_release_inode(sb, ictx, 0 /*clean*/);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

RES:
	;
	nvfuse_release_super(sb);

	return (fid);
}

s32 nvfuse_openfile_path(struct nvfuse_handle *nvh, const char *path, int flags, int mode)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_superblock *sb;
	s8 filename[FNAME_SIZE];
	s32 fd;
	s32 res;

	memset(&dir_entry, 0x00, sizeof(struct nvfuse_dir_entry));

	nvfuse_lock();

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf(" %s: invalid path\n", __FUNCTION__);
		fd = -1;
	} else {
		sb = nvfuse_read_super(nvh);
		fd = nvfuse_openfile(sb, dir_entry.d_ino, filename, flags, mode);
		nvfuse_release_super(sb);
	}

	nvfuse_unlock();

	return fd;
}

s32 nvfuse_openfile_ino(struct nvfuse_superblock *sb, inode_t ino, s32 flags)
{
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_file_table *ft;
	s32 fid = -1;

	ictx = nvfuse_read_inode(sb, NULL, ino);
	inode = ictx->ictx_inode;

	if (inode->i_type != NVFUSE_TYPE_FILE) {
		dprintf_error(API, "This is not a file (type = %d).", inode->i_type);
		return -1;
	}

	fid = nvfuse_allocate_open_file_table(sb);
	if (fid < 0) {
		return NVFUSE_ERROR;
	}

	ft = nvfuse_get_file_table(sb, fid);
	ft->used = TRUE;
	ft->ino = ino;
	ft->size = inode->i_size;
	ft->rwoffset = 0;

	if (O_APPEND & flags)
		nvfuse_seek(sb, ft, inode->i_size, SEEK_SET);
	else
		nvfuse_seek(sb, ft, 0, SEEK_SET);

	nvfuse_release_inode(sb, ictx, NVF_CLEAN);
	return (fid);
}

s32 nvfuse_closefile(struct nvfuse_handle *nvh, s32 fid)
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);

	/* FIXME: flush bhs and bcs related to inode that fd points out */

	nvfuse_close_file_table(sb, fid);
	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_readfile_core(struct nvfuse_superblock *sb, u32 fid, s8 *buffer, s32 count,
			 nvfuse_off_t roffset, s32 sync_read)
{
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_buffer_head *bh;
	struct nvfuse_file_table *of;

	s32 offset, remain, rcount = 0;

	of = nvfuse_get_file_table(sb, fid);

	ictx = nvfuse_read_inode(sb, NULL, of->ino);
	inode = ictx->ictx_inode;

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
	if (roffset) {
		of->rwoffset = roffset;
	}
#else
	of->rwoffset = roffset;
#endif

	while (count > 0 && of->rwoffset < inode->i_size) {

		bh = nvfuse_get_bh(sb, ictx, inode->i_ino, NVFUSE_SIZE_TO_BLK(of->rwoffset), sync_read,
				   NVFUSE_TYPE_DATA);
		if (bh == NULL) {
			dprintf_error(BUFFER, " read error \n");
			goto RES;
		}

		offset = of->rwoffset & (CLUSTER_SIZE - 1);
		remain = CLUSTER_SIZE - offset;

		if (remain > count)
			remain = count;

		if (sync_read)
			rte_memcpy(buffer + rcount, &bh->bh_buf[offset], remain);

		rcount += remain;
		of->rwoffset += remain;
		count -= remain;
		nvfuse_release_bh(sb, bh, 0, NVF_CLEAN);
	}

RES:
	;
	nvfuse_release_inode(sb, ictx, NVF_CLEAN);

	return rcount;
}

s32 nvfuse_readfile_directio_core(struct nvfuse_superblock *sb, u32 fid, s8 *buffer, s32 count,
				  nvfuse_off_t roffset, s32 sync_read)
{
	struct nvfuse_inode_ctx *ictx = NULL;
	struct nvfuse_inode *inode;
	struct nvfuse_file_table *of;
	s32 rcount = 0;

	of = nvfuse_get_file_table(sb, fid);

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
	if (roffset) {
		of->rwoffset = roffset;
	}
#else
	of->rwoffset = roffset;
#endif

	if (count % CLUSTER_SIZE) {
		dprintf_error(API, "count is not aligned to 4KB.");
		rcount = 0;
		goto RET;
	}

	ictx = nvfuse_read_inode(sb, NULL, of->ino);
	inode = ictx->ictx_inode;
	if (inode->i_size < roffset + count) {
		rcount = 0;
		goto RET;
	} else {
		rcount = count;
	}

	of->rwoffset += count;
	rcount = count;

	if (of->rwoffset > of->size)
		of->size = of->rwoffset;

RET:
	if (ictx)
		nvfuse_release_inode(sb, ictx, NVF_CLEAN);

	return rcount;
}

s32 nvfuse_readfile(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset)
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s32 rcount;

	rcount = nvfuse_readfile_core(sb, fid, buffer, count, roffset, READ);

	nvfuse_release_super(sb);
	return rcount;
}

s32 nvfuse_readfile_aio(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count,
			nvfuse_off_t roffset)
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s32 rcount;

	rcount = nvfuse_readfile_core(sb, fid, buffer, count, roffset, 0 /* no sync read */);

	nvfuse_release_super(sb);
	return rcount;
}

s32 nvfuse_readfile_aio_directio(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count,
				 nvfuse_off_t roffset)
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s32 rcount;

	rcount = nvfuse_readfile_directio_core(sb, fid, buffer, count, roffset, 0 /* no sync read */);

	nvfuse_release_super(sb);
	return rcount;
}


s32 nvfuse_writefile_core(struct nvfuse_superblock *sb, s32 fid, const s8 *user_buf, u32 count,
			  nvfuse_off_t woffset)
{
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_file_table *of;
	struct nvfuse_buffer_head *bh = NULL;
	u32 offset = 0, remain = 0, wcount = 0;
	lbno_t lblock = 0;
	int ret;

	of = nvfuse_get_file_table(sb, fid);

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
	if (woffset) {
		of->rwoffset = woffset;
	}
#else
	of->rwoffset = woffset;
#endif

	while (count > 0) {

		ictx = nvfuse_read_inode(sb, NULL, of->ino);
		inode = ictx->ictx_inode;

		lblock = NVFUSE_SIZE_TO_BLK(of->rwoffset);
		offset = of->rwoffset & (CLUSTER_SIZE - 1);
		remain = CLUSTER_SIZE - offset;
		if (remain > count)
			remain = count;

		if (count && inode->i_size <= of->rwoffset) {
			ret = nvfuse_get_block(sb, ictx, NVFUSE_SIZE_TO_BLK(inode->i_size), 1/* num block */, NULL, NULL,
					       1);
			if (ret) {
				dprintf_error(INODE, "data block allocation fails.");
				return NVFUSE_ERROR;
			}
		}

		/*read modify write or partial write */
		if (remain != CLUSTER_SIZE)
			bh = nvfuse_get_bh(sb, ictx, inode->i_ino, lblock, READ, NVFUSE_TYPE_DATA);
		else
			bh = nvfuse_get_bh(sb, ictx, inode->i_ino, lblock, WRITE, NVFUSE_TYPE_DATA);

		rte_memcpy(&bh->bh_buf[offset], user_buf + wcount, remain);

		wcount += remain;
		of->rwoffset += remain;
		count -= remain;

		if (of->rwoffset > of->size)
			of->size = of->rwoffset;

		inode->i_type = NVFUSE_TYPE_FILE;
		inode->i_size = of->size;
		assert(inode->i_size < MAX_FILE_SIZE);

		nvfuse_release_bh(sb, bh, 0, DIRTY);
		nvfuse_release_inode(sb, ictx, DIRTY);
	}

	if (of->flags & O_SYNC) {
		if (of->flags & __O_SYNC) {
			ictx = nvfuse_read_inode(sb, NULL, of->ino);
			nvfuse_fsync_ictx(sb, ictx);
			nvfuse_release_inode(sb, ictx, NVF_CLEAN);
		} else { /* in case of O_DSYNC*/
			ictx = nvfuse_read_inode(sb, NULL, of->ino);
			nvfuse_fsync_ictx(sb, ictx);
			nvfuse_release_inode(sb, ictx, NVF_CLEAN);
		}
		/* flush cmd to nvme ssd */
		reactor_sync_flush(sb->target);
	}

	return wcount;
}

s32 nvfuse_writefile_directio_core(struct nvfuse_superblock *sb, s32 fid, const s8 *user_buf,
				   u32 count, nvfuse_off_t woffset)
{
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_file_table *of;
	u32 wcount = 0;
	int ret;

	of = nvfuse_get_file_table(sb, fid);

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
	if (woffset) {
		of->rwoffset = woffset;
	}
#else
	of->rwoffset = woffset;
#endif

	if (count % CLUSTER_SIZE) {
		dprintf_error(API, "count (%d) is not aligned to 4KB.", count);
		wcount = 0;
		goto RET;
	}

	ictx = nvfuse_read_inode(sb, NULL, of->ino);
	inode = ictx->ictx_inode;
	if (count && inode->i_size <= of->rwoffset) {
		u32 num_alloc = count >> CLUSTER_SIZE_BITS;
		ret = nvfuse_get_block(sb, ictx, NVFUSE_SIZE_TO_BLK(inode->i_size), num_alloc/* num block */, NULL,
				       NULL, 1);
		if (ret) {
			dprintf_error(INODE, "data block allocation fails.");
			return NVFUSE_ERROR;
		}

		assert(inode->i_size < MAX_FILE_SIZE);
		inode->i_size += count;
		nvfuse_release_inode(sb, ictx, DIRTY);
	} else {
		nvfuse_release_inode(sb, ictx, NVF_CLEAN);
	}

	of->rwoffset += count;
	wcount = count;

	if (of->rwoffset > of->size)
		of->size = of->rwoffset;

RET:
	;

	return wcount;
}

s32 nvfuse_writefile(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf, u32 count,
		     nvfuse_off_t woffset)
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s32 wcount;

	wcount = nvfuse_writefile_core(sb, fid, user_buf, count, woffset);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

	nvfuse_release_super(sb);

	return wcount;
}

s32 nvfuse_writefile_directio_prepare(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf,
				      u32 count, nvfuse_off_t woffset)
{
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s32 wcount;

	wcount = nvfuse_writefile_directio_core(sb, fid, user_buf, count, woffset);

	nvfuse_release_super(sb);

	return wcount;
}


s32 nvfuse_gather_bh(struct nvfuse_superblock *sb, s32 fid, const s8 *user_buf, u32 count,
		     nvfuse_off_t woffset, struct list_head *aio_bh_head, s32 *aio_bh_count)
{
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_file_table *of;
	struct nvfuse_buffer_head *bh = NULL;
	u32 offset = 0, remain = 0;
	nvfuse_off_t curoffset;
	lbno_t lblock = 0;

	of = nvfuse_get_file_table(sb, fid);

	curoffset = woffset;

	ictx = nvfuse_read_inode(sb, NULL, of->ino);
	inode = ictx->ictx_inode;

	while (count > 0) {
		lblock = NVFUSE_SIZE_TO_BLK(curoffset);
		offset = curoffset & (CLUSTER_SIZE - 1);
		remain = CLUSTER_SIZE - offset;

		if (remain > count)
			remain = count;

		bh = nvfuse_get_bh(sb, ictx, inode->i_ino, lblock, WRITE, NVFUSE_TYPE_DATA);
		if (bh == NULL) {
			dprintf_error(BUFFER, "get_bh()\n");
			return -1;
		}
		curoffset += remain;
		count -= remain;
		(*aio_bh_count)++;

		list_add(&bh->bh_aio_list, aio_bh_head);
	}

	nvfuse_release_inode(sb, ictx, NVF_CLEAN);

	return 0;
}

static inline u16 old_encode_dev(dev_t dev)
{
	return (major(dev) << 8) | minor(dev);
}

static inline u32 new_encode_dev(dev_t dev)
{
	unsigned major = major(dev);
	unsigned minor = minor(dev);
	return (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
}

static inline int old_valid_dev(dev_t dev)
{
	return major(dev) < 256 && minor(dev) < 256;
}

static inline dev_t old_decode_dev(u16 val)
{
	return makedev((val >> 8) & 255, val & 255);
}

static inline dev_t new_decode_dev(u32 dev)
{
	unsigned major = (dev & 0xfff00) >> 8;
	unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);
	return makedev(major, minor);
}

s32 nvfuse_createfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, inode_t *new_ino,
		      mode_t mode, dev_t dev)
{
	struct nvfuse_dir_entry *dir;
	struct nvfuse_inode_ctx *new_ictx, *dir_ictx;
	struct nvfuse_inode *new_inode, *dir_inode;
	struct nvfuse_buffer_head *dir_bh = NULL;
	u32 search_lblock, search_entry;
	u32 empty_dentry;
	inode_t alloc_ino;
	s32 ret;

	if (strlen(filename) < 1 || strlen(filename) >= FNAME_SIZE) {
		printf("create file : name  = %s, %d\n", filename, (int)strlen(filename));
		return -1;
	}

#if 0
	if (!nvfuse_lookup(sb, NULL, NULL, fiename, par_ino))
		return error_msg(" exist file or directory\n");
#endif

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;

	if (dir_inode->i_links_count == MAX_FILES_PER_DIR) {
		printf(" The number of files exceeds %d\n", MAX_FILES_PER_DIR);
		return -1;
	}

#ifdef NVFUSE_USE_DELAYED_DIRECTORY_ALLOC
	if (dir_inode->i_links_count == 2 && dir_inode->i_bpino == 0) {
		ret = nvfuse_make_first_directory(sb, dir_ictx, dir_inode);
		if (ret) {
			dprintf_error(DIRECTORY, "mkdir_first_directory()\n");
			return -1;
		}
	}
#endif

	/* find an empty directory */
	empty_dentry = nvfuse_find_empty_dentry(sb, dir_ictx, dir_inode);
	if (empty_dentry < 0) {
		return -1;
	}
	search_lblock = empty_dentry / DIR_ENTRY_NUM;
	search_entry = empty_dentry % DIR_ENTRY_NUM;

#ifdef NVFUSE_USE_DELAYED_BPTREE_CREATION
	if (dir_inode->i_bpino == 0 && dir_inode->i_links_count == 2) {
		/* create bptree related nodes for new directory's dentries */
		ret = nvfuse_create_bptree(sb, dir_inode);
		if (ret) {
			dprintf_error(BPTREE, " bptree allocation fails.");
			return NVFUSE_ERROR;
		}
	}
#endif

	dir_inode->i_links_count++;
	dir_inode->i_ptr = search_lblock * DIR_ENTRY_NUM + search_entry;
	assert(dir_inode->i_links_count == dir_inode->i_ptr + 1);

	new_ictx = nvfuse_alloc_ictx(sb);
	if (new_ictx == NULL)
		return -1;

	SPINLOCK_LOCK(&new_ictx->ictx_lock);
	set_bit(&new_ictx->ictx_status, INODE_STATE_LOCK);
	set_bit(&new_ictx->ictx_status, INODE_STATE_DIRTY);

	alloc_ino = nvfuse_alloc_new_inode(sb, new_ictx);
	if (alloc_ino == 0) {
		printf(" It runs out of free inodes.");
		return -1;
	}

	new_ictx = nvfuse_read_inode(sb, new_ictx, alloc_ino);
	nvfuse_insert_ictx(sb, new_ictx);

	new_inode = new_ictx->ictx_inode;
	new_inode->i_type = NVFUSE_TYPE_FILE;
	new_inode->i_size = 0;
	new_inode->i_mode = mode;
	new_inode->i_gid = 0;
	new_inode->i_uid = 0;
	new_inode->i_links_count = 1;
	new_inode->i_atime = time(NULL);
	new_inode->i_ctime = time(NULL);
	new_inode->i_mtime = time(NULL);

	if (S_ISCHR(mode) || S_ISBLK(mode)) {
		if (old_valid_dev(dev))
			new_inode->i_blocks[0] = old_encode_dev(dev);
		else
			new_inode->i_blocks[1] = new_encode_dev(dev);
	}

	if (new_ino)
		*new_ino = new_inode->i_ino;

	dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, search_lblock, READ, NVFUSE_TYPE_META);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
	dir[search_entry].d_flag = DIR_USED;
	dir[search_entry].d_ino = new_inode->i_ino;
	dir[search_entry].d_version = new_inode->i_version;
	strcpy(dir[search_entry].d_filename, filename);

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_set_dir_indexing(sb, dir_inode, filename, dir_inode->i_ptr);
#endif

	nvfuse_release_bh(sb, dir_bh, 0/*tail*/, DIRTY);

	nvfuse_release_inode(sb, new_ictx, DIRTY);
	nvfuse_release_inode(sb, dir_ictx, DIRTY);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_shrink_dentry(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, u32 to_entry,
			 u32 from_entry)
{
	struct nvfuse_buffer_head *dir_bh_from;
	struct nvfuse_dir_entry *dir_from;

	struct nvfuse_buffer_head *dir_bh_to;
	struct nvfuse_dir_entry *dir_to;

	struct nvfuse_inode *inode;
	s32 is_diff_block = 0;

	if (to_entry == from_entry)
		return -1;

	inode = ictx->ictx_inode;

	assert(to_entry < from_entry);

	dir_bh_from = nvfuse_get_bh(sb, ictx, inode->i_ino,
				    NVFUSE_SIZE_TO_BLK((s64)from_entry * DIR_ENTRY_SIZE), READ, NVFUSE_TYPE_META);

	dir_from = (struct nvfuse_dir_entry *)dir_bh_from->bh_buf;
	dir_from += (from_entry % DIR_ENTRY_NUM);
	assert(dir_from->d_flag != DIR_DELETED);

	is_diff_block = (NVFUSE_SIZE_TO_BLK((s64)to_entry * DIR_ENTRY_SIZE) != 
					NVFUSE_SIZE_TO_BLK((s64)from_entry * DIR_ENTRY_SIZE)) ? 1 : 0;

	if (is_diff_block) {
		dir_bh_to = nvfuse_get_bh(sb, ictx, inode->i_ino,
				  NVFUSE_SIZE_TO_BLK((s64)to_entry * DIR_ENTRY_SIZE), READ, NVFUSE_TYPE_META);
		dir_to = (struct nvfuse_dir_entry *)dir_bh_to->bh_buf;
	} else {
		dir_to = (struct nvfuse_dir_entry *)dir_bh_from->bh_buf;
	}

	dir_to += (to_entry % DIR_ENTRY_NUM);
	assert(dir_to->d_flag == DIR_DELETED);

	rte_memcpy(dir_to, dir_from, DIR_ENTRY_SIZE);

	dir_from->d_flag = DIR_DELETED; /* FIXME: zeroing ?*/
	//printf(" shrink_dentry: deleted = %s \n", dir_from->d_filename);


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

	nvfuse_release_bh(sb, dir_bh_from, 0, DIRTY);
	if (is_diff_block)
		nvfuse_release_bh(sb, dir_bh_to, 0, DIRTY);

	return 0;
}

s32 nvfuse_rmfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename)
{
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode = NULL;
	struct nvfuse_dir_entry *dir = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;
	u32 found_entry;
	u32 search_lblock, search_entry;

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;

	/* find an existing dentry */
	found_entry = nvfuse_find_existing_dentry(sb, dir_ictx, dir_inode, filename);
	if (found_entry < 0)
		return 0;

	search_lblock = found_entry / DIR_ENTRY_NUM;
	search_entry = found_entry % DIR_ENTRY_NUM;
	dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, search_lblock, READ,
						   NVFUSE_TYPE_META);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
	dir += search_entry;

	ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
	inode = ictx->ictx_inode;

	if (inode == NULL || inode->i_ino == 0) {
		dprintf_error(INODE, " inode file (%s) is not found.\n", filename);
		nvfuse_release_bh(sb, dir_bh, 0/*tail*/, NVF_CLEAN);
		return NVFUSE_ERROR;
	}

	if (inode->i_type == NVFUSE_TYPE_DIRECTORY) {
		dprintf_error(INODE, " rmfile() is prohibited due to directory (%s)n", filename);
		return NVFUSE_ERROR;
	}

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_del_dir_indexing(sb, dir_inode, filename);
#endif

	inode->i_links_count--;
	if (inode->i_links_count == 0) {
#ifdef VERIFY_BEFORE_RM_FILE
		nvfuse_fallocate_verify(sb, ictx, 0, NVFUSE_SIZE_TO_BLK(inode->i_size));
#endif
		nvfuse_free_inode_size(sb, ictx, 0/*size*/);
		nvfuse_relocate_delete_inode(sb, ictx);
	} else {
		nvfuse_release_inode(sb, ictx, DIRTY);
	}

	dir_inode->i_links_count--;
	dir_inode->i_ptr = dir_inode->i_links_count - 1;
	dir->d_flag = DIR_DELETED;

	nvfuse_release_bh(sb, dir_bh, 0/*tail*/, DIRTY);

	/* Shrink directory entry that last entry is moved to delete entry. */
	nvfuse_shrink_dentry(sb, dir_ictx, found_entry, dir_inode->i_links_count);

	/* Free block reclaimation is necessary but test is required. */
	if ((dir_inode->i_links_count * DIR_ENTRY_SIZE) % CLUSTER_SIZE == 0) {
		//nvfuse_truncate_blocks(sb, dir_ictx, (u64)dir_inode->i_links_count * DIR_ENTRY_SIZE);
		nvfuse_free_inode_size(sb, dir_ictx, (u64)dir_inode->i_links_count * DIR_ENTRY_SIZE);
		dir_inode->i_size -= CLUSTER_SIZE;
	}

	nvfuse_release_inode(sb, dir_ictx, DIRTY);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
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
		printf(" %s: invalid path\n", __FUNCTION__);
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
	struct nvfuse_dir_entry *dir = NULL;
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode = NULL, *inode = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;
	u32 found_entry;
	u32 search_lblock, search_entry;

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;

	/* find an existing dentry */
	found_entry = nvfuse_find_existing_dentry(sb, dir_ictx, dir_inode, filename);
	if (found_entry < 0)
		return 0;

	search_lblock = found_entry / DIR_ENTRY_NUM;
	search_entry = found_entry % DIR_ENTRY_NUM;
	dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, search_lblock, READ,
						   NVFUSE_TYPE_META);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
	dir += search_entry;

	ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
	inode = ictx->ictx_inode;
	if (inode == NULL || inode->i_ino == 0) {
		dprintf_error(INODE, " dir (%s) is not found this directory\n", filename);
		nvfuse_release_bh(sb, dir_bh, 0/*tail*/, NVF_CLEAN);
		return NVFUSE_ERROR;
	}

	if (inode->i_ino == sb->sb_root_ino) {
		printf(" root inode (%d) cannot be removed in nvfuse file system\n", sb->sb_root_ino);
		return -1;
	}

	if (inode->i_type == NVFUSE_TYPE_FILE) {
		dprintf_error(INODE, "rmdir is prohibited due to file (%s).\n", filename);
		return NVFUSE_ERROR;
	}

	if (strcmp(dir->d_filename, filename)) {
		dprintf_error(INODE, " filename is different\n");
		return NVFUSE_ERROR;
	}

	if (inode->i_links_count > 2) {
		dprintf_error(INODE, " the current dir has link count = %d\n", inode->i_links_count);
		return NVFUSE_ERROR;
	}

	dir->d_flag = DIR_DELETED;
	dir_inode->i_links_count--;
	dir_inode->i_ptr = dir_inode->i_links_count - 1;

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_del_dir_indexing(sb, dir_inode, filename);
#endif
	/* delete allocated b+tree inode */
	if (inode->i_bpino) {
		struct nvfuse_inode_ctx *bp_ictx;
		bp_ictx = nvfuse_read_inode(sb, NULL, inode->i_bpino);
		nvfuse_free_inode_size(sb, bp_ictx, 0);
		nvfuse_relocate_delete_inode(sb, bp_ictx);
	}

	/* Current Directory inode Deletion*/
	nvfuse_free_inode_size(sb, ictx, 0);
	nvfuse_relocate_delete_inode(sb, ictx);

	nvfuse_release_bh(sb, dir_bh, 0/*tail*/, DIRTY);

	/* Shrink directory entry that last entry is moved to delete entry. */
	nvfuse_shrink_dentry(sb, dir_ictx, found_entry, dir_inode->i_links_count);

	/* Free block reclaimation is necessary but test is required. */
	if ((dir_inode->i_links_count * DIR_ENTRY_SIZE) % CLUSTER_SIZE == 0) {
		//nvfuse_truncate_blocks(sb, dir_ictx, (u64)dir_inode->i_links_count * DIR_ENTRY_SIZE);
		nvfuse_free_inode_size(sb, dir_ictx, (u64)dir_inode->i_links_count * DIR_ENTRY_SIZE);
		dir_inode->i_size -= CLUSTER_SIZE;
	}

	/* Parent Directory Modification */
	nvfuse_release_inode(sb, dir_ictx, DIRTY);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
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
		printf(" %s: invalid path\n", __FUNCTION__);
		res = -1;
	} else {
		sb = nvfuse_read_super(nvh);
		res = nvfuse_rmdir(sb, dir_entry.d_ino, filename);
		nvfuse_release_super(sb);
	}

	nvfuse_unlock();
	return res;
}

s32 nvfuse_make_first_directory(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx,
				struct nvfuse_inode *inode)
{
	struct nvfuse_buffer_head *dir_bh;
	struct nvfuse_dir_entry *dir;
	s32 ret;

	assert(inode->i_size == 0);

	ret = nvfuse_get_block(sb, ictx, NVFUSE_SIZE_TO_BLK(inode->i_size), 1/* num block */, NULL, NULL,
			       1);
	if (ret) {
		dprintf_error(INODE, "data block allocation fails.");
		return NVFUSE_ERROR;
	}
	inode->i_size = CLUSTER_SIZE;

	nvfuse_mark_inode_dirty(ictx);

	dir_bh = nvfuse_get_bh(sb, ictx, inode->i_ino, 0, WRITE, NVFUSE_TYPE_META);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;

	strcpy(dir[0].d_filename, "."); // current dir
	dir[0].d_ino = inode->i_ino;
	dir[0].d_flag = DIR_USED;

	strcpy(dir[1].d_filename, ".."); // parent dir
	dir[1].d_ino = inode->i_ino;
	dir[1].d_flag = DIR_USED;

	nvfuse_release_bh(sb, dir_bh, 0, DIRTY);

	return 0;
}

s32 nvfuse_mkdir(struct nvfuse_superblock *sb, const inode_t par_ino, const s8 *dirname,
		 inode_t *new_ino, const mode_t mode)
{
	struct nvfuse_dir_entry *dir;
	struct nvfuse_inode_ctx *new_ictx, *dir_ictx;
	struct nvfuse_inode *new_inode = NULL, *dir_inode = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;
	u32 search_lblock = 0, search_entry = 0;
	u32 empty_dentry;
	inode_t alloc_ino;
	s32 ret;


	if (strlen(dirname) < 1 || strlen(dirname) >= FNAME_SIZE) {
		dprintf_error(API, " the size of the dir name  = %s is %d greater than %d\n", dirname, 
				(int)strlen(dirname), FNAME_SIZE);
		ret = NVFUSE_ERROR;
		goto RET;
	}

#if 1
	if (!nvfuse_lookup(sb, NULL, NULL, dirname, par_ino)) {
		dprintf_error(API, " exist file or directory\n");
		ret = NVFUSE_ERROR;
		goto RET;
	}
#endif

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;

	if (dir_inode->i_links_count == MAX_FILES_PER_DIR) {
		dprintf_error(DIRECTORY, " The number of files exceeds %d\n", MAX_FILES_PER_DIR);
		return -1;
	}
	assert(dir_inode->i_ptr + 1 == dir_inode->i_links_count);

#ifdef NVFUSE_USE_DELAYED_DIRECTORY_ALLOC
	if (dir_inode->i_links_count == 2 && dir_inode->i_bpino == 0) {
		ret = nvfuse_make_first_directory(sb, dir_ictx, dir_inode);
		if (ret) {
			dprintf_error(INODE, "mkdir_first_directory()\n");
			return -1;
		}
	}
#endif

	/* find an empty directory */
	empty_dentry = nvfuse_find_empty_dentry(sb, dir_ictx, dir_inode);
	if (empty_dentry < 0) {
		return -1;
	}
	search_lblock = empty_dentry / DIR_ENTRY_NUM;
	search_entry = empty_dentry % DIR_ENTRY_NUM;

#ifdef NVFUSE_USE_DELAYED_BPTREE_CREATION
	if (dir_inode->i_bpino == 0 && dir_inode->i_links_count == 2) {
		/* create bptree related nodes for new directory's dentries */
		ret = nvfuse_create_bptree(sb, dir_inode);
		if (ret) {
			dprintf_error(BPTREE, " bptree allocation fails.");
			return NVFUSE_ERROR;
		}
	}
#endif

	dir_inode->i_links_count++;
	dir_inode->i_ptr = search_lblock * DIR_ENTRY_NUM + search_entry;
	assert(dir_inode->i_links_count == dir_inode->i_ptr + 1);

	new_ictx = nvfuse_alloc_ictx(sb);
	if (new_ictx == NULL)
		return -1;

	SPINLOCK_LOCK(&new_ictx->ictx_lock);
	set_bit(&new_ictx->ictx_status, INODE_STATE_LOCK);
	set_bit(&new_ictx->ictx_status, INODE_STATE_DIRTY);

	alloc_ino = nvfuse_alloc_new_inode(sb, new_ictx);
	if (alloc_ino == 0) {
		printf(" It runs out of free inodes.");
		return -1;
	}

	new_ictx = nvfuse_read_inode(sb, new_ictx, alloc_ino);
	nvfuse_insert_ictx(sb, new_ictx);

	new_inode = new_ictx->ictx_inode;
	new_inode->i_type = NVFUSE_TYPE_DIRECTORY;
#ifndef NVFUSE_USE_DELAYED_DIRECTORY_ALLOC
	new_inode->i_size = CLUSTER_SIZE;
#else
	new_inode->i_size = 0;
#endif
	new_inode->i_ptr = 1;
	new_inode->i_mode = (mode & 0777) | S_IFDIR;
	new_inode->i_gid = 0;
	new_inode->i_uid = 0;
	new_inode->i_links_count = 2;
	new_inode->i_atime = time(NULL);
	new_inode->i_ctime = time(NULL);
	new_inode->i_mtime = time(NULL);

	if (new_ino)
		*new_ino = new_inode->i_ino;

	dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, search_lblock, READ, NVFUSE_TYPE_META);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
	dir[search_entry].d_flag = DIR_USED;
	dir[search_entry].d_ino = new_inode->i_ino;
	dir[search_entry].d_version = new_inode->i_version;
	strcpy(dir[search_entry].d_filename, dirname);

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_set_dir_indexing(sb, dir_inode, (char *)dirname, dir_inode->i_ptr);
#endif

#ifndef NVFUSE_USE_DELAYED_DIRECTORY_ALLOC
	ret = nvfuse_make_first_directory(sb, new_ictx, new_inode);
	if (ret) {
		dprintf_error(DIR, "mkdir_first_directory()\n");
		return -1;
	}
#endif

#ifndef NVFUSE_USE_DELAYED_BPTREE_CREATION
	/* create bptree related nodes for new directory's dentries */
	ret = nvfuse_create_bptree(sb, new_inode);
	if (ret) {
		dprintf_error(BPTREE, " bptree allocation fails.");
		return NVFUSE_ERROR;
	}
#else
	new_inode->i_bpino = 0; /* marked as unallocated */
#endif

	nvfuse_release_bh(sb, dir_bh, 0 /*tail*/, DIRTY);

	nvfuse_release_inode(sb, dir_ictx, DIRTY);
	nvfuse_release_inode(sb, new_ictx, DIRTY);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

	ret = NVFUSE_SUCCESS;

RET:

	return NVFUSE_SUCCESS;
}

s32 nvfuse_rename(struct nvfuse_handle *nvh, inode_t par_ino, s8 *name, inode_t new_par_ino,
		  s8 *newname)
{
	struct nvfuse_superblock *sb;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	inode_t ino = 0;

	sb = nvfuse_read_super(nvh);

	nvfuse_rm_direntry(sb, par_ino, name, &ino);

	if (!nvfuse_lookup(sb, &ictx, NULL, newname, new_par_ino)) {
		inode = ictx->ictx_inode;
		if (inode->i_type == NVFUSE_TYPE_DIRECTORY) {
			nvfuse_release_inode(sb, ictx, DIRTY);
			nvfuse_rmdir(sb, new_par_ino, newname);
		} else {
			nvfuse_release_inode(sb, ictx, DIRTY);
			nvfuse_rmfile(sb, new_par_ino, newname);
		}
	}

	nvfuse_link(sb, new_par_ino, newname, ino);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

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

s32 nvfuse_hardlink(struct nvfuse_superblock *sb, inode_t par_ino, s8 *name, inode_t new_par_ino,
		    s8 *newname)
{
	struct nvfuse_dir_entry direntry;
	inode_t ino = 0;
	s32 res;

	printf(" src name = %s dst name = %s \n", name, newname);

	if (nvfuse_lookup(sb, NULL, &direntry, name, par_ino) < 0) {
		printf(" link: source inode doesn't exist\n");
		return -1;
	}

	ino = direntry.d_ino;

	if (!nvfuse_lookup(sb, NULL, NULL, newname, new_par_ino)) {
		printf(" link: link exists %s \n", newname);
		return -1;
	}

	res = nvfuse_link(sb, new_par_ino, newname, ino);
	if (res) {
		dprintf_error(API, "link() \n");
	}

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

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
		printf(" %s: invalid path\n", __FUNCTION__);
		res = -1;
	} else {
		sb = nvfuse_read_super(nvh);

		if (!nvfuse_lookup(sb, NULL, NULL, filename, dir_entry.d_ino)) {
			dprintf_error(API, "exist file or directory\n");
			return NVFUSE_ERROR;
		}

		res = nvfuse_createfile(sb, dir_entry.d_ino, filename, 0, mode, dev);
		if (res < 0)
			return res;

		nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

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
		printf(" %s: invalid path\n", __FUNCTION__);
		res = -1;
	} else {
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
		printf(" %s: invalid path\n", __FUNCTION__);
		res = -1;
	} else {
		res = nvfuse_truncate(sb, dir_entry.d_ino, filename, size);
	}

	return res;
}

s32 nvfuse_ftruncate(struct nvfuse_handle *nvh, s32 fid, nvfuse_off_t size)
{
	struct nvfuse_superblock *sb;
	struct nvfuse_file_table *ft;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	int res = 0;

	sb = nvfuse_read_super(nvh);

	ft = nvfuse_get_file_table(sb, fid);

	ictx = nvfuse_read_inode(sb, NULL, ft->ino);
	inode = ictx->ictx_inode;

	nvfuse_free_inode_size(sb, ictx, size);

	assert(size < MAX_FILE_SIZE);
	inode->i_size = size;
	nvfuse_release_inode(sb, ictx, DIRTY);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

	return res;
}

s32 nvfuse_symlink(struct nvfuse_handle *nvh, const char *link, inode_t parent, const char *name)
{
	struct nvfuse_superblock *sb;
	int res = 0;
	int ino = 0;
	int fid = 0;
	unsigned int bytes = 0;

	sb = nvfuse_read_super(nvh);

	printf(" symlink : \"%s\", parent #%d, name \"%s\" \n",
	       link, (int)parent, name);

	nvfuse_lock();

	if (!nvfuse_lookup(sb, NULL, NULL, name, parent)) {
		dprintf_error(API, " exist file or directory\n");
		return NVFUSE_ERROR;
	}

	res = nvfuse_createfile(sb, parent, (char *)name, (inode_t *)&ino, 0777 | S_IFLNK, 0);
	if (res != NVFUSE_SUCCESS) {
		dprintf_error(API, "create file error \n");
		return res;
	}

	fid = nvfuse_openfile_ino(sb, ino, O_WRONLY);

	bytes = nvfuse_writefile(nvh, fid, link, strlen(link) + 1, 0);

	if (bytes != strlen(link) + 1) {
		dprintf_error(API, " symlink error \n");
		return -1;
	}

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

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
	unsigned int bytes;
	int fid;
	struct nvfuse_superblock *sb;

	sb = nvfuse_read_super(nvh);

	printf(" readlink : ino  = %d\n", (int) ino);

	if (ino == 0)
		ino = ROOT_INO;

	nvfuse_lock();

	fid = nvfuse_openfile_ino(sb, ino, O_RDONLY);

	bytes = nvfuse_readfile(nvh, fid, buf, size, 0);
	if (bytes != size) {
		dprintf_error(API, "read bytes = %d \n", bytes);
		return -1;
	}

	printf(" read link = %s \n", buf);
	nvfuse_closefile(nvh, fid);

	nvfuse_release_super(sb);

	nvfuse_unlock();

	return bytes;
}

s32 nvfuse_getattr(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char filename[FNAME_SIZE];
	s32 res = 0;

	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {

		res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
		if (res < 0)
			return res;

		if (dir_entry.d_ino == 0) {
			printf(" %s: invalid path\n", __FUNCTION__);
			res = -ENOENT;
			goto RET;
		} else {
			sb = nvfuse_read_super(nvh);
			if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
				res = -ENOENT;
				goto RET;
			}

			inode = ictx->ictx_inode;
			stbuf->st_ino	= inode->i_ino;
			stbuf->st_mode	= inode->i_mode;
			stbuf->st_nlink	= inode->i_links_count;
			stbuf->st_size	= inode->i_size;
			stbuf->st_atime	= inode->i_atime;
			stbuf->st_mtime	= inode->i_mtime;
			stbuf->st_ctime	= inode->i_ctime;
			stbuf->st_gid	= inode->i_gid;
			stbuf->st_uid	= inode->i_uid;

			if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
				stbuf->st_rdev = old_decode_dev(inode->i_blocks[0]);
			} else {
				stbuf->st_rdev = new_decode_dev(inode->i_blocks[1]);
			}

			nvfuse_release_inode(sb, ictx, NVF_CLEAN);
			nvfuse_release_super(sb);

			res = 0;
		}
	}

RET:
	return res;
}

s32 nvfuse_fgetattr(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf, s32 fd)
{
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	struct nvfuse_file_table *ft;
	int res;

	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {
		sb = nvfuse_read_super(nvh);

		ft = sb->sb_file_table + fd;

		ictx = nvfuse_read_inode(sb, NULL, ft->ino);
		inode = ictx->ictx_inode;

		stbuf->st_mode	= inode->i_mode;
		stbuf->st_nlink	= inode->i_links_count;
		stbuf->st_size	= inode->i_size;
		stbuf->st_atime	= inode->i_atime;
		stbuf->st_mtime	= inode->i_mtime;
		stbuf->st_ctime	= inode->i_ctime;

		nvfuse_release_inode(sb, ictx, NVF_CLEAN);
		nvfuse_release_super(sb);

		res = 0;
	}

	return res;
}
#if NVFUSE_OS == NVFUSE_OS_LINUX
s32 nvfuse_access(struct nvfuse_handle *nvh, const char *path, int mask)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
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
			printf(" %s: invalid path\n", __FUNCTION__);
			res = -1;
			goto RET;
		} else {
			sb = nvfuse_read_super(nvh);
			if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
				res = -1;
				goto RET;
			}

			inode = ictx->ictx_inode;

			if (inode->i_type == NVFUSE_TYPE_FILE) {
				if (mask & F_OK)
					res = 0;
				if (mask & R_OK)
					res = 0;
				if (mask & W_OK)
					res = 0;
			} else {
				if (mask & F_OK)
					res = 0;
				if (mask & R_OK)
					res = 0;
				if (mask & W_OK)
					res = -1;
			}


			nvfuse_release_inode(sb, ictx, NVF_CLEAN);
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

		if (nvfuse_lookup(sb, NULL, &cur_dir, filename, parent_dir.d_ino) < 0) {
			res = -1;
			goto RET;
		}

		if (cur_dir.d_ino == 0) {
			printf(" %s: invalid path\n", __FUNCTION__);
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

s32 nvfuse_statvfs(struct nvfuse_handle *nvh, const char *path, struct statvfs *buf)
{
	struct nvfuse_superblock *sb;

	if ((buf == NULL)) return -1;
	buf->f_bsize = CLUSTER_SIZE;    /* file system block size */
	//buf->f_frsize = 0;   /* fragment size */

	sb = nvfuse_read_super(nvh);

	buf->f_blocks = (fsblkcnt_t)sb->sb_no_of_blocks;	/* size of fs in f_frsize units */
	buf->f_bfree = (fsblkcnt_t)sb->sb_free_blocks;		/* # free blocks */
	buf->f_bavail = (fsblkcnt_t)sb->sb_free_blocks;		/* # free blocks for non-root */
	buf->f_files = sb->sb_max_inode_num - sb->sb_free_inodes;    /* # inodes */
	buf->f_ffree = sb->sb_free_inodes;    /* # free inodes */
	buf->f_favail = sb->sb_free_inodes;   /* # free inodes for non-root */
	buf->f_flag = 0;     /* mount flags */

	buf->f_namemax = FNAME_SIZE - 1; /* maximum filename length */

	nvfuse_release_super(sb);

	return 0;
}
#if NVFUSE_OS == NVFUSE_OS_LINUX
s32 nvfuse_chmod_path(struct nvfuse_handle *nvh, const char *path, mode_t mode)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
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
			printf(" %s: invalid path\n", __FUNCTION__);
			res = -1;
			goto RET;
		} else {
			sb = nvfuse_read_super(nvh);
			if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
				res = -1;
				goto RET;
			}

			inode = ictx->ictx_inode;

			mask = S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
			inode->i_mode = (inode->i_mode & ~mask) | (mode & mask);

			nvfuse_release_inode(sb, ictx, DIRTY);
			nvfuse_release_super(sb);
		}
	}

RET:
	return res;
}

s32 nvfuse_chown(struct nvfuse_handle *nvh, const char *path, uid_t uid, gid_t gid)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char filename[FNAME_SIZE];
	int res;

	if (strcmp(path, "/") == 0) {
		res = -1;
	} else {

		res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
		if (res < 0)
			return res;

		if (dir_entry.d_ino == 0) {
			printf(" %s: invalid path\n", __FUNCTION__);
			res = -1;
			goto RET;
		} else {
			sb = nvfuse_read_super(nvh);
			if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
				res = -1;
				goto RET;
			}

			inode = ictx->ictx_inode;
			inode->i_uid = uid;
			inode->i_gid = gid;

			nvfuse_release_inode(sb, ictx, DIRTY);
			nvfuse_release_super(sb);
		}
	}

RET:
	return res;
}

s32 nvfuse_utimens(struct nvfuse_handle *nvh, const char *path, const struct timespec ts[2])
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char filename[FNAME_SIZE];
	int res;

	if (strcmp(path, "/") == 0) {
		res = -1;
	} else {

		res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
		if (res < 0)
			return res;

		if (dir_entry.d_ino == 0) {
			printf(" %s: invalid path\n", __FUNCTION__);
			res = -1;
			goto RET;
		} else {
			sb = nvfuse_read_super(nvh);
			if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
				res = -1;
				goto RET;
			}
			inode = ictx->ictx_inode;
			/* set new access time */
			inode->i_atime = ts[0].tv_sec;
			/* sec new modification time */
			inode->i_mtime = ts[1].tv_sec;

			nvfuse_release_inode(sb, ictx, DIRTY);
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
	struct nvfuse_file_table *ft;
	struct nvfuse_inode_ctx *ictx;

	sb = nvfuse_read_super(nvh);
	ft = sb->sb_file_table + fd;
	ictx = nvfuse_read_inode(sb, NULL, ft->ino);
	/* flush dirty pages associated with inode context including only data pages */
	nvfuse_fdsync_ictx(sb, ictx);
	nvfuse_release_inode(sb, ictx, NVF_CLEAN);
	nvfuse_release_super(sb);

	return 0;
}

s32 nvfuse_fsync(struct nvfuse_handle *nvh, int fd)
{
	struct nvfuse_superblock *sb;
	struct nvfuse_file_table *ft;
	struct nvfuse_inode_ctx *ictx;

	sb = nvfuse_read_super(nvh);
	ft = sb->sb_file_table + fd;
	ictx = nvfuse_read_inode(sb, NULL, ft->ino);

	/* flush dirty pages associated with inode context including meta and data pages */
	nvfuse_fsync_ictx(sb, ictx);

	/* flush cmd to nvme ssd */
	reactor_sync_flush(sb->target);

	nvfuse_release_inode(sb, ictx, NVF_CLEAN);
	nvfuse_release_super(sb);

	return 0;
}

s32 _nvfuse_fsync_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx)
{
	struct list_head *dirty_head, *flushing_head;
	struct list_head *temp, *ptr;
	struct nvfuse_buffer_head *bh;
	struct nvfuse_buffer_cache *bc;
	s32 flushing_count = 0;

	dprintf_error(INODE, " fsync is not supported. \n");
	assert(0);

	/* dirty list for file data */
	dirty_head = &ictx->ictx_data_bh_head;
	flushing_head = &sb->sb_bm->bm_list[BUFFER_TYPE_FLUSHING];

	list_for_each_safe(ptr, temp, dirty_head) {
		bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_dirty_list);
		assert(test_bit(&bh->bh_status, BUFFER_STATUS_DIRTY));

		bc = bh->bh_bc;

		list_move(&bc->bc_list, flushing_head);
		flushing_count++;
		if (flushing_count >= AIO_MAX_QDEPTH)
			break;
	}

	if (flushing_count >= AIO_MAX_QDEPTH)
		goto SYNC_DIRTY;

	/* dirty list for meta data */
	dirty_head = &ictx->ictx_meta_bh_head;
	flushing_head = &sb->sb_bm->bm_list[BUFFER_TYPE_FLUSHING];

	list_for_each_safe(ptr, temp, dirty_head) {
		bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_dirty_list);
		assert(test_bit(&bh->bh_status, BUFFER_STATUS_DIRTY));

		bc = bh->bh_bc;

		list_move(&bc->bc_list, flushing_head);
		flushing_count++;
		if (flushing_count >= AIO_MAX_QDEPTH)
			break;
	}

SYNC_DIRTY:
	;
	nvfuse_sync_dirty_data(sb, flushing_count);

	return 0;
}

s32 nvfuse_fsync_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx)
{
	s32 res;

	/* ictx doesn't keep dirty data */
	while (ictx->ictx_data_dirty_count ||
	       ictx->ictx_meta_dirty_count) {

		res = _nvfuse_fsync_ictx(sb, ictx);
		if (res)
			break;
	}

	return 0;
}

s32 nvfuse_fdsync_ictx(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx)
{
	struct list_head *dirty_head, *flushing_head;
	struct list_head *temp, *ptr;
	struct nvfuse_buffer_head *bh;
	struct nvfuse_buffer_cache *bc;
	s32 flushing_count = 0;

	if (!ictx->ictx_data_dirty_count) {
		return 0;
	}

	dprintf_error(INODE, " fsync is not supported. \n");
	assert(0);

	/* ictx doesn't keep dirty data */
	while (ictx->ictx_data_dirty_count) {
		/* dirty list for file data */
		dirty_head = &ictx->ictx_data_bh_head;
		flushing_head = &sb->sb_bm->bm_list[BUFFER_TYPE_FLUSHING];
		flushing_count = 0;

		list_for_each_safe(ptr, temp, dirty_head) {
			bh = (struct nvfuse_buffer_head *)list_entry(ptr, struct nvfuse_buffer_head, bh_dirty_list);
			assert(test_bit(&bh->bh_status, BUFFER_STATUS_DIRTY));

			bc = bh->bh_bc;

			list_move(&bc->bc_list, flushing_head);
			flushing_count++;
			if (flushing_count >= AIO_MAX_QDEPTH)
				break;
		}

		nvfuse_sync_dirty_data(sb, flushing_count);
	}

	return 0;
}


s32 nvfuse_sync(struct nvfuse_handle *nvh)
{
	struct nvfuse_superblock *sb;
	sb = nvfuse_read_super(nvh);
	nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	nvfuse_release_super(sb);
	return 0;
}

s32 nvfuse_fallocate_verify(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, u32 start,
			    u32 max_block)
{
	u32 curr_block;
	u32 *bitmap;
	u32 collision_cnt = 0;
	s32 res;

	bitmap = malloc(sb->sb_no_of_blocks / 8 + 1);
	assert(bitmap);
	memset(bitmap, 0x00, sb->sb_no_of_blocks / 8 + 1);

	for (curr_block = start / CLUSTER_SIZE; curr_block < max_block; curr_block++) {
		u32 pblock;

		res = nvfuse_get_block(sb, ictx, curr_block, 1, NULL, &pblock, 0 /*create*/);
		if (res < 0) {
			dprintf_error(INODE, "nvfuse_get_block()\n");
			res = -1;
			goto RET;
		}

		if (!pblock) {
			dprintf_error(INODE, "invalid block number = %d\n", pblock);
		}

		if (!test_bit(bitmap, pblock)) {
			set_bit(bitmap, pblock);
		} else {
			dprintf_error(INODE, "collision currblock = %d, pblock = %d\n", curr_block, pblock);
			collision_cnt++;
		}
	}

RET:
	;
	free(bitmap);
	return 0;
}

s32 nvfuse_fallocate(struct nvfuse_handle *nvh, const char *path, s64 start, s64 length)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char filename[FNAME_SIZE];
	int res;
	u32 curr_block;
	u32 max_block;
	u32 remain_block;

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		dprintf_error(DIRECTORY, " %s: invalid path\n", __FUNCTION__);
		res = -1;
		goto RET;
	} else {
		/*printf(" falloc size = %lu \n", length);*/

		sb = nvfuse_read_super(nvh);

		/* test */
		//nvfuse_check_flush_dirty(sb, 1);

		if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
			res = -1;
			goto RET;
		}

		dprintf_info(API, " file name = %s, ino = %d \n", filename, dir_entry.d_ino);

		if (ictx->ictx_inode->i_size < (start + length)) {
			curr_block = start / CLUSTER_SIZE;
			max_block = CEIL(length, CLUSTER_SIZE);
			remain_block = max_block;

			///*
			dprintf_info(INODE, " free no of blocks = %ld\n", (long)sb->sb_free_blocks);
			//*/

			while (remain_block) {
				u32 num_alloc_blks = 0;

				res = nvfuse_get_block(sb, ictx, curr_block, remain_block, &num_alloc_blks, NULL, 1 /*create*/);
				if (res < 0) {
					dprintf_warn(INODE, " nvfuse_get_block()\n");
				}

				curr_block += num_alloc_blks;
				remain_block -= num_alloc_blks;
				if (num_alloc_blks == 0) {
					dprintf_error(INODE, " No more free block in NVFUSE \n");
					break;
				}
				nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
			}
#if 0
			nvfuse_fallocate_verify(sb, ictx, NVFUSE_SIZE_TO_BLK(start), max_block);
#endif

			length = (s64)curr_block * CLUSTER_SIZE;
			inode = ictx->ictx_inode;
			inode->i_size = inode->i_size < length ? length : inode->i_size;
			assert(inode->i_size < MAX_FILE_SIZE);

			nvfuse_release_inode(sb, ictx, DIRTY);
			nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
		} else {
			nvfuse_release_inode(sb, ictx, NVF_CLEAN);
			nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
		}

		nvfuse_release_super(sb);
	}
RET:
	;
	return res;
}

s32 nvfuse_fgetblk(struct nvfuse_superblock *sb, s32 fid, s32 lblk, s32 max_blocks, u32 *num_alloc)
{
	struct nvfuse_file_table *of;
	struct nvfuse_inode_ctx *ictx;
	s32 blk;
	s32 ret;

	of = nvfuse_get_file_table(sb, fid);

	ictx = nvfuse_read_inode(sb, NULL, of->ino);
	if (ictx == NULL) {
		dprintf_error(INODE, "nvfuse_read_inode\n");
		return -1;
	}

	ret = nvfuse_get_block(sb, ictx, lblk, max_blocks, num_alloc, (u32 *)&blk, 0);
	if (ret < 0) {
		dprintf_error(INODE, "nvfuse_get_block\n");
		return -1;
	}

	nvfuse_release_inode(sb, ictx, NVF_CLEAN);
	return blk;
}
