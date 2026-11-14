/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_API_COMPLETION_H
#define NVFUSE_API_COMPLETION_H
#include <stdatomic.h>
/* One caller and one owner per request. Publish DONE only after the owner's
 * last access: a spinning caller can immediately release the request storage. */
enum { NVFIO_WAITING, NVFIO_PARKED, NVFIO_DONE };
struct nvfio_completion { atomic_int state; };
static inline int nvfio_completion_ready(struct nvfio_completion *c)
{
    return atomic_load_explicit(&c->state, memory_order_acquire) == NVFIO_DONE;
}
static inline int nvfio_completion_park(struct nvfio_completion *c)
{
    int expected = NVFIO_WAITING;
    return atomic_compare_exchange_strong_explicit(&c->state, &expected,
        NVFIO_PARKED, memory_order_acq_rel, memory_order_acquire);
}
static inline int nvfio_completion_finish(struct nvfio_completion *c)
{
    return atomic_exchange_explicit(&c->state, NVFIO_DONE,
        memory_order_release) == NVFIO_PARKED;
}
#endif
