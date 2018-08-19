/*
*       NVFUSE Extended Attribute 
*       Copyright (C) 2017 Hankeun Son <hankeun.son@sk.com>
*       First Writing: 30/05/2017
*
*       This code based NVFUSE (NVMe based File System in Userspace)
*       Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>

* xattr consolidation Copyright (c) 2004 James Morris <jmorris@redhat.com>,
* Red Hat Inc.
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


/*
  NVFUSE Extended attribute.
*/

#include <sys/statvfs.h>
#include "nvfuse_types.h"

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
#endif

#ifdef SPDK_ENABLED
#include "spdk/nvme.h"
#include "spdk/env.h"
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

struct nvfuse_xattr_header {
	void *first;
	void *last;
	u16 xattr_entry_cnt;
};

struct nvfuse_xattr_list {
	u16 xid;
	u32 len;
	void *next;
};

struct nvfuse_xattr_entry {
        u32 e_name_len;		//size_t e_name_len;	// limit 256 >= u8, 8bit
        u32 e_value_size;	//size_t e_value_size;	// limit 4096(3900) >  10bit
	// xid;			// 14bit (?)
        char e_name[0];
};


s32 nvfuse_set_xattr(struct nvfuse_handle *nvh, const char *path, const char *name, const char *value);
s32 nvfuse_remove_xattr(struct nvfuse_handle *nvh, const char *path, const char *name);
s32 nvfuse_get_xattr(struct nvfuse_handle *nvh, const char *path, const char *name, char *buffer, size_t buf_size);
s32 nvfuse_list_xattr(struct nvfuse_handle *nvh, const char *path, char *buffer, size_t buffer_size);
