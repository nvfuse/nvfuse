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
#include <aio.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <dirent.h>
#include <pthread.h>

#define IS_NOTHING      0
#define IS_OPEN_CLOSE   1
#define IS_READDIR      2
#define IS_UNLINK       3
#define IS_CREATE       4
#define IS_RENAME       5
#define IS_MKDIR        6
#define IS_RMDIR        7
#define IS_TOTAL         8
#define IS_DSTAT        9
#define IS_OP           10

char *op_list[IS_OP] = {"nothing", "open_close", "readdir", "unlink", "creat", "rename", "mkdir", "rmdir", "stat", "dstat"};

#define KB (1024)
#define MB (1024*1024)
#define GB (1024*1024*1024)
#define TB ((s64)1024*1024*1024*1024)

/* CAPACITY CALCULATION */
#define TERA_BYTES ((u64)1024*1024*1024*1024)
#define GIGA_BYTES (1024*1024*1024)
#define MEGA_BYTES (1024*1024)
#define KILO_BYTES (1024)

#define __int64 long long
typedef signed long long s64;
typedef signed int s32;
typedef signed char s8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define SUCCESS 0
#define ERROR -1

#define DEBUG 0
#define DEBUG_TIME 0
#define DEBUG_FSYNC 0

#define FNAME_SIZE 128
#define DIR_SIZE 128

#define DEFAULT_NUM 1
#define DEFAULT_COUNT DEFAULT_NUM
#define DEFAULT_THREAD DEFAULT_NUM

struct thread_args {
	int meta_check;
	int count;
	int cur_thread;
};

void perf_kernel_usage(char *cmd)
{
	printf("\nOptions for %s: \n", cmd);
	printf("   -M: metadata intensive operation to measure (e.g., open_close , readdir , unlink , create , rename , mkdir , rmdir)\n");
	printf("   -C: repetition to measure metadata intensice operation (default is 1)\n\n");
	printf("   -T: number of threads (default is 1)\n\n");
}

long double get_time(struct timeval *tv, struct timeval *tv_end, int op_index)
{

	double tv_s = tv->tv_sec + (tv->tv_usec / 1000000.0);
	double tv_e = tv_end->tv_sec + (tv_end->tv_usec / 1000000.0);
#if 0
	printf("    %s start : %lf  micro seconds \n", op_list[op_index], tv_s);
	printf("    %s end   : %lf  micro seconds \n", op_list[op_index], tv_e);
	printf("    %s : %lf              seconds \n",);

#endif

	return (tv_e - tv_s);
}

double *execution_time = NULL;
double **execution_time_each = NULL;

void *do_metadata_test(void *data)
{

	struct thread_args th_args = *(struct thread_args *)data;
	struct timeval tv, tv_end;
	struct dirent *dir_entry;
	int flags_create, flags_rdwr, fd, i;
	int meta_check = th_args.meta_check;
	int count = th_args.count;
	int cur_thread = th_args.cur_thread;
	int res,  state;
	DIR *dir_info;
	char buf[20] = {0,};
	char rename_buf[20] = {0,};
	char str_file[FNAME_SIZE] = {0,};
	char str_file_name[FNAME_SIZE] = {0,};
	char str_dir_name[FNAME_SIZE] = {0,};
	long double time = 0.0;
	struct stat st;

	/*FIXME temp for total */
	struct timeval tv_total_create_end, tv_total_delete_end, tv_total_stat_end;
//    int *fd_arr = (int*)malloc(sizeof(int)*count);
//    memset(fd_arr, 0x0, sizeof(int)*count);

	sprintf(str_file, "file_allocate_test");

	flags_create = O_WRONLY | O_CREAT | O_TRUNC;
	flags_rdwr = O_RDWR;

#if DEBUG
	int test_val = 0;
#endif

#if DEBUG
	printf("_perf_metadata %d \n", test_val++);
#endif

	if (meta_check != IS_NOTHING) {

		switch (meta_check) {
		case IS_OPEN_CLOSE :

			sprintf(str_file_name, "./%d/tmp_file.txt", cur_thread);
			fd = open(str_file_name, flags_create, 0644);
			if (fd < 0) {
				perror("Error in meta check(IS_OPEN) : file open error (before open/close) \n");
				goto ERRORS;
			}
			write(fd, str_file, strlen(str_file));
#if DEBUG_FSYNC
			fsync(fd);
#endif
			close(fd);
#if DEBUG
			printf("_perf_metadata %d \n", test_val++);
#endif
			gettimeofday(&tv, NULL);
			/* measure point  */
			for (i = 0; i < count ; i++) {
				fd = open(str_file_name, flags_rdwr, 0644);
				if (fd < 0) {
					perror("Error in meta check(IS_OPEN) : existing file open error (measuring open/close) \n");
					goto ERRORS;
				}
				close(fd);
			}
			/* measure point  */
			gettimeofday(&tv_end, NULL);

			state = unlink(str_file_name);
			if (state != SUCCESS) {
				perror("Error in meta check(IS_OPEN) : file delelte error (after open/close) \n");
				goto ERRORS;
			}

			time = get_time(&tv, &tv_end, IS_OPEN_CLOSE);
			break;

		case IS_READDIR :
			sprintf(str_dir_name, "./%d/tmp_dir", cur_thread);
			state = mkdir(str_dir_name, 0755);
			if (state == ERROR) {
				perror("Error in meta check(IS_READDIR) : mkdir error (before readdir) \n");
				goto ERRORS;
			}

			for (i = 0; i < 3 ; i++) {
				sprintf(buf, "./%d/%s/%d.txt", cur_thread, str_dir_name, i);
				fd = open(buf, flags_create, 0644);
				if (fd < 0) {
					printf("Error in meta check(IS_READDIR) : %s file  create error (before readdir) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
#if DEBUG_FSYNC
				fsync(fd);
#endif
				close(fd);
			}
			dir_info = opendir(str_dir_name);
			i = 0;

			gettimeofday(&tv, NULL);
			/* measure point  */
			for (i = 0; i < count ; i++) {
				while (dir_entry = readdir(dir_info)) {
					if (!dir_entry) {
						printf("meta check(IS_READDIR) : readdir error (measuring readdir)\n");
						goto ERRORS;
					}
				}
			}
			/* measure point  */
			gettimeofday(&tv_end, NULL);

			for (i = 0; i < 3 ; i++) {
				sprintf(buf, "./%d/%s/%d.txt", cur_thread, str_dir_name, i);
				state = unlink(buf);
				if (state != SUCCESS) {
					printf("Error in meta check(IS_READDIR) : %s unlink error (after readdir) \n", buf);
					goto ERRORS;
				}

				memset(buf, 0x0, sizeof(buf));
			}

			state = rmdir(str_dir_name);
			if (state == ERROR) {
				perror("Error in meta check(IS_READDIR) : %s  rmdir error (after readdir) \n");
				goto ERRORS;
			}
			time = get_time(&tv, &tv_end, IS_READDIR);
			break;

		/* measure the unlink operation to empty file */
		case IS_UNLINK :

			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d.txt", cur_thread, i);
				fd = open(buf, flags_create, 0644);
				if (fd < 0) {
					printf("Error in meta check(IS_UNLINK) : %s create error (before unlink) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
#if DEBUG_FSYNC
				fsync(fd);
#endif
				close(fd);
			}

			gettimeofday(&tv, NULL);
			/* measure point  */
			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d.txt", cur_thread, i);
				state = unlink(buf);
				if (state == ERROR) {
					printf("Error in meta check(IS_UNLINK) : %s file unlink error (measuring unlink) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
			}
			/* measure point  */
			gettimeofday(&tv_end, NULL);
			time = get_time(&tv, &tv_end, IS_UNLINK);
			break;

		/* measure the create operation to empty file */
		case IS_CREATE :

			gettimeofday(&tv, NULL);
			/* measure point  */
			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d.txt", cur_thread, i);
				fd = open(buf, flags_create, 0644);
				if (fd < 0) {
					printf("Error in meta check(IS_CREATE) : %s creat error (measuring create) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
#if DEBUG_FSYNC
				fsync(fd);
#endif
				close(fd);
			}
			/* measure point  */

			gettimeofday(&tv_end, NULL);

			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d.txt", cur_thread, i);
				state = unlink(buf);
				if (state == ERROR) {
					printf("Error in meta check(IS_CREATE) : %s unlink error (after create) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
			}
			time = get_time(&tv, &tv_end, IS_CREATE);

			break;

		/* measure the rename operation to existing file */
		case IS_RENAME :
			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d.txt", cur_thread, i);
				fd = open(buf, flags_create, 0664);
				if (fd < 0) {
					printf("Error in meta check(IS_RENAME) : %s  file create error (before rename) \n", buf);
					goto ERRORS;
				}
#if DEBUG_FSYNC
				fsync(fd);
#endif
				close(fd);
				memset(buf, 0x0, sizeof(buf));
			}

			gettimeofday(&tv, NULL);

			/* measure point  */
			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d.txt", cur_thread, i);
				sprintf(rename_buf, "./%d/%d_rename.txt", cur_thread, i);
				state = rename(buf, rename_buf);
				if (state == ERROR) {
					printf("Error in meta check(IS_RENAME) : rename %s to %s  error (measuring rename) \n", buf,
					       rename_buf);
				}
				memset(buf, 0x0, sizeof(buf));
				memset(rename_buf, 0x0, sizeof(rename_buf));
			}
			/* measure point  */

			gettimeofday(&tv_end, NULL);
			for (i = 0; i < count ; i++) {
				sprintf(rename_buf, "./%d/%d_rename.txt", cur_thread, i);
				state = unlink(rename_buf);
				if (state == ERROR) {
					printf("Error in meta check(IS_RENAME) : %s  file delelte error (after rename) \n", rename_buf);
					goto ERRORS;
				}
				memset(rename_buf, 0x0, sizeof(rename_buf));
			}

			time = get_time(&tv, &tv_end, IS_RENAME);
			break;

		case IS_MKDIR :

			gettimeofday(&tv, NULL);

			/* measure point  */
			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d", cur_thread, i);
				state = mkdir(buf, 0755);
				if (state == ERROR) {
					printf("Error in meta check(IS_MKDIR) : %s  directory create error (measuring mkdir) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
			}
			/* measure point  */

			gettimeofday(&tv_end, NULL);

			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d", cur_thread, i);
				state = rmdir(buf);
				if (state == ERROR) {
					printf("Error in meta check(IS_MKDIR) : %s  directory delete error (after mkdir) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
			}

			time = get_time(&tv, &tv_end, IS_MKDIR);
			break;
		case IS_RMDIR :

			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d", cur_thread, i);
				state = mkdir(buf, 0755);
				if (state == ERROR) {
					printf("Error in meta check(IS_RMDIR) : %s  directory create error (before rmdir) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
			}
			gettimeofday(&tv, NULL);

			/* measure point  */
			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d", cur_thread, i);
				state = rmdir(buf);
				if (state == ERROR) {
					printf("Error in meta check(IS_RMDIR) : %s  directory delete error (measuring rmdir) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
			}

			/* measure point */
			gettimeofday(&tv_end, NULL);

			time = get_time(&tv, &tv_end, IS_RMDIR);
			break;

		case IS_TOTAL :

			/*create */
			gettimeofday(&tv, NULL);

			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d.txt", cur_thread, i);
				fd = open(buf, flags_create, 0644);
				if (fd < 0) {
					printf("Error in meta check(IS_STAT) : stat error (before stat) \n");
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
#if DEBUG_FSYNC
				fsync(fd);
#endif
				close(fd);

			}
			sync();

			gettimeofday(&tv_total_create_end, NULL);
			execution_time_each[cur_thread][0] = get_time(&tv, &tv_total_create_end, IS_TOTAL);

			for (i = 0; i < count ; i++) {
				memset(&st, 0x0, sizeof(struct stat));
				memset(buf, 0x0, sizeof(buf));
				sprintf(buf, "./%d/%d.txt", cur_thread, i);
				state = stat(buf, &st);
				if (state == ERROR) {
					printf("Error in meta check(IS_STAT) : %s  stat error (measuring stat) \n", buf);
					goto ERRORS;
				}
			}

			gettimeofday(&tv_total_stat_end, NULL);
			execution_time_each[cur_thread][1] = get_time(&tv_total_create_end, &tv_total_stat_end, IS_TOTAL);

			/* unlnk  */
			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d.txt", cur_thread, i);
				state = unlink(buf);
				if (state == ERROR) {
					printf("Error in meta check(IS_STAT) : stat error (after stat) \n");
					goto ERRORS;
				}
				memset(rename_buf, 0x0, sizeof(buf));
			}

			gettimeofday(&tv_total_delete_end, NULL);
			execution_time_each[cur_thread][2] = get_time(&tv_total_stat_end, &tv_total_delete_end, IS_TOTAL);

			sync();
			/*
			for(i=0 ; i < count ;i++){
			    fsync(fd_arr[i]);
			}
			*/

			gettimeofday(&tv_end, NULL);
			execution_time_each[cur_thread][3] = get_time(&tv_total_delete_end, &tv_end, IS_TOTAL);

			time = get_time(&tv, &tv_end, IS_TOTAL);
			execution_time_each[cur_thread][4] = time;

			break;

		case IS_DSTAT :

			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d", cur_thread, i);
				state = mkdir(buf, 0755);
				if (state == ERROR) {
					printf("Error in meta check(IS_DSATA) : %s  directory create error (before rmdir) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
			}

			gettimeofday(&tv, NULL);
			/* measure point  */

			for (i = 0; i < count ; i++) {
				memset(&st, 0x0, sizeof(struct stat));
				memset(buf, 0x0, sizeof(buf));
				sprintf(buf, "./%d/%d", cur_thread, i);
				state = stat(buf, &st);
				if (state == ERROR) {
					printf("Error in meta check(IS_DSTAT) : %s  stat error (measuring stat) \n", buf);
					goto ERRORS;
				}
			}

			/* measure point */
			gettimeofday(&tv_end, NULL);

			for (i = 0; i < count ; i++) {
				sprintf(buf, "./%d/%d", cur_thread, i);
				state = rmdir(buf);
				if (state == ERROR) {
					printf("Error in meta check(IS_DSTAT) : %s  directory delete error (measuring rmdir) \n", buf);
					goto ERRORS;
				}
				memset(buf, 0x0, sizeof(buf));
			}



			time = get_time(&tv, &tv_end, IS_RMDIR);
			break;


		default:
			printf("Invalid metadata type setting  Error\n");
			break;

		}

		execution_time[cur_thread] = time;
	}
	printf("End thread %d time : %Lf sec \n", cur_thread, time);

	return (void *)SUCCESS;

ERRORS:

	return (void *)ERROR;

}


char *tmp_op[5] = {"create", "stat", "unlink", "fsync", "total"};

int _perf_metadata(int meta_check, int count, int threads)
{
	pthread_t *pthreads = NULL;
	int res = -1;
	int i, state, j;
	char str_dir[1024] = {0,};
	long double result = 0.0;

	execution_time = (double *)malloc(sizeof(double) * threads);
	memset(execution_time, 0x0, sizeof(double)*threads);

	execution_time_each = (double **)malloc(sizeof(double *) * (threads + 1));
	for (i = 0; i < threads + 1 ; i++) {
		execution_time_each[i] = (double *)malloc(sizeof(double) * 5); //create, stat, unlink, sync, total
		memset(execution_time_each[i], 0x0, sizeof(double) * 5);
	}


	pthreads = (pthread_t *)malloc(sizeof(pthread_t) * threads);
	memset(pthreads, 0x0, sizeof(pthread_t) * threads);

	for (i = 0 ; i < threads ; i++) {
		memset(str_dir, 0x0, 1024);
		sprintf(str_dir, "%d", i);
		state = mkdir(str_dir, 0755);
		if (state == ERROR) {
			perror("_perf_metadata - mkdir");
			printf(" _perf_metadata - mkdir error before test in %d \n", i);
			return ERROR;
		}
	}

	struct thread_args *args = (struct thread_args *)malloc(sizeof(struct thread_args) * threads);
	for (i = 0; i < threads ; i++) {
		args[i].meta_check = meta_check;
		args[i].count = count;
		args[i].cur_thread = i;
	}

	for (i = 0 ; i < threads; i++) {
		res = pthread_create(&pthreads[i], NULL, do_metadata_test, (void *)&args[i]);
		if (res == ERROR) {
			printf(" Error: _perf_metadata - pthread_create error \n");
			goto TEST_ERROR;
		}
	}

	for (i = 0 ; i < threads ; i++) {
		pthread_join(pthreads[i], (void **)&res);
	}

	for (i = 0 ; i < threads ; i++) {
		memset(str_dir, 0x0, 1024);
		sprintf(str_dir, "%d", i);
		state = rmdir(str_dir);
		if (state == ERROR) {
			printf(" _perf_metadata - rmdir error after test  %d \n", i);
			goto TEST_ERROR;
		}
	}

	for (i = 0 ; i < threads ; i++) {
		result += execution_time[i];
		for (j = 0; j < 5 ; j++) {
			execution_time_each[threads][j] += execution_time_each[i][j];
		}
	}

	printf("execution_time : %Lf sec \n", result);

	puts("");
	for (i = 0 ; i < 5 ; i++) {
		printf("%10s    time : %f sec \n", tmp_op[i], execution_time_each[threads][i]);
	}



	return SUCCESS;
TEST_ERROR:
	free(pthreads);
	return ERROR;
}


static int perf_metadata(s32 meta_check, s32 count, s32 threads)
{
	int res;

	res = _perf_metadata(meta_check, count, threads);
	if (res < 0) {
		printf(" Error: metadata DEBUG \n");
		return ERROR;
	}

	return SUCCESS;
}

int main(int argc, char *argv[])
{
	int ret = 0, count = DEFAULT_COUNT, threads = DEFAULT_THREAD;
	int meta_check = IS_NOTHING;
	char op;
	int need_debug = 0;
#if DEBUG
	int test_val = 0;
#endif

	if (argc == 1) {
		goto INVALID_ARGS;
	}

	/* optind must be reset before using getopt() */
	optind = 0;
	while ((op = getopt(argc, argv, "M:C:T:")) != -1) {
		switch (op) {
		case 'M':
			if (!strcmp(optarg, "open_close")) {
#if DEBUG
				printf("main %d\n", test_val++);
#endif
				meta_check = IS_OPEN_CLOSE;
			} else if (!strcmp(optarg, "readdir")) {
				meta_check = IS_READDIR;
			} else if (!strcmp(optarg, "unlink")) {
				meta_check = IS_UNLINK;
			} else if (!strcmp(optarg, "create")) {
				meta_check = IS_CREATE;
			} else if (!strcmp(optarg, "rename")) {
				meta_check = IS_RENAME;
			} else if (!strcmp(optarg, "mkdir")) {
				meta_check = IS_MKDIR;
			} else if (!strcmp(optarg, "rmdir")) {
				meta_check = IS_RMDIR;
			} else if (!strcmp(optarg, "total")) {
				meta_check = IS_TOTAL;
			} else if (!strcmp(optarg, "dstat")) {
				meta_check = IS_DSTAT;
			} else {
				fprintf(stderr, " Invalid ioengine type = %s", optarg);
				goto INVALID_ARGS;
			}
			break;
		case 'C':
			count = atoi(optarg);
			break;
		case 'T':
			threads = atoi(optarg);
			break;
		default:
			goto INVALID_ARGS;
		}
	}

#if DEBUG
	printf("main %d\n", test_val++);
#endif

	if (meta_check != IS_NOTHING) {
		if (count == 0)
			printf("count is not set default repetition is one \n");
#if DEBUG
		printf("main %d\n", test_val++);
#endif
		ret = perf_metadata(meta_check, count, threads);
#if DEBUG
		printf("main %d\n", test_val++);
#endif
		if (ret == ERROR) {
			printf("perf_metadata error \n");
			goto INVALID_ARGS;
		} else {
			return SUCCESS;
		}



	}

INVALID_ARGS:
	perf_kernel_usage(argv[0]);
	return -1;
}
