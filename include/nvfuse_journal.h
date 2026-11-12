#ifndef NVFUSE_JOURNAL_H
#define NVFUSE_JOURNAL_H
#include <stddef.h>
#include <stdint.h>
#define NVJ_BLOCK 4096u
#define NVJ_GROUP_BLOCKS 32768u
#define NVJ_BLOCKS 4096u
#define NVJ_START (NVJ_GROUP_BLOCKS - NVJ_BLOCKS)
#define NVJ_DESCRIPTORS 16u
#define NVJ_PAYLOAD (3u + NVJ_DESCRIPTORS)
#define NVJ_CAPACITY (NVJ_BLOCKS - NVJ_PAYLOAD - 2u)
#define NVJ_SIGNATURE UINT32_C(0x4a46564e)
#define NVJ_META_SIGNATURE UINT32_C(0x4a4d564e)
struct nvj;
struct nvj_ops {
    int (*transfer)(void *, uint64_t, size_t, void *, int);
    int (*flush)(void *);
    void *context;
};
struct nvj_stats {
    uint64_t commits, replays, journal_bytes, home_bytes, flushes, peak_blocks;
};
typedef void (*nvj_hook)(void *, const char *, uint64_t);
/* New format only. Caller reserves the extent and initializes home metadata. */
int nvj_format(const struct nvj_ops *, uint64_t bytes, int metadata_only);
int nvj_probe(const struct nvj_ops *, uint64_t device_bytes, uint64_t *volume_bytes, int metadata_only);
int nvj_open(struct nvj **, const struct nvj_ops *, uint64_t bytes, int metadata_only);
void nvj_close(struct nvj *);
int nvj_begin(struct nvj *, int full_data, unsigned budget);
int nvj_commit(struct nvj *);
void nvj_discard(struct nvj *);
void nvj_mark_metadata(struct nvj *, uint32_t block);
int nvj_transfer(struct nvj *, uint64_t, size_t, void *, int);
int nvj_flush(struct nvj *);
int nvj_error(const struct nvj *);
void nvj_fail(struct nvj *, int error);
void nvj_require_recovery(struct nvj *);
int nvj_active(const struct nvj *);
int nvj_recovery_required(const struct nvj *);
const struct nvj_stats *nvj_stats(const struct nvj *);
void nvj_set_hook(struct nvj *, nvj_hook, void *);
#endif
