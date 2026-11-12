#ifndef NVFUSE_TX_INTERNAL_H
#define NVFUSE_TX_INTERNAL_H
#include "nvfuse_tx.h"
#include "nvfuse_buffer_cache.h"
struct nvfuse_superblock;
int nvfuse_sync_superblock(struct nvfuse_superblock *);
struct io_target;
int nvfuse_tx_enter(struct nvfuse_handle *);
int nvfuse_tx_leave(struct nvfuse_handle *, int result);
int nvfuse_file_reload(struct nvfuse_handle *);
#endif
