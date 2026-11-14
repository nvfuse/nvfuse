/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_TX_INTERNAL_H
#define NVFUSE_TX_INTERNAL_H
#include "nvfuse_tx.h"
#include "nvfuse_buffer_cache.h"
struct nvfuse_superblock;
int nvfuse_sync_superblock(struct nvfuse_superblock *);
struct io_target;
int nvfuse_tx_begin_group(struct nvfuse_handle *,enum nvfuse_tx_mode);
int nvfuse_tx_enter(struct nvfuse_handle *);
int nvfuse_tx_leave_lookup(struct nvfuse_handle *, int result);
int nvfuse_tx_leave(struct nvfuse_handle *, int result);
int nvfuse_allocate_range(struct nvfuse_handle *, const char *, s64, s64);
void nvfuse_cleanup_add(struct nvfuse_superblock *, struct nvfuse_inode_ctx *);
int nvfuse_finish_inode_suffix(struct nvfuse_superblock *, struct nvfuse_inode_ctx *);
int nvfuse_resize_inode(struct nvfuse_superblock *, struct nvfuse_inode_ctx *, s64);
int nvfuse_orphan_add(struct nvfuse_superblock *, inode_t);
int nvfuse_orphan_reap(struct nvfuse_handle *);
int nvfuse_file_reload(struct nvfuse_handle *);
#endif
