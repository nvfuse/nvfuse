/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_TX_H
#define NVFUSE_TX_H
#include "nvfuse_journal.h"
struct nvfuse_handle;
enum nvfuse_tx_mode { NVFUSE_TX_METADATA=1, NVFUSE_TX_DATA=2 };
enum nvfuse_tx_state { NVFUSE_TX_IDLE=0, NVFUSE_TX_ACTIVE=1, NVFUSE_TX_ABORT_ONLY=2,
                       NVFUSE_TX_RECOVERY_REQUIRED=3 };
enum nvfuse_tx_outcome { NVFUSE_TX_UNKNOWN=0, NVFUSE_TX_COMMITTED=1, NVFUSE_TX_IN_PROGRESS=2 };
/* Bounded durable keys: retained until explicitly acknowledged with forget.
 * UNKNOWN after healthy recovery means no retained receipt; never retry a key
 * once forgotten. Keys identify requests, not payload hashes. */
int nvfuse_tx_begin_key(struct nvfuse_handle *,enum nvfuse_tx_mode,unsigned,const unsigned char key[16]);
int nvfuse_tx_query_key(struct nvfuse_handle *,const unsigned char key[16],enum nvfuse_tx_outcome *);
int nvfuse_tx_forget_key(struct nvfuse_handle *,const unsigned char key[16]);
/* Standalone journal format only. One calling thread, no nesting.
 * Abort invalidates descriptors opened after begin. File contents roll back
 * only in DATA mode. Sync APIs inside an explicit TX return -EBUSY.
 * Begin requires no open descriptors. A zero budget uses NVJ_CAPACITY;
 * mutation/storage failures make the TX abort-only; expected read-only misses do not. */
int nvfuse_tx_begin(struct nvfuse_handle *, enum nvfuse_tx_mode, unsigned block_budget);
int nvfuse_tx_commit(struct nvfuse_handle *);
int nvfuse_tx_abort(struct nvfuse_handle *);
int nvfuse_tx_status(struct nvfuse_handle *);
/* Optional file-backend checkpoint worker; SPDK remains owner-thread only. */
int nvfuse_tx_set_async_checkpoint(struct nvfuse_handle *,int enabled);
int nvfuse_tx_get_stats(struct nvfuse_handle *, struct nvj_stats *);
/* Test hooks run at named persistence boundaries; checkpoint hooks may run on the worker when asynchronous mode is enabled. */
int nvfuse_tx_set_hook(struct nvfuse_handle *, nvj_hook, void *);
#endif
