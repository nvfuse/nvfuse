/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_QUEUE_H
#define NVFUSE_QUEUE_H
#include <stdint.h>
struct nvfuse_handle;
struct nvfuse_queue;
struct nvfuse_future;
enum nvfuse_queue_operation { NVFUSE_QUEUE_READ=0,NVFUSE_QUEUE_WRITE=1,
    NVFUSE_QUEUE_DIRECT_READ=2,NVFUSE_QUEUE_DIRECT_WRITE=3 };
struct nvfuse_queue_stats { uint64_t submitted,completed,cancelled; };
/* Create/poll/destroy run on the handle owner. Producers may submit/wait/stop
 * from any thread. Mode 0 commits each job; 1/2 commits each polled group in
 * metadata/DATA mode. Descriptor ownership stays with the owner until drained.
 * Buffer ownership stays with a future until its single wait consumes it.
 * Overlapping jobs execute in FIFO admission order. No physical I/O parallelism
 * is implied by multi-thread submission. No callbacks run on producers. */
struct nvfuse_queue *nvfuse_queue_create(struct nvfuse_handle *,unsigned depth,unsigned mode);
struct nvfuse_future *nvfuse_queue_submit(struct nvfuse_queue *,enum nvfuse_queue_operation,
                                        unsigned fd,void *buffer,unsigned bytes,int64_t offset);
int nvfuse_future_wait(struct nvfuse_future *); /* waits, returns result, frees future */
int nvfuse_queue_poll(struct nvfuse_queue *,unsigned maximum);
/* Stop rejects new submissions and cancels queued jobs, retaining in-flight
 * jobs until normal completion. Join producers before destroy. Completed
 * futures remain independently waitable after queue destruction. */
void nvfuse_queue_stop(struct nvfuse_queue *);
int nvfuse_queue_destroy(struct nvfuse_queue *);
void nvfuse_queue_get_stats(struct nvfuse_queue *,struct nvfuse_queue_stats *);
#endif
