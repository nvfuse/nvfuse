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

#include <time.h>
#include <stdio.h>
#include "nvfuse_config.h"

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
#	include <windows.h>
#elif NVFUSE_OS == NVFUSE_OS_LINUX
#	include <unistd.h>
#	include <sys/time.h>
#endif

int timeval_subtract (struct timeval *result, struct timeval *x, struct timeval *y)
{
	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_usec < y->tv_usec) {
		int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
		y->tv_usec -= 1000000 * nsec;
		y->tv_sec += nsec;
	}
	if (x->tv_usec - y->tv_usec > 1000000) {
		int nsec = (x->tv_usec - y->tv_usec) / 1000000;
		y->tv_usec += 1000000 * nsec;
		y->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait.
	*      tv_usec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_usec = x->tv_usec - y->tv_usec;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

void timeval_add(struct timeval *t1, struct timeval *t2)
{
	if((t1->tv_usec + t2->tv_usec ) < 1000000){
		t1->tv_usec += t2->tv_usec;
		t1->tv_sec += t2->tv_sec;
	}else{
		int nsec = (t1->tv_usec + t2->tv_usec) / 1000000;
		t1->tv_usec += t2->tv_usec;
		t1->tv_usec -= 1000000*nsec;
		t1->tv_sec += t2->tv_sec;
		t1->tv_sec += nsec;
	}
}

double time_since_ms(struct timeval *start_tv, struct timeval *stop_tv)
{
    double sec, usec;
    double ret;
    sec = stop_tv->tv_sec - start_tv->tv_sec;
    usec = stop_tv->tv_usec - start_tv->tv_usec;
    if (sec > 0 && usec < 0) {
        sec--;
	usec += 1000000;
    } 
    ret = sec *1000 + usec / (double)1000;
    if (ret < 0)
        ret = 0;
    return ret;
}

double time_since(struct timeval *start_tv, struct timeval *stop_tv)
{
    double sec, usec;
    double ret;
    sec = stop_tv->tv_sec - start_tv->tv_sec;
    usec = stop_tv->tv_usec - start_tv->tv_usec;
    if (sec > 0 && usec < 0) {
        sec--;
		usec += 1000000;
    } 
    ret = sec + usec / (double)1000000;
    if (ret < 0)
        ret = 0;
    return ret;
}

/*
 * return seconds between start_tv and now in double precision
 */
double time_since_now(struct timeval *start_tv)
{
    struct timeval stop_time;
    gettimeofday(&stop_time, NULL);
    return time_since(start_tv, &stop_time);
}

float tv_to_sec(struct timeval *tv){
	return (float)tv->tv_sec + (float)tv->tv_usec/1000000;
}