#ifndef NVFUSE_FIO_SPSC_H
#define NVFUSE_FIO_SPSC_H
#include <stdatomic.h>
#include <stddef.h>
#define NVFIO_RING_SIZE 256U
/* Exactly one producer and one consumer; cursors use unsigned wraparound. */
struct nvfio_ring {
    _Alignas(64) atomic_uint head;
    _Alignas(64) atomic_uint tail;
    void *entries[NVFIO_RING_SIZE];
};
static inline void nvfio_ring_init(struct nvfio_ring *q)
{
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
}
static inline unsigned nvfio_ring_count(struct nvfio_ring *q)
{
    unsigned head = atomic_load_explicit(&q->head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head - tail;
}
static inline int nvfio_ring_push(struct nvfio_ring *q, void *entry)
{
    unsigned head = atomic_load_explicit(&q->head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head - tail == NVFIO_RING_SIZE) return -1;
    q->entries[head % NVFIO_RING_SIZE] = entry;
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 0;
}
static inline void *nvfio_ring_peek(struct nvfio_ring *q)
{
    unsigned tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&q->head, memory_order_acquire);
    return head == tail ? NULL : q->entries[tail % NVFIO_RING_SIZE];
}
static inline void nvfio_ring_consume(struct nvfio_ring *q)
{
    unsigned tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
}
#endif
