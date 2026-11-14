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
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>
#include <assert.h>
#include <dirent.h>

#include "nvfuse_core.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_config.h"
#include "nvfuse_api.h"
#include "nvfuse_aio.h"
#include "nvfuse_malloc.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_misc.h"
#include "time.h"
#include "nvfuse_debug.h"

#ifdef SPDK_ENABLED
#include "spdk/env.h"
#endif

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
#include <windows.h>
#endif

//extern struct nvfuse_handle *g_nvh;

s32 nvfuse_mkfile(struct nvfuse_handle *nvh, s8 *str, s8 *ssize)
{
#define BLOCK_IO_SIZE (CLUSTER_SIZE * 8)
	u64 size;
	s32 i = CLUSTER_SIZE, fd, write_block_size = BLOCK_IO_SIZE;
	s32 ret = 0;
	s8 file_source[BLOCK_IO_SIZE];

	if (strlen(str) < 1 || strlen(str) >= FNAME_SIZE) {
		dprintf_error(API, " the size of file name is greater than max length.\n");
		return NVFUSE_ERROR;
	}

	if (atoi(ssize) == 0) {
		dprintf_error(API, " invalid zero length.\n");
		return NVFUSE_ERROR;
	}

	fd = nvfuse_openfile_path(nvh, str, O_RDWR | O_CREAT, 0);

	if (fd != -1) {
#if (NVFUSE_OS == NVFUSE_OS_WINDOWS)
		size = _atoi64(ssize);
#else
		size = atoi(ssize);
#endif

		/*ret = nvfuse_fallocate(nvh, str, 0, size);*/

		if (size  < 1) size = CLUSTER_SIZE;

		for (; size >= write_block_size; size -= write_block_size) {
			ret = nvfuse_writefile(nvh, fd, file_source, write_block_size, 0);

			if (ret == -1)
				return -1;
		}
		nvfuse_writefile(nvh, fd, file_source, i, 0); /* write remainder */

		nvfuse_fsync(nvh, fd);
		nvfuse_closefile(nvh, fd);
	}

	if (fd > 0)
		return NVFUSE_SUCCESS;

	return NVFUSE_ERROR;
}

void *nvfuse_aio_test_alloc_req(struct nvfuse_handle *nvh, void *_user_ctx)
{
	struct nvfuse_aio_req *areq = NULL;
	struct user_context *user_ctx;

	user_ctx = (struct user_context *)_user_ctx;

	areq = nvfuse_malloc(sizeof(struct nvfuse_aio_req));
	memset(areq, 0x00, sizeof(struct nvfuse_aio_req));
	areq->fid = user_ctx->fd;
	areq->opcode = user_ctx->is_read ? READ : WRITE;
	areq->buf = user_ctx->user_buf + user_ctx->io_size * user_ctx->buf_ptr;
	user_ctx->buf_ptr = (user_ctx->buf_ptr + 1) % user_ctx->qdepth;
	if (!user_ctx->is_rand) {
		areq->offset = user_ctx->io_curr;
	} else {
		s64 blkno = (u64)nvfuse_rand() % (user_ctx->file_size / user_ctx->io_size);
		areq->offset = blkno * user_ctx->io_size;
	}

	assert(areq->offset + user_ctx->io_size  <= user_ctx->file_size);

	//dprintf_info(AIO, " aio offset = %ld\n", areq->offset);

	areq->bytes = user_ctx->io_size;
	areq->error = 0;
	INIT_LIST_HEAD(&areq->list);
	areq->actx_cb_func = nvfuse_aio_test_callback;
	areq->sb = &nvh->nvh_sb;

	memset(areq->buf, 0xaa, user_ctx->io_size);

	user_ctx->io_curr += user_ctx->io_size;
	user_ctx->io_remaining -= user_ctx->io_size;

	return areq;
}

void nvfuse_aio_test_callback(void *arg)
{
	struct nvfuse_aio_req *areq = (struct nvfuse_aio_req *)arg;
	struct nvfuse_aio_queue *aioq = areq->queue;

#ifdef SPDK_ENABLED
	u64 latency_tsc;
	latency_tsc = areq->complete_tsc - areq->submit_tsc;
	aioq->aio_stat->aio_lat_total_tsc += latency_tsc;
	aioq->aio_stat->aio_lat_total_count++;
	aioq->aio_stat->aio_lat_min_tsc = MIN(latency_tsc, aioq->aio_stat->aio_lat_min_tsc);
	aioq->aio_stat->aio_lat_max_tsc = MAX(latency_tsc, aioq->aio_stat->aio_lat_max_tsc);
	aioq->aio_stat->aio_total_size += areq->bytes;
#endif

	free(areq);
}

#define IS_NOTHING      0
#define IS_OPEN_CLOSE   1
#define IS_READDIR      2
#define IS_UNLINK       3
#define IS_CREATE       4
#define IS_RENAME       5
#define IS_MKDIR        6
#define IS_RMDIR        7
#define IS_OP           8

#define DEBUG_TIME 1
#define DEBUG_FSYNC 0

char *op_list[IS_OP] = {"nothing", "open_close", "readdir", "unlink", "creat", "rename", "mkdir", "rmdir"};

static int print_timeval(struct timeval *tv, struct timeval *tv_end, int op_index)
{

	double tv_s = tv->tv_sec + (tv->tv_usec / 1000000.0);
	double tv_e = tv_end->tv_sec + (tv_end->tv_usec / 1000000.0);
#if DEBUG_TIME
	dprintf_info(STAT, "    %s start : %lf  micro seconds \n", op_list[op_index], tv_s);
	dprintf_info(STAT, "    %s end   : %lf  micro seconds \n", op_list[op_index], tv_e);
#endif
	dprintf_info(STAT, "    %s : %lf              seconds \n", op_list[op_index], tv_e - tv_s);
	return 0;
}

s32 nvfuse_metadata_test(struct nvfuse_handle *nvh, s8 *str, s32 meta_check, s32 count)
{
	struct timeval tv, tv_end;
	struct dirent cur_dirent;

	s32 flags_create, flags_rdwr, state, fid, i;

	char *path_dir = "test_direcpty";
	char *path_file = "test_file.txt";
	off_t offset = 0;

	char buf[20] = {0,};
	char rename_buf[22] = {0,};

	flags_create = O_WRONLY | O_CREAT | O_TRUNC;
	flags_rdwr = O_RDWR;

#ifdef DEBUG
	int test_val = 0;
	dprintf_info(TEST, "nvfuse_metadata_test %d \n", test_val++);
#endif

	if (meta_check != IS_NOTHING) {

		switch (meta_check) {
		/* measure the open/close operation to exisisting file */
		case IS_OPEN_CLOSE :
			dprintf_info(TEST, "metadata operation - open_close operation ...\n");

			fid = nvfuse_openfile_path(nvh, path_file, flags_create, 0644);
			if (fid == NVFUSE_ERROR) {
				dprintf_error(TEST, "\tError in meta check(IS_OPEN_CLOSE) : %s file create error (before open/close) \n",
				       path_file);
				return NVFUSE_ERROR;
			}
			state = nvfuse_writefile(nvh, fid, str, sizeof(str), 0);
			if (state == NVFUSE_ERROR) {
				dprintf_error(TEST, "\tError in meta check(IS_OPEN_CLOSE) : file write error (before open/close) \n");
				return NVFUSE_ERROR;
			}
#if DEBUG_FSYNC
			nvfuse_fsync(nvh, fid);
#endif
			nvfuse_closefile(nvh, fid);

			gettimeofday(&tv, NULL);
			/* measure point  */
			for (i = 0; i < count ; i++) {
				fid = nvfuse_openfile_path(nvh, path_file, flags_rdwr, 0644);
				if (fid < 0) {
					dprintf_error(TEST, "\tError in meta check(IS_OPEN_CLOSE) : file open error (measuring open/close) \n");
					return NVFUSE_ERROR;
				}
				nvfuse_closefile(nvh, fid);
			}
			/* measure point  */
			gettimeofday(&tv_end, NULL);

			state = nvfuse_unlink(nvh, path_file);
			if (state == NVFUSE_ERROR) {
				dprintf_error(TEST, "\tError in meta check(IS_OPEN_CLOSE) : %s  file delelte error (after open/close) \n",
				       path_file);
				return NVFUSE_ERROR;
			}

			print_timeval(&tv, &tv_end, IS_OPEN_CLOSE);
			break;

		/* measure the readdir operation to existing directory */
		case IS_READDIR :
			dprintf_info(TEST, "metadata operation - readdir operation ...\n");
			state = nvfuse_mkdir_path(nvh, path_dir, 0755);
			if (state == NVFUSE_ERROR) {
				dprintf_error(TEST, "\tError in meta check(IS_READDIR) : %s  directory create error (before readdir) \n",
				       path_dir);
				return NVFUSE_ERROR;
			}

			for (i = 0; i < 3 ; i++) {
				sprintf(buf, "%s/%d.txt", path_dir, i);
				fid = nvfuse_openfile_path(nvh, buf, flags_create, 0644);
				if (fid == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_READDIR) : %s file create error (before readdir) \n", buf);
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
#if DEBUG_FSYNC
				nvfuse_fsync(fd);
#endif
				close(fid);
			}
			s32 par_ino = nvfuse_opendir(nvh, path_dir);

			gettimeofday(&tv, NULL);
			/* measure point  */
			for (i = 0; i < count ; i++) {
				while (nvfuse_readdir(nvh, par_ino, &cur_dirent, offset)) {
					offset = offset + 1;
				}
			}
			/* measure point  */
			gettimeofday(&tv_end, NULL);


			for (i = 0; i < 3 ; i++) {
				sprintf(buf, "%s/%d.txt", path_dir, i);
				state = nvfuse_unlink(nvh, buf);
				if (state == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_READDIR) : %s unlink error (after readdir) \n", buf);
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
			}

			state = nvfuse_rmdir_path(nvh, path_dir);
			if (state == NVFUSE_ERROR) {
				dprintf_error(TEST, "Error in meta check(IS_READDIR) : %s  directory delete error (after readdir) \n", path_dir);
				return NVFUSE_ERROR;
			}
			print_timeval(&tv, &tv_end, IS_READDIR);
			break;

		/* measure the unlink operation to empty file */
		case IS_UNLINK :
			dprintf_info(TEST, "metadata operation - unlink operation ...\n");

			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d.txt", i);
				fid = nvfuse_openfile_path(nvh, buf, flags_create, 0644);
				if (fid == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_UNLINK) : %s  file create error (before unlink) \n", buf);
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
#if DEBUG_FSYNC
				nvfuse_fsync(nvh, fid);
#endif
				nvfuse_closefile(nvh, fid);
			}

			gettimeofday(&tv, NULL);
			/* measure point  */
			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d.txt", i);
				state = nvfuse_unlink(nvh, buf);
				if (state == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_UNLINK) : %s  file unlink error (measuring unlink) \n", buf);
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
			}
			/* measure point  */
			gettimeofday(&tv_end, NULL);

			print_timeval(&tv, &tv_end, IS_UNLINK);
			break;

		/* measure the create operation to empty file */
		case IS_CREATE :
			dprintf_info(TEST, "metadata operation - create operation ...\n");

			gettimeofday(&tv, NULL);
			/* measure point  */

			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d.txt", i);
				fid = nvfuse_openfile_path(nvh, buf, flags_create, 0644);
				if (fid == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_CREATE) : file create error (measuring create) \n");
					return NVFUSE_ERROR;
				}

				memset(buf, 0x0, sizeof(buf));
#if DEBUG_FSYNC
				nvfuse_fsync(nvh, fd);
#endif
				nvfuse_closefile(nvh, fid);
			}

			/* measure point  */
			gettimeofday(&tv_end, NULL);

			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d.txt", i);
				state = nvfuse_unlink(nvh, buf);
				if (state == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_CREATE) : file delelte error (after create) \n");
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
			}
			print_timeval(&tv, &tv_end, IS_CREATE);
			break;

		/* measure the rename operation to existing file */
		case IS_RENAME :
			dprintf_info(TEST, "metadata operation - rename operation ...\n");
			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d.txt", i);
				fid = nvfuse_openfile_path(nvh, buf, flags_create, 0664);
				if (fid == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_RENAME) : file create error (before rename) \n");
					return NVFUSE_ERROR;
				}
#if DEBUG_FSYNC
				nvfuse_fsync(nvh, fid);
#endif
				nvfuse_closefile(nvh, fid);
				memset(buf, 0x0, sizeof(buf));
			}

			gettimeofday(&tv, NULL);
			/* measure point  */

			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d.txt", i);
				sprintf(rename_buf, "%d_rename.txt", i);
				state = nvfuse_rename_path(nvh, buf, rename_buf);
				if (state == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_RENAME) : rename error (measuring rename) \n");
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
				memset(rename_buf, 0x0, sizeof(rename_buf));
			}

			/* measure point  */
			gettimeofday(&tv_end, NULL);

			for (i = 0; i < count ; i++) {
				sprintf(rename_buf, "%d_rename.txt", i);
				state = nvfuse_rmfile_path(nvh, rename_buf);
				if (state == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_RENAME) : file delelte error (after rename) \n");
					return NVFUSE_ERROR;
				}
				memset(rename_buf, 0x0, sizeof(rename_buf));
			}

			print_timeval(&tv, &tv_end, IS_RENAME);
			break;

		/* measure the mkdir operation */
		case IS_MKDIR :
			dprintf_info(TEST, "metadata operation - mkdir operation ...\n");

			gettimeofday(&tv, NULL);
			/* measure point  */
			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d", i);
				state = nvfuse_mkdir_path(nvh, buf, 0755);
				if (state == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_MKDIR) : directory create error (measuring mkdir) \n");
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
			}

			/* measure point  */
			gettimeofday(&tv_end, NULL);

			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d", i);
				state = nvfuse_rmdir_path(nvh, buf);
				if (state == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_MKDIR) : directory delete error (after mkdir) \n");
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
			}

			print_timeval(&tv, &tv_end, IS_MKDIR);
			break;

		case IS_RMDIR :
			dprintf_info(TEST, "metadata operation - rmdir operation ...\n");

			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d", i);
				state = nvfuse_mkdir_path(nvh, buf, 0755);
				if (state == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_RMDIR) : directory create error (before rmdir) \n");
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
			}

			gettimeofday(&tv, NULL);
			/* measure point  */

			for (i = 0; i < count ; i++) {
				sprintf(buf, "%d", i);
				state = nvfuse_rmdir_path(nvh, buf);
				if (state == NVFUSE_ERROR) {
					dprintf_error(TEST, "Error in meta check(IS_RMDIR) : directory delete error (measuring rmdir) \n");
					return NVFUSE_ERROR;
				}
				memset(buf, 0x0, sizeof(buf));
			}

			/* measure point  */
			gettimeofday(&tv_end, NULL);

			print_timeval(&tv, &tv_end, IS_RMDIR);
			break;
		default:
			dprintf_info(TEST, "Invalid metadata type setting  Error\n");
			break;

		}
	} else {
		dprintf_error(TEST, "\t metadata option is not selected \n");
	}

	nvfuse_sync(nvh);
	return NVFUSE_SUCCESS;
}

#if 0
s32 nvfuse_aio_test(struct nvfuse_handle *nvh, s32 direct)
{
	char str[128];
	struct timeval tv;
	s64 file_size;
	s32 i;
	s32 res;

	for (i = 0; i < 1; i++) {
		sprintf(str, "file%d", i);
		file_size = (s64)8 * 1024 * 1024 * 1024;
		gettimeofday(&tv, NULL);
		res = nvfuse_aio_test_rw(nvh, str, file_size, 4096, AIO_MAX_QDEPTH, WRITE, direct,
					 0 /* sequential */, 0);
		if (res < 0) {
			dprintf_error(AIO, " Error: aio write test \n");
			break;
		}

		dprintf_info(AIO, " nvfuse aio through %.3fMB/s\n", (double)file_size / (1024 * 1024) / time_since_now(&tv));
		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0) {
			dprintf_error(AIO, " Error: rmfile = %s\n", str);
			break;
		}
	}

	for (i = 0; i < 1; i++) {
		sprintf(str, "file%d", i);
		file_size = (s64)8 * 1024 * 1024 * 1024;
		gettimeofday(&tv, NULL);
		res = nvfuse_aio_test_rw(nvh, str, file_size, 4096, AIO_MAX_QDEPTH, READ, direct,
					 0 /* sequential */, 0);
		if (res < 0) {
			dprintf_error(AIO, " Error: aio write test \n");
			break;
		}
		dprintf_info(AIO, " nvfuse aio through %.3fMB/s\n", (double)file_size / (1024 * 1024) / time_since_now(&tv));
		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0) {
			dprintf_error(AIO, " Error: rmfile = %s\n", str);
			break;
		}
	}

	exit(0);

	return NVFUSE_SUCCESS;
}
#endif

s32 nvfuse_fallocate_test(struct nvfuse_handle *nvh)
{
	char str[128];
	struct timeval tv;
	s64 file_size;
	s32 i;
	s32 res;
	s32 fid;

	for (i = 0; i < 3; i++) {
		sprintf(str, "file%d", i);
		switch (i) {
		case 0:
			file_size = (s64)16 * 1024;
			break;
		case 1:
			file_size = (s64)16 * 1024 * 1024;
			break;
		case 2:
			file_size = (s64)16 * 1024 * 1024 * 1024;
			break;
		default:
			file_size = (s64)16 * 1024;
		}


		fid = nvfuse_openfile_path(nvh, str, O_RDWR | O_CREAT, 0);
		if (fid < 0) {
			dprintf_error(EXAMPLE, " Error: file open or create \n");
			return -1;
		}
		nvfuse_closefile(nvh, fid);

		gettimeofday(&tv, NULL);
		dprintf_info(EXAMPLE, "\n TEST (Fallocate and Deallocate) %d.\n", i);
		dprintf_info(EXAMPLE, " start fallocate %s size %lu \n", str, (long)file_size);
		/* pre-allocation of data blocks*/
		nvfuse_fallocate(nvh, str, 0, file_size);
		dprintf_info(EXAMPLE, " finish fallocate %s size %lu \n", str, (long)file_size);
		dprintf_info(EXAMPLE, " nvfuse fallocate throughput %.3f MB/s\n",
		       (double)file_size / (1024 * 1024) / nvfuse_time_since_now(&tv));

		gettimeofday(&tv, NULL);
		dprintf_info(EXAMPLE, " start rmfile %s size %lu \n", str, (long)file_size);
		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0) {
			dprintf_error(EXAMPLE, " Error: rmfile = %s\n", str);
			break;
		}
		dprintf_info(EXAMPLE, " nvfuse rmfile throughput %.3f MB/s\n",
		       (double)file_size / (1024 * 1024) / nvfuse_time_since_now(&tv));
	}

	return NVFUSE_SUCCESS;
}

s32 nvfuse_cd(struct nvfuse_handle *nvh, s8 *str)
{
	struct nvfuse_dir_entry dir_temp;
	struct nvfuse_inode_ctx *d_ictx;
	struct nvfuse_inode *d_inode;
	struct nvfuse_superblock *sb;

	sb = nvfuse_read_super(nvh);

	if (str[0] == '/' && strlen(str) == 1) {
		nvfuse_set_cwd_ino(nvh, nvfuse_get_root_ino(nvh));
		return NVFUSE_SUCCESS;
	}

	if (nvfuse_lookup(sb, &d_ictx, &dir_temp, str, nvfuse_get_cwd_ino(nvh)) < 0) {
		dprintf_error(API, " invalid dir path\n");
		return NVFUSE_ERROR;
	}

	d_inode = d_ictx->ictx_inode;

	if (d_inode->i_type == NVFUSE_TYPE_DIRECTORY) {
		nvfuse_set_cwd_ino(nvh, dir_temp.d_ino);
	} else {
		dprintf_error(API, " invalid dir path\n");
		return NVFUSE_ERROR;
	}
	nvfuse_release_inode(sb, d_ictx, NVF_CLEAN);
	nvfuse_release_super(sb);
	return NVFUSE_SUCCESS;
}

void nvfuse_test(struct nvfuse_handle *nvh)
{
	s32 i, k;
	s8 str[128];
	s32 nr = 100000;
	s32 iter = 2;

	memset(str, 0, 128);

	for (k = 0; k < iter; k++) {
		/* create files*/
		for (i = 0; i < nr; i++) {
			sprintf(str, "file%d", i);
			nvfuse_mkfile(nvh, str, "4096");
		}

		/* lookup files */
		for (i = 0; i < nr; i++) {
			struct stat st_buf;
			int res;

			sprintf(str, "file%d", i);
			res = nvfuse_getattr(nvh, str, &st_buf);
			if (res)
				dprintf_error(TEST, " No such file %s\n", str);
		}

		/* delete files */
		for (i = 0; i < nr; i++) {
			sprintf(str, "file%d", i);
			nvfuse_rmfile_path(nvh, str);
		}
	}

	for (k = 0; k < iter; k++) {
		/* create directories */
		for (i = 0; i < nr; i++) {
			sprintf(str, "dir%d", i);
			nvfuse_mkdir_path(nvh, str, 0644);
		}

		/* lookup files */
		for (i = 0; i < nr; i++) {
			struct stat st_buf;
			int res;

			sprintf(str, "dir%d", i);
			res = nvfuse_getattr(nvh, str, &st_buf);
			if (res)
				dprintf_error(TEST, " No such dir %s\n", str);
		}

		/* delete directories */
		for (i = 0; i < nr; i++) {
			sprintf(str, "dir%d", i);
			nvfuse_rmdir_path(nvh, str);
		}
	}
}

s32 nvfuse_type(struct nvfuse_handle *nvh, s8 *str)
{
	u32 i, size, offset = 0;
	s32 fid, read_block_size = CLUSTER_SIZE;
	s8 b[CLUSTER_SIZE + 1];
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);

	fid = nvfuse_openfile_path(nvh, str, O_RDWR | O_CREAT, 0);

	if (fid != -1) {
		size = sb->sb_file_table[fid].size;

		for (i = size; i >= read_block_size;
		     i -= read_block_size, offset += read_block_size) {
			nvfuse_readfile(nvh, fid, b, read_block_size, 0);
			b[read_block_size]	= '\0';
			printf("%s", b);
		}

		nvfuse_readfile(nvh, fid, b, i, 0); /* write remainder */

		b[i] = '\0';

		printf("%s", b);
	}

	printf("\n");

	nvfuse_closefile(nvh, fid);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_rdfile(struct nvfuse_handle *nvh, s8 *str)
{
	struct nvfuse_superblock *sb;
	u32 i, size, offset = 0;
	s32 fid, read_block_size = CLUSTER_SIZE * 32;
	s8 b[CLUSTER_SIZE * 32];

	sb = nvfuse_read_super(nvh);

	fid = nvfuse_openfile_path(nvh, str, O_RDWR, 0);

	if (fid != -1) {
		size = sb->sb_file_table[fid].size;


		for (i = size; i >= read_block_size;
		     i -= read_block_size, offset += read_block_size) {
			nvfuse_readfile(nvh, fid, b, read_block_size, 0);
		}

		nvfuse_readfile(nvh, fid, b, i, 0); /* write remainder */
	}

	nvfuse_closefile(nvh, fid);

	return NVFUSE_SUCCESS;
}

void nvfuse_srand(long seed)
{
#if (NVFUSE_OS==NVFUSE_OS_LINUX)
	srand48(seed);
#else
	srand(seed);
#endif
}

s64 nvfuse_rand(void)
{
	s64 val;
#if (NVFUSE_OS==NVFUSE_OS_LINUX)
	val = mrand48();
#else
	val = (s64)(rand()) << 32;
	val += rand();
#endif
	return val;
}

void nvfuse_rusage_diff(struct rusage *x, struct rusage *y, struct rusage *result)
{
	timeval_subtract(&result->ru_stime,
			 &y->ru_stime,
			 &x->ru_stime);

	timeval_subtract(&result->ru_utime,
			 &y->ru_utime,
			 &x->ru_utime);

	result->ru_maxrss = y->ru_maxrss - x->ru_maxrss;        /* maximum resident set size */
	result->ru_ixrss = y->ru_ixrss - x->ru_ixrss;         /* integral shared memory size */
	result->ru_idrss = y->ru_idrss - x->ru_idrss;         /* integral unshared data size */
	result->ru_isrss = y->ru_isrss - x->ru_isrss;         /* integral unshared stack size */
	result->ru_minflt = y->ru_minflt - x->ru_minflt;        /* page reclaims (soft page faults) */
	result->ru_majflt = y->ru_majflt - x->ru_majflt;        /* page faults (hard page faults) */
	result->ru_nswap = y->ru_nswap - x->ru_nswap;         /* swaps */
	result->ru_inblock = y->ru_inblock - x->ru_inblock;       /* block input operations */
	result->ru_oublock = y->ru_oublock - x->ru_oublock;       /* block output operations */
	result->ru_msgsnd = y->ru_msgsnd - x->ru_msgsnd;        /* IPC messages sent */
	result->ru_msgrcv = y->ru_msgrcv - x->ru_msgrcv;        /* IPC messages received */
	result->ru_nsignals = y->ru_nsignals - x->ru_nsignals;      /* signals received */
	result->ru_nvcsw = y->ru_nvcsw - x->ru_nvcsw;         /* voluntary context switches */
	result->ru_nivcsw = y->ru_nivcsw - x->ru_nivcsw;        /* involuntary context switches */
}

void nvfuse_rusage_add(struct rusage *x, struct rusage *result)
{
	timeval_add(&result->ru_stime,
		    &x->ru_stime);

	timeval_add(&result->ru_utime,
		    &x->ru_utime);

	result->ru_maxrss += x->ru_maxrss;        /* maximum resident set size */
	result->ru_ixrss += x->ru_ixrss;         /* integral shared memory size */
	result->ru_idrss += x->ru_idrss;         /* integral unshared data size */
	result->ru_isrss += x->ru_isrss;         /* integral unshared stack size */
	result->ru_minflt += x->ru_minflt;        /* page reclaims (soft page faults) */
	result->ru_majflt += x->ru_majflt;        /* page faults (hard page faults) */
	result->ru_nswap += x->ru_nswap;         /* swaps */
	result->ru_inblock += x->ru_inblock;       /* block input operations */
	result->ru_oublock += x->ru_oublock;       /* block output operations */
	result->ru_msgsnd += x->ru_msgsnd;        /* IPC messages sent */
	result->ru_msgrcv += x->ru_msgrcv;        /* IPC messages received */
	result->ru_nsignals += x->ru_nsignals;      /* signals received */
	result->ru_nvcsw += x->ru_nvcsw;         /* voluntary context switches */
	result->ru_nivcsw += x->ru_nivcsw;        /* involuntary context switches */
}

void print_rusage(struct rusage *rusage, char *prefix, int divisor, double total_exec)
{
//	printf(" usr %f sec \n", (double)tv_to_sec(&rusage->ru_utime));
//	printf(" sys %f sec \n", (double)tv_to_sec(&rusage->ru_stime));

	dprintf_info(STAT, " %s usr cpu utilization = %3.0f %% (%f sec)\n", prefix,
	       (double)tv_to_sec(&rusage->ru_utime) / (double)divisor / total_exec * 100,
	       total_exec);
	dprintf_info(STAT, " %s sys cpu utilization = %3.0f %% (%f sec)\n", prefix,
	       (double)tv_to_sec(&rusage->ru_stime) / (double)divisor / total_exec * 100,
	       total_exec);
#if 0
	dprintf_info(STAT, " %s max resider set size = %ld \n", prefix, rusage->ru_maxrss / divisor);
	dprintf_info(STAT, " %s integral share memory size = %ld \n", prefix, rusage->ru_ixrss / divisor);
	dprintf_info(STAT, " %s integral unshared data size = %ld \n", prefix, rusage->ru_idrss / divisor);
	dprintf_info(STAT, " %s integral unshared stack size = %ld \n", prefix, rusage->ru_isrss / divisor);
#endif
	dprintf_info(STAT, " %s page reclaims (soft page faults) = %ld \n", prefix, rusage->ru_minflt / divisor);
	dprintf_info(STAT, " %s page reclaims (hard page faults) = %ld \n", prefix, rusage->ru_majflt / divisor);
#if 0
	dprintf_info(STAT, " %s swaps = %ld \n", prefix, rusage->ru_nswap / divisor);
	dprintf_info(STAT, " %s block input operations = %ld \n", prefix, rusage->ru_inblock / divisor);
	dprintf_info(STAT, " %s block output operations = %ld \n", prefix, rusage->ru_oublock / divisor);
	dprintf_info(STAT, " %s IPC messages sent = %ld \n", prefix, rusage->ru_msgsnd / divisor);
	dprintf_info(STAT, " %s IPC messages received = %ld \n", prefix, rusage->ru_msgrcv / divisor);
	dprintf_info(STAT, " %s signals received = %ld \n", prefix, rusage->ru_nsignals / divisor);
#endif
	dprintf_info(STAT, " %s voluntary context switches = %ld \n", prefix, rusage->ru_nvcsw / divisor);
	dprintf_info(STAT, " %s involuntary context switches = %ld \n", prefix, rusage->ru_nivcsw / divisor);
}
