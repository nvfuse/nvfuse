#ifndef NVFUSE_TX_H
#define NVFUSE_TX_H
#include "nvfuse_journal.h"
struct nvfuse_handle;
enum nvfuse_tx_mode { NVFUSE_TX_METADATA=1, NVFUSE_TX_DATA=2 };
enum nvfuse_tx_state { NVFUSE_TX_IDLE=0, NVFUSE_TX_ACTIVE=1, NVFUSE_TX_ABORT_ONLY=2,
                       NVFUSE_TX_RECOVERY_REQUIRED=3 };
/* Standalone journal format only. One calling thread, no nesting.
 * Abort invalidates descriptors opened after begin. File contents roll back
 * only in DATA mode. Sync APIs inside an explicit TX return -EBUSY.
 * Begin requires no open descriptors. A zero budget uses NVJ_CAPACITY;
 * any participating operation error makes the TX abort-only. */
int nvfuse_tx_begin(struct nvfuse_handle *, enum nvfuse_tx_mode, unsigned block_budget);
int nvfuse_tx_commit(struct nvfuse_handle *);
int nvfuse_tx_abort(struct nvfuse_handle *);
int nvfuse_tx_status(struct nvfuse_handle *);
int nvfuse_tx_get_stats(struct nvfuse_handle *, struct nvj_stats *);
/* Test hooks run only on the owning thread, at named persistence boundaries. */
int nvfuse_tx_set_hook(struct nvfuse_handle *, nvj_hook, void *);
#endif
