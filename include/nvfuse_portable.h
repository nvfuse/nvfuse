#ifndef NVFUSE_PORTABLE_H
#define NVFUSE_PORTABLE_H

#ifdef SPDK_ENABLED
#error "Choose either the file backend or SPDK, not both"
#endif

/* Local, single-process adapters for the subset of env APIs used by the core.
 * This is not an SPDK implementation and provides no DMA or shared memory. */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/uio.h>
#include <sys/statvfs.h>
#include <dirent.h>
/* Keep standalone adapters out of the real SPDK/DPDK symbol namespace. This
 * permits linking the core to a separately compiled modern NVMe transport. */
#define spdk_dma_malloc nvfuse_local_dma_malloc
#define spdk_dma_zmalloc nvfuse_local_dma_zmalloc
#define spdk_mempool_create nvfuse_local_pool_create
#define spdk_mempool_free nvfuse_local_pool_free
#define spdk_mempool_get nvfuse_local_pool_get
#define spdk_mempool_put nvfuse_local_pool_put
#define rte_mempool_lookup nvfuse_local_pool_lookup
#define rte_mempool_get nvfuse_local_pool_get_into
#define rte_mempool_get_bulk nvfuse_local_pool_get_bulk
#define rte_mempool_put_bulk nvfuse_local_pool_put_bulk
#define rte_ring_create nvfuse_local_ring_create
#define rte_ring_lookup nvfuse_local_ring_lookup
#define rte_ring_enqueue nvfuse_local_ring_enqueue
#define rte_ring_dequeue nvfuse_local_ring_dequeue
#define rte_ring_free nvfuse_local_ring_free
#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)

/* Cached objects are reinitialized repeatedly; no OS lock resources to leak. */
typedef struct { atomic_int locked; } rte_spinlock_t;
static inline void rte_spinlock_init(rte_spinlock_t *l) { atomic_init(&l->locked, 0); }
static inline void rte_spinlock_lock(rte_spinlock_t *l) {
    while (atomic_exchange_explicit(&l->locked, 1, memory_order_acquire)) {}
}
static inline void rte_spinlock_unlock(rte_spinlock_t *l) {
    atomic_store_explicit(&l->locked, 0, memory_order_release);
}
static inline int rte_spinlock_is_locked(rte_spinlock_t *l) { return atomic_load(&l->locked); }
typedef struct { atomic_int value; } rte_atomic32_t;
static inline void rte_atomic32_init(rte_atomic32_t *v) { atomic_init(&v->value, 0); }
static inline int rte_atomic32_read(rte_atomic32_t *v) { return atomic_load(&v->value); }
static inline void rte_atomic32_set(rte_atomic32_t *v, int n) { atomic_store(&v->value, n); }
static inline void rte_atomic32_inc(rte_atomic32_t *v) { atomic_fetch_add(&v->value, 1); }
static inline void rte_atomic32_dec(rte_atomic32_t *v) { atomic_fetch_sub(&v->value, 1); }

#define rte_memcpy memcpy
#define SPDK_ENV_SOCKET_ID_ANY (-1)
#define RTE_PROC_PRIMARY 0
static inline int spdk_process_is_primary(void) { return 1; }
static inline int rte_eal_process_type(void) { return RTE_PROC_PRIMARY; }
static inline unsigned rte_lcore_id(void) { return 0; }
static inline int rte_socket_id(void) { return 0; }
static inline uint64_t spdk_get_ticks_hz(void) { return 1000000000ULL; }
static inline uint64_t spdk_get_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
void *spdk_dma_malloc(size_t size, size_t align, uint64_t *phys);
void *spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys);
#define spdk_dma_free free

struct rte_mempool;
#define spdk_mempool rte_mempool
struct rte_mempool *spdk_mempool_create(const char *, size_t, size_t, size_t, int);
void spdk_mempool_free(struct rte_mempool *);
void *spdk_mempool_get(struct rte_mempool *);
void spdk_mempool_put(struct rte_mempool *, void *);
struct rte_mempool *rte_mempool_lookup(const char *);
int rte_mempool_get(struct rte_mempool *, void **);
int rte_mempool_get_bulk(struct rte_mempool *, void **, unsigned);
void rte_mempool_put_bulk(struct rte_mempool *, void *const *, unsigned);
#define rte_mempool_put spdk_mempool_put
#define rte_mempool_free spdk_mempool_free
/* All callers in this standalone build pass NULL initialization callbacks. */
#define rte_mempool_create(n,c,s,cache,priv,mi,ma,oi,oa,socket,flags) \
    spdk_mempool_create(n,c,s,cache,socket)
static inline void rte_malloc_dump_stats(FILE *out, const char *type) { (void)out; (void)type; }
#define rte_panic(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)
#define rte_exit(code, ...) do { fprintf(stderr, __VA_ARGS__); exit(code); } while (0)

struct rte_ring;
struct rte_ring *rte_ring_create(const char *, unsigned, int, unsigned);
struct rte_ring *rte_ring_lookup(const char *);
int rte_ring_enqueue(struct rte_ring *, void *);
int rte_ring_dequeue(struct rte_ring *, void **);
void rte_ring_free(struct rte_ring *);

/* Only needed to keep shared core declarations independent of SPDK headers. */
struct spdk_app_opts { int unused; };
struct spdk_bdev_io;
typedef void (*spdk_bdev_io_completion_cb)(struct spdk_bdev_io *, bool, void *);
enum { SPDK_BDEV_IO_TYPE_READ = 1, SPDK_BDEV_IO_TYPE_WRITE, SPDK_BDEV_IO_TYPE_FLUSH };
#endif
