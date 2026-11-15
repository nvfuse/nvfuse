/* Bounded cache execution while the filesystem owner waits for I/O. */
#ifndef NVFUSE_COMMIT_CACHE_H
#define NVFUSE_COMMIT_CACHE_H
#include <stdatomic.h>
#include <stdint.h>
/* 0 closed, 1 open/idle, 3 open/busy, 2 closing/busy. */
static inline int nvcc_enter(_Atomic unsigned *state)
{
    unsigned expected=1;
    return atomic_compare_exchange_strong_explicit(state,&expected,3,
                    memory_order_acquire,memory_order_relaxed);
}
static inline void nvcc_leave(_Atomic unsigned *state)
{ atomic_fetch_and_explicit(state,~2u,memory_order_release); }
static inline void nvcc_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}
static inline void nvcc_close(_Atomic unsigned *state)
{
    atomic_fetch_and_explicit(state,~1u,memory_order_acq_rel);
    while(atomic_load_explicit(state,memory_order_acquire)&2u)nvcc_pause();
}
struct nvfuse_superblock;
struct nvfuse_handle;
struct nvj;
int nvj_metadata_known_frozen(const struct nvj *,uint32_t);
int nvfuse_commit_cached_append(struct nvfuse_superblock *,int,const void *,unsigned,uint64_t);
void nvfuse_commit_cache_hooks(int (*)(struct nvfuse_handle *,uint32_t),void (*)(void));
int nvfuse_file_data_cache_open(struct nvfuse_superblock *,uint32_t);
void nvfuse_file_data_cache_close(void);
#endif
