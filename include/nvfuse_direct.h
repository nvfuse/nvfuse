/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_DIRECT_H
#define NVFUSE_DIRECT_H
#include <stdint.h>
struct nvfuse_handle;
/* Standalone filesystem-cache bypass, not a host O_DIRECT promise. Buffer,
 * count and offset must be 4096-aligned. Metadata and DATA-TX overlays remain
 * coherent. Calls are synchronous and require the handle's owner thread. */
int nvfuse_pread_direct(struct nvfuse_handle *,unsigned fd,void *,unsigned count,int64_t offset);
int nvfuse_pwrite_direct(struct nvfuse_handle *,unsigned fd,const void *,unsigned count,int64_t offset);
#endif
