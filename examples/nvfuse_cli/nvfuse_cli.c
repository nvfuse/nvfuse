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
#include <errno.h>
#ifdef __linux__
#	include <sys/types.h>
#	include <sys/stat.h>
#endif
#include <fcntl.h>
#include "nvfuse_core.h"
#include "nvfuse_config.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_api.h"
#include "imark.h"


s32 nvfuse_type(struct nvfuse_handle *nvh, s8 *str);
int parse_and_execute(char *input);
void postmark_main();

s32 nvfuse_aio_test(struct nvfuse_handle *nvh);

struct nvfuse_handle *g_nvh;
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

#define FALSE	0
#define TRUE	1
#define EOL	1
#define ARG	2

char *input;
char *ptr, *tok;
static char	tokens[512];

/* globla nvfuse handle */
struct nvfuse_handle *g_nvh;
#define INIT_IOM	1
#define FORMAT		0
#define MOUNT		0
#define DEINIT_IOM	1
#define UMOUNT		0


#ifndef __USE_FUSE__
void main(int argc, char *argv[]) {
#else
void mini_main(int argc, char *argv[]){
#endif 

	char linebuf[256];
	char *buf;
	int ret;
	char *devname = NULL;
	struct nvfuse_io_manager *io_manager;

	printf("\n");
	printf(" Caution!: all data stored in your divice is removed permanently. \n");
	printf("\n");

#if NVFUSE_OS == NVFUSE_OS_LINUX
	devname = argv[1];
	if (argc < 2) {
		printf(" please enter the device file (e.g., /dev/sdb)\n");
		return;
	}
	printf(" device name = %s \n", devname);
#endif
	
#	if (EXAM_USE_RAMDISK == 1)
	g_nvh = nvfuse_create_handle(NULL, devname, INIT_IOM, IO_MANAGER_RAMDISK, FORMAT, MOUNT);
#	elif (EXAM_USE_FILEDISK == 1)
	g_nvh = nvfuse_create_handle(NULL, devname, INIT_IOM, IO_MANAGER_FILEDISK, FORMAT, MOUNT);
#	elif (EXAM_USE_UNIXIO == 1)
	g_nvh = nvfuse_create_handle(NULL, devname, INIT_IOM, IO_MANAGER_UNIXIO, FORMAT, MOUNT);
#	elif (EXAM_USE_SPDK == 1)
	g_nvh = nvfuse_create_handle(NULL, devname, INIT_IOM, IO_MANAGER_SPDK, FORMAT, MOUNT);
#	endif
	
	printf("NVFUSE-CLI # ");
	input = linebuf;
	while (fgets(input, 255, stdin)) {
		input[strlen(input)-1] = 0;
		ret = parse_and_execute(input);
		if(ret == -1)
			break;
		printf("NVFUSE-CLI # ");	
	}
	
	nvfuse_destroy_handle(g_nvh, DEINIT_IOM, UMOUNT);
}

int get_token(char **outptr)
{
	int	type;

	*outptr = tok;
	while ((*ptr == ' ') || (*ptr == '\t')) ptr++;

	*tok++ = *ptr;

	switch (*ptr++) {
		case '\0' : type = EOL; break;
		default : type = ARG;
			while ((*ptr != ' ') && (*ptr != '&') &&
				(*ptr != '\t') && (*ptr != '\0'))
				*tok++ = *ptr++;
	}
	*tok++ = '\0';
	return(type);
}

void help()
{
	printf("---------------------------------\n");
	printf("  - quit \n");
	printf("  - exit \n");
	printf("  - dir     - printf directory list\n");
	printf("  - cd      [directory path]\n");
	printf("  - mkdir   [directory name]\n");
	printf("  - rmdir   [direntory name]\n");
	printf("  - mkfile  [filename] [size]\n");
	printf("  - appned  [filename] [size]\n"); 
	printf("  - type    [filename]\n");
	printf("  - pwd     - print name of current working directory \n");
	printf("  - mount   - mount file system \n");
	printf("  - format  - format file system\n");
	printf("  - df      - file system information\n");
	printf("  - diskdump [filename]\n");
	printf("  - diskrecv [filename]\n");
	printf("  - shelp    - help \n");
	printf("---------------------------------\n");

}

int parse_and_execute(char *input)
{
	char	*arg[512];
	int	type;
	int	quit = FALSE;
	int	narg = 0;
	int	finished;
	int temp_ino;
	ptr = input;
	tok = tokens;

	for (finished = 0; !finished; ) {
		switch (type = get_token(&arg[narg])) {
		case ARG :
			narg++;
			break;
		case EOL :
			if (!strcmp(arg[0], "quit")) {
				return -1;
			} else if (!strcmp(arg[0], "exit")) {
				return -1;
			} else if (!strcmp(arg[0],"dir")) {
				nvfuse_dir(g_nvh);
			} else if (!strcmp(arg[0],"cd")) {
				nvfuse_cd(g_nvh, arg[1]);
			} else if (!strcmp(arg[0],"mkdir")) {
				nvfuse_mkdir_path(g_nvh, arg[1], 0);
			} else if (!strcmp(arg[0],"mknod")) {
				nvfuse_mknod(g_nvh, arg[1], 0, 0);
			} else if (!strcmp(arg[0],"rmfile")) {
				nvfuse_rmfile_path(g_nvh, arg[1]);
			} else if (!strcmp(arg[0],"rmdir")) {
				nvfuse_rmdir_path(g_nvh, arg[1]);
			} else if (!strcmp(arg[0],"mkfile")) {
				nvfuse_mkfile(g_nvh, arg[1], arg[2]);					
			} else if (!strcmp(arg[0],"type")) {
				nvfuse_type(g_nvh, arg[1]);
			} else if (!strcmp(arg[0],"mount")) {
				nvfuse_mount(g_nvh);
			} else if (!strcmp(arg[0],"umount")) {
				nvfuse_umount(g_nvh);
			} else if (!strcmp(arg[0], "imark")) {
				imark_main(0, NULL);
			} else if (!strcmp(arg[0],"format")) {
				nvfuse_format(g_nvh);
			} else if (!strcmp(arg[0], "test")) {
				nvfuse_test(g_nvh);
			} else if (!strcmp(arg[0], "aiotest")) {
				nvfuse_aio_test(g_nvh);
			} else if (!strcmp(arg[0], "sync")) {
				nvfuse_sync(g_nvh);
			} else if (!strcmp(arg[0], "help")) {
				help();
			} else if (!strcmp(arg[0],"rdfile")) {
				nvfuse_rdfile(g_nvh, arg[1]);
			} else if (!strcmp(arg[0], "truncate")) {				
				nvfuse_truncate_path(g_nvh, arg[1], atoi(arg[2]));
			} else if (!strcmp(arg[0],"pm")) {
				postmark_main();
			}
			else if (strcmp(arg[0], "")) 
			{ 
				printf("Invalid Command\n");
			}
			narg = 0;
			finished = 1;
			
			break;
		}
	}
	return 0;
}
