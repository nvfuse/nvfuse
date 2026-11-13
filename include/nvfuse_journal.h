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
/* Process-start NVFUSE_JOURNAL_CRC=auto (default) or software. Immutable,
 * thread-safe dispatch; all implementations produce identical disk CRCs. */
const char *nvj_crc_backend(void);
struct nvj_ops {
    int (*transfer)(void *, uint64_t, size_t, void *, int);
    int (*flush)(void *);
    void *context;
};
#define NVJ_HOME_BATCH_BLOCKS 32u
struct nvj_home_write {
    uint64_t offset;
    size_t bytes;
    const void *buffer;
};
/* Complete ALL writes before returning, even after an individual failure.
 * Destinations are distinct; ordering within this durable-certified home
 * checkpoint is immaterial. No flush or journal/control writes are included. */
typedef int (*nvj_home_writer)(void *, const struct nvj_home_write *, unsigned);
enum nvj_profile_stage {
    NVJ_P_DESCRIPTOR, NVJ_P_ORDERED, NVJ_P_PREPARE, NVJ_P_LOG_WRITE,
    NVJ_P_PAYLOAD_FLUSH, NVJ_P_CERTIFICATE, NVJ_P_HOME_WRITE,
    NVJ_P_HOME_FLUSH, NVJ_P_RETIRE, NVJ_P_COUNT
};
struct nvj_stats {
    uint64_t commits, replays, journal_bytes, home_bytes, flushes, peak_blocks, direct_bytes;
    uint64_t profile_ns[NVJ_P_COUNT];
    uint64_t compare_reads, compare_equal, compare_ns, stage_read_ns;
    uint64_t descriptor_alloc_ns, payload_crc_ns, descriptor_encode_ns, certificate_crc_ns;
    uint64_t home_batches, home_requests;
    uint64_t log_join_commits, log_join_blocks;
};
/* Test filter returns a negative errno before physical I/O, or zero. */
typedef int (*nvj_io_fault)(void *,uint64_t,size_t,int write_op);
void nvj_set_io_fault_hook(struct nvj *,nvj_io_fault,void *);
typedef void (*nvj_hook)(void *, const char *, uint64_t);
/* New format only. Caller reserves the extent and initializes home metadata. */
int nvj_format_sized(const struct nvj_ops *, uint64_t, int metadata_only, unsigned journal_blocks);
/* New disposable/new-volume formats only; v3 is an opt-in compact append log.
 * Existing v1/v2 volumes are never upgraded by open. */
int nvj_format_version(const struct nvj_ops *, uint64_t, int metadata_only,
                       unsigned journal_blocks, unsigned version);
unsigned nvj_capacity(const struct nvj *);
unsigned nvj_journal_start(const struct nvj *);
int nvj_format(const struct nvj_ops *, uint64_t bytes, int metadata_only);
int nvj_probe(const struct nvj_ops *, uint64_t device_bytes, uint64_t *volume_bytes, int metadata_only);
int nvj_open(struct nvj **, const struct nvj_ops *, uint64_t bytes, int metadata_only);
void nvj_close(struct nvj *);
int nvj_prepare_metadata(struct nvj *, unsigned additional);
unsigned nvj_budget(const struct nvj *);
int nvj_full_data(const struct nvj *);
int nvj_begin(struct nvj *, int full_data, unsigned budget);
/* Caller guarantees transfer/flush callbacks may run on a worker thread. */
int nvj_set_async_checkpoint(struct nvj *, int enabled);
/* Owner-only settings; batching preserves flush boundaries and disk format.
 * Diagnostic I/O hooks force per-block transfers to retain their boundaries. */
int nvj_set_batch_io(struct nvj *, int enabled);
/* Combine adjacent descriptors/payload only when it saves a bounded request.
 * Default OFF in generic engines; hooks and batch_io=0 retain reference I/O. */
int nvj_set_log_join(struct nvj *, int enabled);
/* Optional synchronous scatter-write callback, set while inactive. Each call
 * contains at most NVJ_HOME_BATCH_BLOCKS blocks total. Diagnostic hooks/faults
 * and batch_io=0 retain the ordinary per-block checkpoint path. */
int nvj_set_home_writer(struct nvj *, nvj_home_writer, void *context);
/* Default ON for generic engines. OFF stages complete protected images
 * directly; partial writes still read the old block to preserve other bytes.
 * Identical full images can then consume budget/log space. Set when inactive. */
int nvj_set_compare_reads(struct nvj *, int enabled);
void nvj_set_profile(struct nvj *, int enabled);
int nvj_commit(struct nvj *);
/* Materialize all already committed v3 records and durably reclaim the epoch.
 * Requires no active transaction. v1/v2 commits already checkpoint inline. */
int nvj_checkpoint(struct nvj *);
void nvj_discard(struct nvj *);
unsigned nvj_version(const struct nvj *);
size_t nvj_memory_bytes(const struct nvj *);
/* Enable fixed NVFUSE block-group metadata ranges without per-block indexes. */
void nvj_mark_filesystem_metadata(struct nvj *);
void nvj_mark_metadata(struct nvj *, uint32_t block);
int nvj_transfer(struct nvj *, uint64_t, size_t, void *, int);
/* Serialized owner only: decide whether async I/O must use the overlay.
 * Ordinary data may bypass it; the owner must fence all bypass writes before
 * committing, and record them so a data-only fsync still flushes. */
int nvj_needs_overlay(struct nvj *, uint64_t, size_t, int write_op);
void nvj_note_data_write(struct nvj *);
int nvj_flush(struct nvj *);
int nvj_error(const struct nvj *);
void nvj_fail(struct nvj *, int error);
void nvj_require_recovery(struct nvj *);
int nvj_active(const struct nvj *);
int nvj_recovery_required(const struct nvj *);
const struct nvj_stats *nvj_stats(const struct nvj *);
void nvj_set_hook(struct nvj *, nvj_hook, void *);
#endif
