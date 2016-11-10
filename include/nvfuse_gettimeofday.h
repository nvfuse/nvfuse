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
#if NVFUSE_OS == NVFUSE_OS_WINDOWS
#	include <Windows.h>
#else
#	include <sys/time.h>
#	include <time.h>
#endif

#ifndef GETTIMEOFDAY_H
#define GETTIMEOFDAY_H

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
int gettimeofday(struct timeval *tp, void *tzp);
#endif

int timeval_subtract (struct timeval *result,struct timeval *x, struct timeval *y);
void timeval_add(struct timeval *t1,struct timeval *t2);
#endif /* GETTIMEOFDAY_H */
