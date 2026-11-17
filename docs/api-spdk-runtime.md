# Primary API/SPDK execution and buffered I/O

API + SPDK is the performance path. Linux FUSE remains a compatibility interface.
The primary runtime uses one serialized filesystem executor and an SPDK I/O
reactor. These changes preserve the B+tree, inode and block-group disk layouts,
the existing buffer cache, and journal v2/v3 recovery formats.

## Application entry points

Include `nvfuse_spdk_runtime.h` from `include/` in a primary-runtime application.
`make -f Makefile.spdk` includes `src/nvfuse_spdk_runtime.c` in
`build/spdk-primary/nvfuse.a`. Link this archive and the SPDK event/bdev
libraries, as shown by `tests/spdk/Makefile.tools` and the fio engine Makefile;
applications do not compile or include the runtime implementation. Private queue,
cache-policy, statistics and writeback helpers live in `src/spdk_runtime/`. The fio
example contains only the fio adapter. The cache-ordering test builds a separate
`NVFIO_TEST` runtime; these hooks are absent from the ordinary archive.

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

## External storage adapters

Application-specific adapters and their guides live under
[`integrations/`](../integrations/README.md), outside the filesystem sources.
Build the core with `make -f Makefile.spdk`, then link the adapter against
`build/spdk-primary/nvfuse.a` and headers from that same source revision. The
adapter build also needs the matching SPDK/DPDK headers, event/bdev modules and
system libraries. Follow `tests/spdk/Makefile.tools` for the link requirements;
copying a standalone archive alone is not an installed SDK. Rebuild adapters
when the core, SPDK or application reader ABI changes; a stable binary ABI is
not promised by this prerelease.

The core boundary is `include/nvfuse_spdk_runtime.h`. Namespace and metadata
callbacks additionally use the corresponding `nvfuse_api.h` and core types.
An adapter owns application buffers, file routing and its version compatibility;
the core owns filesystem state, metadata ordering and SPDK requests. Keep
application checksums, search scheduling and benchmark tuning outside the core.

For direct reads, allocate a client and requests, open an existing immutable
file with `nvfio_open(path, 0)`, use `nvfio_dma_alloc()` and submit reads with
`nvfio_submit(..., write=0)`. Buffers, offsets and lengths must be 4096-byte
aligned. Retain every request and DMA buffer until its completion has been
consumed with `nvfio_poll()` or `nvfio_wait()`, check each request's error, and
drain all accepted requests even after a partial submission failure. Close files
and free clients before the final `nvfio_stop()`. A reader must not format an existing volume;
provisioning and full-content verification precede serving.

`nvfio_poll()` remains nonblocking. A caller which would otherwise spin on an
empty completion queue can use `nvfio_wait(client, max, completed, timeout_us)`.
It polls first, then arms a Linux private futex and rechecks the queue before
sleeping. The first blocking call enables notifications on the filesystem owner;
subsequent waits reuse that state until the client is freed. Each client keeps
its calls serialized, including submit, poll, wait and free.

When only 4 KiB reads remain outstanding, an empty wait may coalesce completion
notifications up to `min(max, outstanding)`. Writes and larger requests keep
first-completion notification for streaming refill. Already-ready completions,
signals and timeouts can still return a partial batch. Use `max=1` when an empty
wait should request notification for the first completion. This changes neither
`nvfio_poll()` nor the requirement to retire every accepted request.

`timeout_us` accepts 0 through 1,000,000. Zero only polls. A zero return can also
mean a timeout, signal or spurious wakeup; keep waiting for every accepted request.
The timeout bounds the futex sleep, rather than providing a scheduling deadline
for the entire call. Positive returns contain ordinary completions, whose request
errors must still be checked. Request and DMA lifetimes and journal ordering
retain their usual rules. Poll-only applications can keep their existing loop.

The requested AIO lane count remains the client's capacity limit. The filesystem
owner initializes one lane and adds lanes when existing reservations cannot fit
a request. Initialized lanes live through the usual client drain and free; no
request can outlive its lane. Small graph batches usually use one lane, while
bulk I/O can grow to the requested limit. Later lane allocation can fail during
submission, so callers must keep the usual accepted-request error and drain rules.

After an empty AIO service pass, the filesystem owner yields outside its gate
while clients are live, allowing runnable application workers to use that CPU.
The filesystem and I/O owners remain separate; callbacks and periodic journal
work keep their existing ordering. A yield does not provide a latency deadline.
Include both runtime owners when assigning and reporting an application's CPU
budget.

One process owns a volume. A multi-process service needs an explicit storage
ownership design before several workers can use the same NVFUSE device.
Supporting one graph reader does not redirect an application's WAL, object
storage, metadata or index builder. Qualify lifecycle/reopen, data integrity,
recall where relevant, version compatibility and performance separately.
Applications linking NVFUSE must evaluate the combined distribution's GPLv2
requirements; see [NOTICE](../NOTICE) and [COPYING](../COPYING).

## Process signals and NUMA allocation

By default, SPDK installs its ordinary process signal handlers. For an embedding
application that owns process signals, set `NVFUSE_SPDK_EMBEDDED=1` before starting
the runtime. NVFUSE then sets SPDK's `disable_signal_handlers` option. The caller
remains responsible for quiescing requests and invoking `nvfio_stop()` in normal
thread context; this option does not make the runtime restartable.

IPC rings and mempools use the calling thread's NUMA socket by default.
`NVFUSE_IPC_SOCKET_ANY=1` passes `SPDK_ENV_SOCKET_ID_ANY` to those allocations so
an existing arena on another node can be used. It does not provision memory,
move cores or change the bdev. Prefer local allocation when available; remote
allocation may add latency. Both controls recognize exactly `1`; unset or other
values retain their defaults.

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

For eligible standalone v3 files, fsync collects the file's data, inode,
indirect blocks and only its new allocation dependencies. Shared metadata images
start from committed state; unrelated dirty files remain pending. Unsupported
dependencies use the full collector. During immutable commit or selected-data
completion waits, one bounded cache caller may make progress while the owner
retains the filesystem gate. Target inode aliases cannot append during data
completion; the owner waits for the caller before resuming cache access.
See [file-scoped fsync](file-scoped-fsync-design.md) for eligibility and ordering.

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
| `NVFUSE_JOURNAL_FILE_SYNC` | standalone: `1` | Dependency-complete file fsync subset on eligible v3 volumes; `0` restores the full collector |
| `NVFUSE_API_COMMIT_CACHE` | `1` | One bounded cache caller during immutable journal commit; requires eligible standalone v3 ownership |
| `NVFUSE_API_DATA_CACHE` | `1` | The same caller during scoped data-completion waits, excluding target inode aliases; requires commit-cache admission |
| `NVFUSE_API_CACHE_READ` | `1` | Caller loaded-page read |
| `NVFUSE_API_SIZE_EPOCH` | `1` | Descriptor size hints with mutation invalidation |
| `NVFUSE_API_CACHE_OWNER` | `1` | Omit nested locks only in the exclusive loaded copy |
| `NVFUSE_AIO_OWNER_MAPPING` | `1` | Resolve cached direct-read mappings under the exclusive runtime gate |
| `NVFUSE_API_BUFFERED_WB` | `1` | Speculative typed buffered-data writeback |
| `NVFUSE_API_BUFFERED_WB_CURSOR` | `1` | Reduce repeated speculative selection scans |
| `NVFUSE_API_BUFFERED_WB_WINDOW_PAGES` | `512` | Selection window, range 16–512 pages |
| `NVFUSE_BUFFERED_BC_DIRECT` | standalone: `1` | Regular data without transient buffer heads |
| `NVFUSE_BUFFERED_EXTENT_MAPPING` | standalone: `1` | Reuse bounded scalar mappings and copy eligible full-page cache misses |
| `NVFUSE_BUFFERED_FULL_COPY` | standalone: `1` | Skip redundant zeroing immediately before a complete mapped-page copy; partial and append paths retain initialization |
| `NVFUSE_BUFFERED_WRITE_HIT` | standalone: `1` | Update eligible loaded data pages |
| `NVFUSE_BUFFERED_FAST_APPEND` | standalone: `1` | Bounded single-page append fast case |
| `NVFUSE_BUFFERED_ALLOC_ACCOUNT` | standalone: `1` | Account through an already-held descriptor |
| `NVFUSE_BUFFERED_ALLOC_FUSED` | standalone: `1` | Fold the free-space precheck into allocation |
| `NVFUSE_BUFFERED_ALLOC_CACHED` | standalone: `1` | Use loaded authoritative allocation metadata |
| `NVFUSE_JOURNAL_DIRTY_IOV` | standalone: `1` | Coalesce eligible contiguous dirty-data requests |

These controls do not disable WAL, application sync, journaling or recovery.
The direct-read mapping path uses the existing inode and indirect-block cache;
it retains no mapping pointers between requests. Cold, referenced, locked or
flushing metadata, EOF, holes, fragmentation and indirect-boundary crossings
use the ordinary locked mapper. The runtime invokes the serialized AIO entry
point only while holding its exclusive filesystem gate in standalone mode;
generic AIO callers keep the ordinary entry point and locking.
Journal version remains an explicit formatting choice; use
`NVFUSE_JOURNAL_FORMAT_VERSION=3` when creating a v3 filesystem. Existing volumes
must be reopened with formatting disabled. See [the fio guide](../integrations/fio/README.md) for
build and workload commands.

## Verification

`make -f Makefile.local api-completion-test boundary-test` checks queue lifetime
and indirect allocation boundaries. The primary `api_cache_test` exercises
cached/cold reads, aliases, truncate, pending AIO and paused writeback exclusion.
The primary `serialized_mapping_test` compares all four mapping levels with
the ordinary mapper and checks cache-state, mutation, extent and EOF fallbacks.
`run_primary_journal.py` tests journal publication crash points, injected payload
I/O failure, inode/block reuse and partial appends on a disposable AIO image.
Run v2 and v3 separately. Buffered fault fixtures use a test-only hook under the
filesystem gate to pause periodic publication during the controlled failure
window; explicit fsync remains active and production timer limits are unchanged.

The read-only completion-wait test uses an existing immutable NVFUSE file and a
full POSIX copy with the same 4096-byte-aligned length. With the usual runtime
variables set and the volume exclusively owned, build and run from the root:

```sh
make -C tests/spdk -f Makefile.tools APP=completion_wait_test \
  SPDK_ROOT_DIR="$SPDK_SOURCE"
tests/spdk/completion_wait_test /demo.disk.index "$GRAPH_FILE"
```

It checks full payloads, exactly-once retirement, partial drains, zero/short waits,
signals, an accepted out-of-range read error and rejection of freeing a busy
client. It reopens the existing filesystem with formatting disabled.

Process exits and software fault injection are not physical
power-cut qualification. Performance results must identify the exact source,
application settings, CPU placement, cache state and durability policy.

Buffered writes on the standalone journaled primary also default to reusing
scalar contiguous mappings for at most 32 blocks within one write and its held
inode lock. No mapping pointer survives the call. Full-page cache misses may
copy into an eligible clean or unused cache entry directly; referenced,
flushing or unsupported entries retain the ordinary cache path. Cache policy,
journal staging and fsync ordering are unchanged. Set
`NVFUSE_BUFFERED_EXTENT_MAPPING=0` before startup to select the reference path.
`NVFUSE_WRITE_EXTENT` reports actual use. The primary-journal regression runner
covers fragmented overwrite, truncate/regrow and process exit immediately after
fsync, followed by two cold payload/EOF checks.

A proven full-page mapped cache miss resets cache metadata and copies all4096
bytes before publishing the page under the existing cache lock. Set
`NVFUSE_BUFFERED_FULL_COPY=0` to retain zero-before-copy. This does not change
partial writes, append initialization, metadata layout or fsync ordering.


Caller-cache reads retry an initially contended filesystem gate for approximately
50 microseconds before using the ordinary owner queue. `NVFUSE_API_CACHE_RETRY=0`
restores immediate fallback; the default is ON. A successful retry retains the
same exclusive gate and handle/live-AIO-client/data-failure/cache-state checks.
A timeout uses the original dispatch and drain path, so pending AIO cannot be
bypassed or indefinitely blocked. Shutdown retry counters count whole-process
contention attempts, acquisitions and timeouts, rather than individual DB phases.

`NVFUSE_API_CACHE_RETRY_US=10` restores the earlier retry budget; `50` is the
default. These are the two supported budgets. Longer retries can improve
compaction-heavy batch traffic while costing throughput for a contending
single-update writer; use the10us setting when that tradeoff is unfavorable.
Both settings preserve the same gate checks, bounded queued fallback and fsync
guarantees. The reported budget is printed at shutdown.
