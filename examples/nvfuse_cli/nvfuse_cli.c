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

s32 nvfuse_format();
int parse_and_execute(char *input);
s32 nvfuse_type(s8 *str);
void postmark_main();

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

#ifndef __USE_FUSE__
void main(int argc, char *argv[]) {
#else
void mini_main(int argc, char *argv[]){
#endif 

	char linebuf[256];
	char *buf;
	int ret;
	char *devname = NULL;

#if NVFUSE_OS == NVFUSE_OS_LINUX
	devname = argv[1];
	if (argc < 2){
		printf(" please enter the device file (e.g., /dev/sdb)\n");
		return;
	}
	printf(" device name = %s \n", devname);
#endif
	printf("\n");
	printf(" Caution!: all data stored in your divice is removed permanently. \n");
	printf("\n");

	nvfuse_io_manager = (struct nvfuse_io_manager *)malloc(sizeof(struct nvfuse_io_manager));
#if EXAM_USE_RAMDISK == 1 
	nvfuse_init_memio(nvfuse_io_manager, "RANDISK", "RAM");	
#elif EXAM_USE_FILEDISK == 1
	printf(" Open File = %s\n", DISK_FILE_PATH);
	nvfuse_init_fileio(nvfuse_io_manager,"FILE", DISK_FILE_PATH);
#elif EXAM_USE_UNIXIO == 1
	nvfuse_init_unixio(nvfuse_io_manager, "SSD", devname, AIO_MAX_QDEPTH);
#elif EXAM_USE_SPDK == 1
	nvfuse_init_spdk(nvfuse_io_manager, "SPDK", devname, AIO_MAX_QDEPTH);
#endif 
	nvfuse_io_manager->io_open(nvfuse_io_manager, 0);	
	
	printf("NVFUSE-CLI # ");
	input = linebuf;
	while (fgets(input, 255, stdin)) {
		input[strlen(input)-1] = 0;
		ret = parse_and_execute(input);
		if(ret == -1)
			break;
		printf("NVFUSE-CLI # ");	
	}
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
				nvfuse_dir();
			} else if (!strcmp(arg[0],"cd")) {
				nvfuse_cd(arg[1]);
			} else if (!strcmp(arg[0],"mkdir")) {
				nvfuse_mkdir_path(arg[1], 0);
			} else if (!strcmp(arg[0],"mknod")) {
				nvfuse_mknod(arg[1], 0, 0);
			} else if (!strcmp(arg[0],"rmfile")) {
				nvfuse_rmfile_path(arg[1]);
			} else if (!strcmp(arg[0],"rmdir")) {
				nvfuse_rmdir_path(arg[1]);
			} else if (!strcmp(arg[0],"mkfile")) {
				nvfuse_mkfile(arg[1], arg[2]);					
			} else if (!strcmp(arg[0],"type")) {
				nvfuse_type(arg[1]);
			} else if (!strcmp(arg[0],"mount")) {
				nvfuse_mount();
			} else if (!strcmp(arg[0],"umount")) {
				nvfuse_umount();
			} else if (!strcmp(arg[0], "imark")) {
				imark_main(0, NULL);
			} else if (!strcmp(arg[0],"format")) {
				nvfuse_format();
			} else if (!strcmp(arg[0],"test")) {
				nvfuse_test();			
			} else if (!strcmp(arg[0],"help")) {
				help();
			} else if (!strcmp(arg[0],"rdfile")) {
				nvfuse_rdfile(arg[1]);
			} else if (!strcmp(arg[0], "truncate")) {
				nvfuse_rdfile(arg[1]);
				nvfuse_truncate_path(arg[1], atoi(arg[2]));
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
