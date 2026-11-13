#ifndef NVFUSE_FIO_CACHE_POLICY_H
#define NVFUSE_FIO_CACHE_POLICY_H
#include <stdint.h>
#define NVFIO_SPDK_MEMORY_MB 2048U
#define NVFIO_CACHE_MIN_MB 16U
#define NVFIO_CACHE_MAX_MB (NVFIO_SPDK_MEMORY_MB / 4U)
static inline unsigned nvfio_auto_buffer_mb(uint64_t device_bytes)
{
    /* Divide before converting to MiB so capacity arithmetic cannot overflow. */
    uint64_t target = device_bytes / 1000U / (1024U * 1024U);
    if (target < NVFIO_CACHE_MIN_MB) return NVFIO_CACHE_MIN_MB;
    if (target > NVFIO_CACHE_MAX_MB) return NVFIO_CACHE_MAX_MB;
    return (unsigned)target;
}
#endif
