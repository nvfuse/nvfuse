/* SPDX-License-Identifier: GPL-2.0-only */
/* Test-only allocation accounting/failure injection around the real allocator.
 * Load only into native_buffer_checks.py, never into an application server. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

static void *(*allocate_dma)(size_t);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static _Atomic long calls, bytes, fail_at = -1;

static void resolve_allocator(void)
{
    *(void **)(&allocate_dma) = dlsym(RTLD_NEXT, "nvfio_dma_alloc");
    if (!allocate_dma) abort();
}

void nvai_probe_reset(long fail)
{
    atomic_store(&calls, 0);
    atomic_store(&bytes, 0);
    atomic_store(&fail_at, fail);
}

long nvai_probe_calls(void) { return atomic_load(&calls); }
long nvai_probe_bytes(void) { return atomic_load(&bytes); }

void *nvfio_dma_alloc(size_t size)
{
    pthread_once(&once, resolve_allocator);
    long call = atomic_fetch_add(&calls, 1);
    if (call == atomic_load(&fail_at)) return NULL;
    void *buffer = allocate_dma(size);
    if (buffer) atomic_fetch_add(&bytes, (long)size);
    return buffer;
}
