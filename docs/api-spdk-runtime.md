# Primary API/SPDK execution and buffered I/O

API + SPDK is the performance path. Linux FUSE remains a compatibility interface.
The primary runtime uses one serialized filesystem executor and an SPDK I/O
reactor. These changes preserve the B+tree, inode and block-group disk layouts,
the existing buffer cache, and journal v2/v3 recovery formats.

## Application entry points

Include `examples/fio_plugin/modern/runtime.h` in a primary-runtime application.
`nvfio_execute()` runs a callback on the filesystem owner. For core filesystem
callbacks that do not call SPDK thread/channel APIs, use
`nvfio_execute_serialized()`: the caller can execute under the filesystem gate,
avoiding an owner round trip. It falls back to the owner while AIO clients exist.
Callbacks must not reenter synchronous runtime wrappers.

Use `nvfio_pread()`, `nvfio_pwrite()` and `nvfio_fsync()` for buffered positional
I/O. Negative results are errors; writes can be short. The typed functions reject
O_DIRECT descriptors; asynchronous direct I/O uses `nvfio_submit()` and the fio
engine. Complete all calls before the last `nvfio_stop()`. One runtime mounts one
explicit bdev. The primary build defaults to 1024 descriptor slots, including
three reserved slots. Compile the core and every linked application with the
same `MAX_OPEN_FILE` if overriding that limit.

## Serialization and cache ownership

A short bounded spin precedes the ordinary filesystem mutex. Loaded cached
reads may copy on the caller while holding that gate. Size epochs invalidate
descriptor size hints after mutations, including aliases and truncate. Cache
misses, referenced/flushing/unloaded pages and live AIO clients retain the
ordinary owner path. The standalone primary has no independent cache flush
worker; only the loaded-page copy can omit nested cache locks. Core mutators
retain their existing locking and references.

API calls use an MPSC pending queue and a completion state that distinguishes
spinning, parked and completed callers. The owner publishes the result before
completion; a caller cannot release its request before that publication.

## Buffered writeback and durability

Typed nonsynchronous writes can submit prior full dirty regular-data pages after
256 KiB per descriptor, checking every 64 KiB. Selection is bounded to 512 pages
and checks current inode/cache identity, references, state and journal overlay
protection. Existing cache references pin DMA buffers until every completion is
consumed. A per-descriptor cursor reduces rescanning; it only suppresses
speculative work and never suppresses the ordinary fsync flush.

Overlapping writes, synchronous writes, generic callbacks, lifecycle operations,
AIO allocation and metadata publication drain pending writeback. Internal cache
eviction may stage protected metadata but cannot publish its transaction.
`nvfio_fsync()` drains data before the existing ordered-data and journal boundary.
Unmount drains and fences the reactor before freeing tasks and buffers. No
additional payload cache or persistent pointer mapping is introduced.

The write path retains the inode across a request, batches only allocations
inside its actual payload, and reuses loaded authoritative mapping/bitmap pages.
Cold, mapped, pinned and boundary cases fall back. Newly allocated partial pages
are zeroed. Freed indirect blocks lose their physical cache aliases before reuse.
Dirty data can share a bounded scatter/gather request; protected metadata still
uses the journal. Flush dispatch is skipped only when SPDK explicitly reports no
volatile write cache, after preceding writes complete. File and volatile-cache
bdevs retain real flushes and the journal retains its barrier hooks.

## Controls and backout

The standalone primary enables the validated buffered fast paths by default.
Explicit `0` disables an individual path; invalid boolean values are rejected.

| Variable | Default | Purpose |
| --- | --- | --- |
| `NVFUSE_API_GATE_SPIN_US` | `10` | Bounded mutex spin, range 0–1000 microseconds |
| `NVFUSE_API_CACHE_READ` | `1` | Caller loaded-page read |
| `NVFUSE_API_SIZE_EPOCH` | `1` | Descriptor size hints with mutation invalidation |
| `NVFUSE_API_CACHE_OWNER` | `1` | Omit nested locks only in the exclusive loaded copy |
| `NVFUSE_API_BUFFERED_WB` | `1` | Speculative typed buffered-data writeback |
| `NVFUSE_API_BUFFERED_WB_CURSOR` | `1` | Reduce repeated speculative selection scans |
| `NVFUSE_API_BUFFERED_WB_WINDOW_PAGES` | `512` | Selection window, range 16–512 pages |
| `NVFUSE_BUFFERED_BC_DIRECT` | standalone: `1` | Regular data without transient buffer heads |
| `NVFUSE_BUFFERED_WRITE_HIT` | standalone: `1` | Update eligible loaded data pages |
| `NVFUSE_BUFFERED_FAST_APPEND` | standalone: `1` | Bounded single-page append fast case |
| `NVFUSE_BUFFERED_ALLOC_ACCOUNT` | standalone: `1` | Account through an already-held descriptor |
| `NVFUSE_BUFFERED_ALLOC_FUSED` | standalone: `1` | Fold the free-space precheck into allocation |
| `NVFUSE_BUFFERED_ALLOC_CACHED` | standalone: `1` | Use loaded authoritative allocation metadata |
| `NVFUSE_JOURNAL_DIRTY_IOV` | standalone: `1` | Coalesce eligible contiguous dirty-data requests |

These controls do not disable WAL, application sync, journaling or recovery.
Journal version remains an explicit formatting choice; use
`NVFUSE_JOURNAL_FORMAT_VERSION=3` when creating a v3 filesystem. Existing volumes
must be reopened with formatting disabled. See [the fio guide](spdk-fio.md) for
build and workload commands.

## Verification

`make -f Makefile.local api-completion-test boundary-test` checks queue lifetime
and indirect allocation boundaries. The primary `api_cache_test` exercises
cached/cold reads, aliases, truncate, pending AIO and paused writeback exclusion.
`run_primary_journal.py` tests journal publication crash points, injected payload
I/O failure, inode/block reuse and partial appends on a disposable AIO image.
Run v2 and v3 separately. Buffered fault fixtures use a test-only hook under the
filesystem gate to pause periodic publication during the controlled failure
window; explicit fsync remains active and production timer limits are unchanged.
Process exits and software fault injection are not physical
power-cut qualification. Performance results must identify the exact source,
application settings, CPU placement, cache state and durability policy.
