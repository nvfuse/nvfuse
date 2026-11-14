/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_API_QUEUE_H
#define NVFUSE_API_QUEUE_H
#include <stddef.h>
#include <stdatomic.h>
struct nvfio_call_link { struct nvfio_call_link *next; };
struct nvfio_call_queue { _Atomic(struct nvfio_call_link *) head; };
/* Producers never dereference the old head, so a caller may reuse its request
 * after completion. Only the filesystem owner takes and reverses each batch. */
static inline void nvfio_call_push(struct nvfio_call_queue *q, struct nvfio_call_link *n)
{
    n->next = atomic_load_explicit(&q->head, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&q->head, &n->next, n,
        memory_order_release, memory_order_relaxed)) {}
}
static inline struct nvfio_call_link *nvfio_call_take(struct nvfio_call_queue *q)
{
    struct nvfio_call_link *list = atomic_exchange_explicit(&q->head, NULL, memory_order_acquire);
    struct nvfio_call_link *fifo = NULL;
    while (list) {
        struct nvfio_call_link *next = list->next;
        list->next = fifo; fifo = list; list = next;
    }
    return fifo;
}
#endif
