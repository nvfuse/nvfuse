#include "nvfuse_portable.h"
#include <pthread.h>

void *spdk_dma_malloc(size_t size, size_t align, uint64_t *phys)
{
    void *ptr = NULL;
    if (phys) { errno = ENOTSUP; return NULL; }
    if (!align) align = 4096;
    int error = posix_memalign(&ptr, align, size);
    if (error) { errno = error; return NULL; }
    return ptr;
}

void *spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys)
{
    void *ptr = spdk_dma_malloc(size, align, phys);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

struct rte_mempool {
    char *name;
    size_t capacity, size, allocated, available;
    void **objects, **free_objects;
    pthread_mutex_t lock;
    struct rte_mempool *next;
};
static struct rte_mempool *pools;

struct rte_mempool *rte_mempool_lookup(const char *name)
{
    for (struct rte_mempool *p = pools; p; p = p->next)
        if (!strcmp(p->name, name)) return p;
    return NULL;
}

struct rte_mempool *spdk_mempool_create(const char *name, size_t count,
                                      size_t size, size_t cache, int socket)
{
    (void)cache; (void)socket;
    if (!count || !size || rte_mempool_lookup(name)) { errno = EINVAL; return NULL; }
    struct rte_mempool *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->name = strdup(name);
    p->objects = calloc(count, sizeof(void *));
    p->free_objects = calloc(count, sizeof(void *));
    if (!p->name || !p->objects || !p->free_objects) {
        free(p->name); free(p->objects); free(p->free_objects); free(p);
        return NULL;
    }
    p->capacity = count; p->size = size;
    pthread_mutex_init(&p->lock, NULL);
    p->next = pools; pools = p;
    return p;
}

/* Allocate payloads lazily, retaining them for reuse until pool destruction. */
void *spdk_mempool_get(struct rte_mempool *p)
{
    if (!p) return NULL;
    pthread_mutex_lock(&p->lock);
    void *ptr = NULL;
    if (p->available) ptr = p->free_objects[--p->available];
    else if (p->allocated < p->capacity) {
        ptr = calloc(1, p->size);
        if (ptr) p->objects[p->allocated++] = ptr;
    }
    pthread_mutex_unlock(&p->lock);
    return ptr;
}
void spdk_mempool_put(struct rte_mempool *p, void *ptr)
{
    assert(p && ptr);
    pthread_mutex_lock(&p->lock);
    assert(p->available < p->allocated);
    p->free_objects[p->available++] = ptr;
    pthread_mutex_unlock(&p->lock);
}
int rte_mempool_get(struct rte_mempool *p, void **ptr)
{
    *ptr = spdk_mempool_get(p);
    return *ptr ? 0 : -ENOMEM;
}
int rte_mempool_get_bulk(struct rte_mempool *p, void **ptrs, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        if (rte_mempool_get(p, &ptrs[i])) {
            while (i) spdk_mempool_put(p, ptrs[--i]);
            return -ENOMEM;
        }
    }
    return 0;
}
void rte_mempool_put_bulk(struct rte_mempool *p, void *const *ptrs, unsigned count)
{
    for (unsigned i = 0; i < count; i++) spdk_mempool_put(p, ptrs[i]);
}
void spdk_mempool_free(struct rte_mempool *p)
{
    if (!p) return;
    struct rte_mempool **link = &pools;
    while (*link && *link != p) link = &(*link)->next;
    assert(*link == p);
    *link = p->next;
    for (size_t i = 0; i < p->allocated; i++) free(p->objects[i]);
    pthread_mutex_destroy(&p->lock);
    free(p->name); free(p->objects); free(p->free_objects); free(p);
}

/* Process-local rings support existing core statistics, never cross-process IPC. */
struct rte_ring {
    char *name;
    void **items;
    unsigned capacity, head, tail, count;
    struct rte_ring *next;
};
static struct rte_ring *rings;
struct rte_ring *rte_ring_lookup(const char *name)
{
    for (struct rte_ring *r = rings; r; r = r->next)
        if (!strcmp(r->name, name)) return r;
    return NULL;
}
struct rte_ring *rte_ring_create(const char *name, unsigned count, int socket, unsigned flags)
{
    (void)socket; (void)flags;
    if (count < 2 || rte_ring_lookup(name)) return NULL;
    struct rte_ring *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->name = strdup(name); r->items = calloc(count, sizeof(void *));
    if (!r->name || !r->items) { free(r->name); free(r->items); free(r); return NULL; }
    r->capacity = count - 1;
    r->next = rings; rings = r;
    return r;
}
int rte_ring_enqueue(struct rte_ring *r, void *ptr)
{
    if (!r || r->count == r->capacity) return -ENOBUFS;
    r->items[r->head] = ptr; r->head = (r->head + 1) % r->capacity; r->count++;
    return 0;
}
int rte_ring_dequeue(struct rte_ring *r, void **ptr)
{
    if (!r || !r->count) return -ENOENT;
    *ptr = r->items[r->tail]; r->tail = (r->tail + 1) % r->capacity; r->count--;
    return 0;
}
void rte_ring_free(struct rte_ring *r)
{
    if (!r) return;
    struct rte_ring **link = &rings;
    while (*link && *link != r) link = &(*link)->next;
    assert(*link == r); *link = r->next;
    free(r->name); free(r->items); free(r);
}
