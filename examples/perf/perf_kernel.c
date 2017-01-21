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

#define IS_NOTHING      0
#define IS_OPEN_CLOSE   1
#define IS_READDIR      2
#define IS_UNLINK       3
#define IS_CREATE       4
#define IS_RENAME       5
#define IS_MKDIR        6
#define IS_RMDIR        7
#define IS_OP           8

char *op_list[IS_OP] = {"nothing", "open_close","readdir", "unlink","creat", "rename", "mkdir", "rmdir"};

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
#define DEBUG_TIME 1
#define DEBUG_FSYNC 0

static int print_timeval(struct timeval *tv, struct timeval *tv_end, int op_index){

    double tv_s = tv->tv_sec + (tv->tv_usec / 1000000.0);
    double tv_e = tv_end->tv_sec + (tv_end->tv_usec / 1000000.0);
#if DEBUG_TIME
    printf("    %s start : %lf  micro seconds \n",op_list[op_index],tv_s);
    printf("    %s end   : %lf  micro seconds \n",op_list[op_index],tv_e);
#endif
    printf("    %s : %lf              seconds \n", op_list[op_index],tv_e -tv_s);
}


int _perf_metadata(char* str,int meta_check,int count)
{
	struct timeval tv, tv_end;
    struct dirent *dir_entry;
    int flags_create, flags_rdwr,fd, i;

    char *path_dir = "test_direcpty";
    char *path_file = "test_file.txt";
    DIR *dir_info;

    char buf[20] = {0,};
    char rename_buf[20] = {0,};

    s32 state;
            
    flags_create = O_WRONLY | O_CREAT | O_TRUNC;
    flags_rdwr = O_RDWR;

#if DEBUG
    int test_val = 0;
    printf("_perf_metadata %d \n",test_val++);
#endif
    if(meta_check != IS_NOTHING){
 
        switch(meta_check){
            case IS_OPEN_CLOSE :
                printf("metadata operation - open_close operation ...\n");

                fd = open(path_file,flags_create,0644);
                if (fd < 0){
                    printf("Error in meta check(IS_OPEN) : %s file open error (before open/close) \n",path_file);
                    goto ERRORS;
                }
                write(fd,str,strlen(str));
#if DEBUG_FSYNC
                fsync(fd);
#endif
                close(fd);
#if DEBUG
    printf("_perf_metadata %d \n",test_val++);
#endif
                gettimeofday(&tv,NULL);                                   
                /* measure point  */
                for(i=0; i < count ; i++){
                    fd = open(path_file,flags_rdwr,0644);
                    if (fd < 0){
                        printf("Error in meta check(IS_OPEN) : %s  existing file open error (measuring open/close) \n", path_file);
                        goto ERRORS;
                    }
                    close(fd);
                }
                /* measure point  */
                gettimeofday(&tv_end,NULL);

                state = unlink(path_file);
                if(state != SUCCESS){
                    printf("Error in meta check(IS_OPEN) : %s file delelte error (after open/close) \n",path_file);
                    goto ERRORS;
                }

                print_timeval(&tv,&tv_end,IS_OPEN_CLOSE);
                break;

            case IS_READDIR :
                printf("metadata operation - readdir operation ...\n");
                state = mkdir(path_dir, 0755);
                if(state == ERROR){
                    printf("Error in meta check(IS_READDIR) : %s mkdir error (before readdir) \n",path_dir);                    
                    goto ERRORS;
                }

                for(i=0; i < 3 ; i++){
                    sprintf(buf,"%s/%d.txt",path_dir,i);
                    fd = open(buf,flags_create,0644);
                    if (fd < 0){
                        printf("Error in meta check(IS_READDIR) : %s file  create error (before readdir) \n",buf);
                        goto ERRORS;
                    }
                    memset(buf,0x0,sizeof(buf));
#if DEBUG_FSYNC
                    fsync(fd);
#endif                    
                    close(fd);
                }
                dir_info = opendir(path_dir);
                i = 0;

                gettimeofday(&tv,NULL);
                /* measure point  */
                for(i=0; i < count ; i++){
                    while(dir_entry = readdir(dir_info)){
                        if(!dir_entry){
                            printf("meta check(IS_READDIR) : readdir error (measuring readdir)\n");
                            goto ERRORS;
                        }
                    }
                }
                /* measure point  */
                gettimeofday(&tv_end,NULL);

                for(i=0; i < 3 ; i++){
                    sprintf(buf,"%s/%d.txt",path_dir,i);
                    state = unlink(buf);
                    if(state != SUCCESS){
                        printf("Error in meta check(IS_READDIR) : %s unlink error (after readdir) \n",buf);
                        goto ERRORS;
                    }

                    memset(buf,0x0,sizeof(buf));
                }

                state = rmdir(path_dir);
                if(state == ERROR){
                    printf("Error in meta check(IS_READDIR) : %s  rmdir error (after readdir) \n",path_dir);                    
                    goto ERRORS;
                }
                print_timeval(&tv,&tv_end,IS_READDIR);
                break;

                /* measure the unlink operation to empty file */
            case IS_UNLINK :
                printf("metadata operation - unlink operation ...\n");

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    fd = open(buf,flags_create,0644);
                    if (fd < 0){
                        printf("Error in meta check(IS_UNLINK) : %s create error (before unlink) \n",buf);
                        goto ERRORS;
                    }
                    memset(buf,0x0,sizeof(buf));
#if DEBUG_FSYNC
                    fsync(fd);
#endif              
                    close(fd);
                }

                gettimeofday(&tv,NULL);
                /* measure point  */
                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    state = unlink(buf);
                    if(state == ERROR){
                        printf("Error in meta check(IS_UNLINK) : %s file unlink error (measuring unlink) \n", buf);
                        goto ERRORS;
                    }
                    memset(buf,0x0,sizeof(buf));
                }
                /* measure point  */
                gettimeofday(&tv_end,NULL);
                print_timeval(&tv,&tv_end,IS_UNLINK);
                break;

                /* measure the create operation to empty file */
            case IS_CREATE :
                printf("metadata operation - create operation ...\n");

                gettimeofday(&tv,NULL);
                /* measure point  */
                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    fd = open(buf,flags_create,0644);
                    if (fd < 0){
                        printf("Error in meta check(IS_CREATE) : %s creat error (measuring create) \n",buf);
                        goto ERRORS;
                    }
                    memset(buf,0x0,sizeof(buf));
#if DEBUG_FSYNC
                    fsync(fd);
#endif              
                    close(fd);
                }
                /* measure point  */

                gettimeofday(&tv_end,NULL);

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    state = unlink(buf);
                    if(state == ERROR){
                        printf("Error in meta check(IS_CREATE) : %s unlink error (after create) \n", buf);
                        goto ERRORS;
                    }
                    memset(buf,0x0,sizeof(buf));
                }
                print_timeval(&tv,&tv_end,IS_CREATE);

                break;

                /* measure the rename operation to existing file */
            case IS_RENAME :
                printf("metadata operation - rename operation ...\n");
                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    fd = open(buf,flags_create,0664);
                    if (fd < 0){
                        printf("Error in meta check(IS_RENAME) : %s  file create error (before rename) \n", buf);
                        goto ERRORS;
                    }
#if DEBUG_FSYNC
                    fsync(fd);
#endif              
                    close(fd);
                    memset(buf,0x0,sizeof(buf));
                }

                gettimeofday(&tv,NULL);

                /* measure point  */
                for(i=0; i < count ; i++){
                    sprintf(buf,"%d.txt",i);
                    sprintf(rename_buf, "%d_rename.txt",i);
                    state = rename(buf,rename_buf); 
                    if(state == ERROR){
                        printf("Error in meta check(IS_RENAME) : rename %s to %s  error (measuring rename) \n", buf,rename_buf);
                    }
                    memset(buf,0x0,sizeof(buf));
                    memset(rename_buf,0x0,sizeof(rename_buf));
                }
                /* measure point  */

                gettimeofday(&tv_end,NULL);
                for(i=0; i < count ; i++){
                    sprintf(rename_buf, "%d_rename.txt",i);
                    state = unlink(rename_buf);
                    if(state == ERROR){
                        printf("Error in meta check(IS_RENAME) : %s  file delelte error (after rename) \n",rename_buf);
                        goto ERRORS;
                    }
                    memset(rename_buf,0x0,sizeof(rename_buf));
                }

                print_timeval(&tv,&tv_end,IS_RENAME);
                break;

            case IS_MKDIR :
                printf("metadata operation - mkdir operation ...\n");

                gettimeofday(&tv,NULL);

                /* measure point  */
                for(i=0; i < count ; i++){
                    sprintf(buf,"%d",i);
                    state = mkdir(buf, 0755);
                    if(state == ERROR){
                        printf("Error in meta check(IS_MKDIR) : %s  directory create error (measuring mkdir) \n",buf);
                        goto ERRORS;
                    }
                    memset(buf,0x0,sizeof(buf));
                }
                /* measure point  */

                gettimeofday(&tv_end,NULL);

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d",i);
                    state = rmdir(buf);
                    if(state == ERROR){
                        printf("Error in meta check(IS_MKDIR) : %s  directory delete error (after mkdir) \n", buf);
                        goto ERRORS;
                    }
                    memset(buf,0x0,sizeof(buf));
                }

                print_timeval(&tv,&tv_end,IS_MKDIR);               
                break;
            case IS_RMDIR :
                printf("metadata operation - rmdir operation ...\n");

                for(i=0; i < count ; i++){
                    sprintf(buf,"%d",i);
                    state = mkdir(buf, 0755);                
                    if(state == ERROR){
                        printf("Error in meta check(IS_RMDIR) : %s  directory create error (before rmdir) \n",buf );
                        goto ERRORS;
                    }                        
                    memset(buf,0x0,sizeof(buf));
                }
                gettimeofday(&tv,NULL);

                /* measure point  */
                for(i=0; i < count ; i++){
                    sprintf(buf,"%d",i);
                    state = rmdir(buf);
                    if(state == ERROR){
                        printf("Error in meta check(IS_RMDIR) : %s  directory delete error (measuring rmdir) \n",buf );
                        goto ERRORS;
                    }
                    memset(buf,0x0,sizeof(buf));
                }

                /* measure point */
                gettimeofday(&tv_end,NULL);

                print_timeval(&tv,&tv_end,IS_RMDIR);               
                break;

            default:
                printf("Invalid metadata type setting  Error\n");
                break;

        }

    }
     
    return SUCCESS;

ERRORS:

	return ERROR;
}

#define FNAME_SIZE 128

static int perf_metadata(s32 meta_check, s32 count)
{	
	char str[FNAME_SIZE];
	s32 res;

	sprintf(str, "file_allocate_test");

	res = _perf_metadata(str,meta_check,count);
	if (res < 0)
	{
		printf(" Error: metadata DEBUG \n");
		return ERROR;
	}

	return SUCCESS;
}

void perf_kernel_usage(char *cmd)
{
	printf("\nOptions for %s: \n",cmd);
	printf("   -M: metadata intensive operation to measure (e.g., open_close , readdir , unlink , create , rename , mkdir , rmdir)\n");
	printf("   -C: repetition to measure metadata intensice operation (default is 1)\n\n");
}


int main(int argc, char *argv[])
{
	int ret = 0, count = 0;  
    int meta_check = IS_NOTHING; 
	char op;
    int need_debug=0;
#if DEBUG
    int test_val = 0;
#endif

	if (argc == 1)
	{
		goto INVALID_ARGS;
	}
	
	/* optind must be reset before using getopt() */
	optind = 0;
	while ((op = getopt(argc,argv,"M:C:")) != -1) {
        switch (op) {
            case 'M':
                if (!strcmp(optarg, "open_close")){
#if DEBUG 
                    printf("main %d\n",test_val++);
#endif
                    meta_check = IS_OPEN_CLOSE;	
                }else if (!strcmp(optarg, "readdir")){
                    meta_check = IS_READDIR;
                }else if (!strcmp(optarg, "unlink")){
                    meta_check = IS_UNLINK;
                }else if (!strcmp(optarg, "create")){
                    meta_check = IS_CREATE;   
                }else if (!strcmp(optarg, "rename")){
                    meta_check = IS_RENAME;
                }else if (!strcmp(optarg, "mkdir")){
                    meta_check = IS_MKDIR;
                }else if (!strcmp(optarg, "rmdir")){
                    meta_check = IS_RMDIR;
                }else{
                    fprintf( stderr, " Invalid ioengine type = %s", optarg);
                    goto INVALID_ARGS;
                }
                break;
            case 'C':
                count = atoi(optarg);
                break;
            default:
                goto INVALID_ARGS;
        }
	}

#if DEBUG
        printf("main %d\n",test_val++);
#endif

    if(meta_check != IS_NOTHING){
        if(count == 0)
            printf("count is not set default repetition is one \n");
#if DEBUG
        printf("main %d\n",test_val++);
#endif
		ret = perf_metadata(meta_check, count);
#if DEBUG
        printf("main %d\n",test_val++);
#endif
        if(ret == ERROR){
            printf("perf_metadata error \n");
            goto INVALID_ARGS;
        }else{
            return SUCCESS;
        }
	}

INVALID_ARGS:
    perf_kernel_usage(argv[0]);
	return -1;
}
