# File-scoped primary fsync

Status: integrated and software-qualified on the performance branch. Final
default measurements exceed XFS for the focused LevelDB short-batch target.
`NVFUSE_JOURNAL_FILE_SYNC` defaults ON for the standalone primary. Eligibility still requires v3; existing volume versions and
the default format version are unchanged. Set it to `0` for the original full
collector. No on-disk format change.

The existing primary `fsync(fd)` commits all dirty buffers. A WAL sync can
therefore flush background SST data and hold the shared filesystem gate while
that unrelated work completes. Merely filtering data pages is unsafe: committing
an unrelated inode's new size/mappings without its ordered data can expose
uninitialized bytes after recovery.

The new path collects a bounded, dependency-complete subset for an existing
regular file. It writes the target's dirty data, inode and changed indirect
blocks. Shared data-allocation bitmaps, block-group descriptors and the
superblock start from the last **committed** journal/home images. The collector
adds only the target's new allocations and adjusts the corresponding free/used
counts. Other files' inode sizes, mappings, directory/index updates and allocation
bits stay pending in the normal dirty cache. Shared cached metadata and live
accounting are never replaced with the filtered images.

The journal's ordered-data flush still precedes log publication and the log
flush still precedes successful fsync acknowledgement. Completed unrelated
writeback may also become durable at that barrier; unrelated dirty cache pages
are not forced out. Both fsync and fdatasync use the same conservative subset.
Global sync, periodic commits, pressure/eviction fallback and unmount retain the
full collector.

The initial path requires standalone primary ownership, v3, no outstanding AIO,
and an empty active journal overlay. An overlay populated by cache eviction may
contain another file's metadata and therefore forces the full path. Planning
rechecks this condition after acquiring the target inode. The caller retains
the filesystem gate throughout. The API runtime can lend one bounded cache
execution slot during the two waiting intervals described below; ordinary
filesystem callbacks and lifecycle operations remain excluded.

Inode creation, block freeing/reuse and truncation require a successful full
commit before another subset. In particular, the current inode allocator resets
`i_version`; it is insufficient as an ABA/lifetime proof by itself. Target
lifetime/namespace attributes must match the committed inode, its size must not
shrink, and old nonzero mappings must remain unchanged. Each new mapping must
name an unallocated bit in the committed bitmap. Unsupported attributes,
aliases, missing new indirect nodes and bounded-work limits fall back before
any subset write. Allocation failure during planning also uses the full path;
I/O/commit failures retain the existing recovery-required contract.

Limits are 64 selected metadata images, 16 affected block groups, 256 scanned
indirect nodes and 2048 new allocations per subset. Larger operations remain
supported through the original full commit. V1/v2 volumes are never upgraded.

Required validation includes target-only ACK followed by immediate process
exit; deferred-file size/content and allocation counts after repeated original
image reopen; direct/single/double-indirect boundaries; successive fsyncs of
different files; creation/truncate/populated-overlay fallbacks; failed/interrupted
log publication; and independent metadata ownership/bitmap checking after replay.
Existing generic journal persistence oracles remain necessary: a process exit
alone does not model loss of volatile host/device caches.

Throughput comparisons must use the same binary with the setting OFF/ON,
identical sync/WAL/workload/CPU conditions, fresh XFS references, phase-specific
actual-use counters and balanced/reversed trials. Do not substitute historical
private commit-cache results for this candidate's measured performance.

## Bounded cache work during storage waits

`NVFUSE_API_COMMIT_CACHE` defaults ON. During explicit API sync, once ordered
data has drained and metadata staging is finished, the journal owns immutable
commit images. Until that commit returns, one application caller at a time may
read already loaded data (at most 64 KiB) or perform a strict EOF cached append
(at most 8 KiB). The append uses only an existing idle inode context, known
metadata classification, loaded allocation metadata and an already available
pointer path. It cannot issue I/O, evict dirty cache pages, call the journal,
create a pointer path, run a generic callback or perform a synchronous write.
Its changed cache pages belong to the next transaction.

`NVFUSE_API_DATA_CACHE` also defaults ON and requires commit-cache admission.
For a scoped data batch, submission and journal staging finish before opening
this second interval. While the owner only consumes completions, the same
bounded operations may run for other files. Every descriptor alias of the
sync target is excluded from append, preserving the selected DMA buffers and
planned metadata. This interval closes and waits for its admitted caller before
the owner releases jobs or changes FLUSHING pages back to CLEAN. The global
data collector does not open this additional interval.

Admission uses a single atomic state; closing first rejects new entrants and
then waits for the current caller. The owner does not access filesystem cache
state until that caller leaves. This does not add filesystem owners or change
block-group ownership, journal barriers, WAL policy or application sync flags.
No AIO clients may exist during admission. Legacy journal versions, unsupported
process models and the inode-context dirty-buffer build policy keep the normal
serialized path. Set both runtime controls to `0` to disable admission;
`NVFUSE_JOURNAL_FILE_SYNC=0` independently restores full fsync collection.

Other filesystems also scope fsync to a file's data and required metadata, but
shared log transactions and device barriers can persist additional changes.
The contract does not require that only one file reach storage. NVFUSE retains
the full collector whenever it cannot prove a complete dependency subset.

Final qualification and both filesystem-order performance results are recorded in [the performance report](performance-xfs-parity-20261114.md). The broader all-profile XFS goal remains open; physical power-cut qualification was not performed.
