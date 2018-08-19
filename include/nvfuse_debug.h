/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2017 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 09/06/2017
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

#ifndef __NVFUSE_DEBUG__
#define __NVFUSE_DEBUG__

#define EXAMPLE		(1 << 0)
#define INODE		(1 << 1)
#define BUFFER		(1 << 2)
#define MEMALLOC	(1 << 3)
#define IPC			(1 << 4)
#define SPDK		(1 << 5)
#define FORMAT		(1 << 6)
#define MOUNT		(1 << 7)
#define BPTREE		(1 << 8)
#define API			(1 << 9)
#define DIRECTORY	(1 << 10)
#define BLOCK		(1 << 11) /* block information */
#define BD			(1 << 12) /* block descriptor */
#define IO			(1 << 13)
#define SB			(1 << 14) /* super block */
#define DEV			(1 << 15)
#define AIO			(1 << 16)
#define STAT		(1 << 17)
#define ETC			(1 << 18)
#define TEST		(1 << 19)
#define BH			(1 << 20)
#define BC			(1 << 21)
#define FLUSHWORK	(1 << 22)
#define REACTOR		(1 << 23)
#define FIO			(1 << 24)
#define NONE		(0)

#define DEBUG_ERROR_OPTS	(REACTOR | FLUSHWORK | INODE | BUFFER | MEMALLOC | EXAMPLE | IPC | SPDK | FORMAT | MOUNT | BPTREE | API | BLOCK | BD | IO | SB | AIO | TEST)
#define DEBUG_WARNING_OPTS	(REACTOR | FLUSHWORK | INODE | BUFFER | MEMALLOC | EXAMPLE | IPC | SPDK | FORMAT | MOUNT | BPTREE | API | BLOCK | BD | IO | SB | AIO | TEST)
#define DEBUG_INFO_OPTS		(FIO | REACTOR | FLUSHWORK | BUFFER | MEMALLOC | EXAMPLE | IPC | SPDK | FORMAT | MOUNT | BPTREE | API | BLOCK | IO | SB | AIO | STAT | TEST)
#define DEBUG_DEBUG_OPTS	(REACTOR | FLUSHWORK | FORMAT)

#define COLOR_RESET   "\x1b[0m"
#define COLOR_RED     "\x1b[31m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_CYAN    "\x1b[36m"

#ifdef DEBUG

	/* Error Message Print */
	#define dprintf_error(opt, fmt, args...) \
			if (opt & DEBUG_ERROR_OPTS) \
				printf(COLOR_RED "ERR" COLOR_RESET "[%s|%s():%d:cpu%d]" fmt, __FILE__, __FUNCTION__, __LINE__, rte_lcore_id(), ##args)

	/* Warning Message Print */
	#define dprintf_warn(opt, fmt, args...) \
			if (opt & DEBUG_WARNING_OPTS) \
				printf(COLOR_YELLOW "WARN" COLOR_RESET "[%s|%s():%d:cpu%d]" fmt, __FILE__, __FUNCTION__, __LINE__, rte_lcore_id(), ##args)

	/* Info Message Print */
	#define dprintf_info(opt, fmt, args...) \
			if (opt & DEBUG_INFO_OPTS) \
				printf(COLOR_GREEN "INFO" COLOR_RESET "[%s|%s():%d:cpu%d]" fmt, __FILE__, __FUNCTION__, __LINE__, rte_lcore_id(), ##args)

	/* Debug Message Print */
	#define dprintf_debug(opt, fmt, args...) \
			if (opt & DEBUG_DEBUG_OPTS) \
				printf(COLOR_BLUE "DEBUG" COLOR_RESET "[%s|%s():%d:cpu%d]" fmt, __FILE__, __FUNCTION__, __LINE__, rte_lcore_id(), ##args)

#else
#	define dprintf(opt, fmt, args...)
#endif

/* Specific Debug Options */
//#define DEBUG_FLUSH_DIRTY_INODE
//#define DEBUG_INODE_LIST
//#define DEBUG_BC_COUNT

#endif /* __NVFUSE_DEBUG__ */
