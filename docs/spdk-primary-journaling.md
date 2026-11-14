# Primary API/SPDK metadata journaling

The primary reactor links the shared physical-redo journal engine through one
serialized filesystem owner. This is the asynchronous API/SPDK path used by the
[modern fio engine](spdk-fio.md), separate from the synchronous compatibility
transport used by standalone API and FUSE mounts.

## Format and durability boundary

`NVFUSE_JOURNAL=1` is the runtime default. `0` explicitly selects legacy mode.
A journal volume rejects an OFF mount; a legacy volume rejects an ON mount.
Formatting requires `NVFUSE_FORMAT=1` and destroys existing contents. There is
no implicit conversion or in-place migration.

New journal formats default to v2. Experimental
`NVFUSE_JOURNAL_FORMAT_VERSION=3` selects the [compact append log](journal-compact-log-v3-design.md)
only during explicit formatting. Existing v1/v2/v3 volumes keep their stored
protocol. The default journal reserves 16 MiB at the end of block group 0;
bitmap, group and global free counts exclude this extent. The old discovery
anchor is invalidated and flushed before overwriting home metadata during a
new format, including a legacy/OFF format.

A successful ordinary write completion is not a durability boundary. `fsync`,
`fdatasync`, `sync`, periodic owner commits and clean unmount establish durable
metadata state. Both fsync variants commit filesystem-wide in this adapter.
V1/v2 complete home checkpoint and log retirement synchronously. V3 acknowledges
a durable append record, retains committed images and materializes home blocks
under bounded log pressure. Ordinary buffered data is ordered before its
metadata publication; metadata recovery does not roll back existing file data.

The modern runtime enables SPDK NVMe `enable_flush=true` before JSON controller
attachment and rejects journal mount/format if JSON disables it. SPDK v26.09
otherwise defaults to suppressing NVMe FLUSH even on volatile-cache devices.
A controller without VWC need not execute hardware FLUSH for every logical
barrier. The storage backend must still satisfy the journal's persistence
contract; process exits alone do not qualify SSD power-cut behavior.

## Ownership and I/O ordering

Filesystem algorithms, caches and journal mutation run on one filesystem owner.
Fixed metadata, directories, indirect mappings, B+tree blocks and symlink
metadata are protected. Both synchronous block calls and dirty-cache batches
pass through the overlay. Journal raw callbacks bypass staging and use bounded
DMA buffers on the existing I/O reactor, avoiding recursive journaling.

Direct file data remains asynchronous on the existing queues. Overlay queries
prevent bypass of staged images or blocks still protected by the committed v3
overlay. Async completions retain the I/O thread as their single CQ producer.
Synchronous protected requests and all-protected dirty batches can copy directly
into the owner overlay; mixed regular-data batches retain the reactor path.
Cache references stay pinned during collection and the journal copies bytes
before callers can clean or release them.

Before commit, the runtime stops consuming frontend requests and drains submitted
AIO across all clients. The journal barrier returns `EBUSY` if submitted AIO
remains. Dirty buffers and the superblock are staged before commit. Data-only
overwrites also receive a persistence barrier when no metadata image changed.
Direct writes update inode mtime/ctime at the existing one-second resolution.

The fio runtime groups periodic commits at 100 ms by default. Draining submitted
I/O can extend that interval; it is not a hard durability deadline. Read-only
intervals do not force periodic commits. Direct API callbacks run through
`nvfio_execute()` on the same owner and choose bounded sync boundaries.
Independent integrations must provide equivalent serialization and AIO fencing.

## Bounded work and runtime options

Options are selected before process startup. Reference modes preserve the same
format and persistence ordering unless the option explicitly selects a format.

| Option | Default | Contract |
|---|---|---|
| `NVFUSE_JOURNAL_MS` | `100` | Periodic owner commit interval, 1–1000 ms. |
| `NVFUSE_JOURNAL_COMPARE_READS` | `0` | Complete protected images stage without home comparison reads; partial images preserve missing bytes. `1` restores comparisons. |
| `NVFUSE_JOURNAL_BATCH_IO` | `1` | Coalesce adjacent log/home blocks into requests of at most 128 KiB. Diagnostic hooks retain observable boundaries. |
| `NVFUSE_JOURNAL_LOG_JOIN` | `1` | Join a v1/v2 descriptor region with a profitable adjacent payload prefix; certificate order and checksum coverage stay unchanged. |
| `NVFUSE_JOURNAL_HOME_BATCH` | `1` | Submit scattered home writes in windows of at most 32 blocks / 128 KiB; drain every completion before buffer reuse or home flush. |
| `NVFUSE_JOURNAL_RAW_REUSE` | `1` | Reuse a persistent task for serialized raw journal requests; request and DMA lifetimes still end after completion. |
| `NVFUSE_JOURNAL_RAW_POLL` | `1` with raw reuse | Poll the raw task using the existing SPSC registration; `0` blocks. Explicit polling without reuse is rejected. Home tasks remain blocking. |
| `NVFUSE_JOURNAL_OWNER_COLLECT` | `1` | Avoid I/O-thread handoffs for synchronous memory-only protected staging; raw I/O and mixed batches keep their original ownership. |
| `NVFUSE_RENAME_INPLACE` | `1` | Preserve the directory-entry offset for same-parent non-directory rename to an absent name. Directory, replacement and cross-parent cases retain the general path. |
| `NVFUSE_JOURNAL_SPARSE_RESET` | `1` | Clear known active-overlay keys for small transactions, searching through holes created during whole-set removal. Dense transactions use full reset. |
| `NVFUSE_JOURNAL_CRC` | `auto` | Use SSE4.2 on supported x86 or CRC instructions on supported little-endian ARM; `software` selects the compatible reference. |
| `NVFUSE_JOURNAL_PROFILE` | `0` | Enable owner elapsed-time counters; leave disabled for ordinary throughput measurement. |

Scattered home writes can complete in any order, but every completion must drain
before the DMA buffer is reused, including after an individual failure. Home
flush and retirement follow only successful home publication. Persistent task
teardown rendezvous with I/O-thread queue draining before destroying locks.
Raw completion failures return EIO and make recovery mandatory.

CRC implementations retain the Castagnoli polynomial, initial/final complement,
byte order and coverage. Dispatch is immutable after process initialization.
Software and accelerated readers/writers share the same disk format; no format
upgrade is needed. Generic engines retain their own explicit policy defaults.

## Recovery, errors and support limits

The discovery anchor is validated before mutable filesystem metadata is read.
Replay completes before the legacy core mounts. Only then does the adapter
normalize the in-memory legacy signature/state; home writes preserve the journal
signature. A clean-state override must never bypass replay.

The default geometry admits at most 4075 distinct protected images per
transaction. Direct API batches must budget for every submitted protected block,
including unchanged complete images when comparisons are disabled. The adapter
does not add automatic credit admission, arbitrary-size transactions, concurrent
filesystem writers, portable user-TX/orphan machinery or complete errno unwinding
through legacy allocation/cache failures. Unrecoverable legacy paths remain
fail-fast. A failed commit requires recovery rather than continued mutation.

Optional application transactions and standalone checkpoint queues have their
own [API contract](user-transactions-design.md) and
[ownership design](checkpoint-concurrency-design.md). Those features must not be
inferred from this primary metadata adapter.

## Profiling semantics

`collect_ns` covers dirty-cache collection and superblock staging, split into
`dirty_ns` and `super_ns`. `profile_ns` follows `enum nvj_profile_stage`.

For v1/v2, stages cover descriptor allocation/encoding/CRC, ordered-data flush,
prepared controls, log writes, payload flush, commit certificates, home writes,
home flush and retirement. For v3, indices 0/1/3/4/6/7/8 cover record encoding,
ordered-data barrier, append write, append flush, home writes, home flush and
epoch retirement. V3 index 2 measures final active-index reset; index 5 measures
committed-overlay publication in memory, not a certificate disk write.

These counters measure owner elapsed time, including software and reactor/device
waiting. They are not NAND service time. Compare/partial-read and detailed CRC
counters are nested in their parent stages; do not add overlapping timers.
Inclusive sync time can contain bookkeeping not assigned to an individual stage.
Store run-specific logs, latency samples and comparisons under `build/test-results/`.

Implementation: [`src/nvfuse_primary_journal.c`](../src/nvfuse_primary_journal.c),
[`src/nvfuse_journal.c`](../src/nvfuse_journal.c),
[`src/nvfuse_journal_v3.inc`](../src/nvfuse_journal_v3.inc), and
[`include/nvfuse_primary_journal.h`](../include/nvfuse_primary_journal.h).
