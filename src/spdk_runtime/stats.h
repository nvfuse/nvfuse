/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFIO_STATS_H
#define NVFIO_STATS_H
#include <stdint.h>
/* Version 2: aligned little-endian uint64_t words, Linux x86_64.
 * Single FS-thread writer. Readers retry if sequence is odd or changes.
 * Durations are API submission -> FS completion, excluding frontend queues. */
#define NVFIO_STATS_MAGIC UINT64_C(0x4e56465354415431)
enum nvfio_stat_word {
    NS_SEQ, NS_MAGIC, NS_VERSION, NS_PID, NS_STATE, NS_TIMESTAMP_NS,
    NS_READ_IOS, NS_WRITE_IOS, NS_READ_BYTES, NS_WRITE_BYTES,
    NS_READ_LAT_TICKS, NS_WRITE_LAT_TICKS, NS_ERRORS,
    NS_INFLIGHT, NS_PENDING, NS_READY, NS_SUBMITTED, NS_COMPLETED,
    NS_TICKS_HZ, NS_FS_CORE, NS_IO_CORE, NS_WORDS
};
#define NVFIO_STATS_BDF_BYTES 32
#define NVFIO_STATS_BDEV_BYTES 128
struct nvfio_stats_record {
    uint64_t word[NS_WORDS];
    char bdf[NVFIO_STATS_BDF_BYTES], bdev[NVFIO_STATS_BDEV_BYTES];
};
#endif
