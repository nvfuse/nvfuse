# NVFUSE Metadata Journaling

Date: 2026-11-12

Status: Single-writer implementation with optional file-backend background checkpointing for the standalone file and SPDK/NVMe backends.

## Purpose and boundaries

The filesystem automatically protects its own metadata. Applications do not need a transaction API to recover a supported filesystem operation after an abrupt shutdown. The optional [user TX API](user-transactions-design.md) groups several operations and can additionally protect regular-file contents.

The implementation uses full-block physical redo, one writer and one outstanding transaction, with synchronous checkpointing by default. Optional background checkpointing retains exclusive ownership until retirement. Uncommitted metadata stays in an in-memory overlay. A durable commit authorizes replay; checkpointing copies protected images to their home addresses. A home address is the normal block location outside the journal.

Ordinary buffered operations use automatic metadata transactions. Existing regular-file overwrites can survive an aborted metadata transaction. DATA mode adds those bytes to the protected set. This distinction is independent of the requirement to preserve allocation and namespace consistency.

## Whole disk layout

Each block is 4 KiB; each group is 128 MiB. New standalone images reserve a filesystem-wide journal at the end of BG0: 16 MiB by default, optionally 32/64 MiB on new version-2 file images. The diagram shows the default. The formatter marks every journal block allocated and subtracts it from group and global free counts. Other group boundaries and fixed metadata offsets are unchanged.

```mermaid
flowchart TB
    subgraph volume["Whole volume: increasing physical addresses"]
        direction LR
        BG0["BG0: 128 MiB"] --> BG1["BG1: 128 MiB"] --> REST["Remaining groups"]
    end
    subgraph first["BG0 expanded"]
        direction LR
        SB["0<br/>Mutable superblock"] --> BD["1<br/>Group descriptor"] --> IB["2<br/>Inode bitmap"] --> DB["3<br/>Block bitmap"] --> IT["4..4099<br/>Inode table"] --> DATA["4100..28671<br/>Files and dynamic metadata"] --> J["28672..32767<br/>Journal"]
    end
    volume -. "BG0 expanded" .-> first
    style J fill:#fff0cc,stroke:#a66b00
```

Directory data, directory indexes, indirect blocks and symlink targets are metadata even though they occupy the data area. Fixed metadata blocks 0..4099 of each group are classified at mount; dynamically accessed metadata blocks are classified by the buffer-cache path. Classification is retained for the mount lifetime. Reused blocks may consequently receive extra logging, which is conservative.

Unlike the original proposal, **block 0 remains mutable** and is journaled. The stable discovery anchor is at physical block 28672, a fixed address independent of mutable superblock bytes. NVMe reopening obtains volume size from that anchor before replay, bounded by the actual device size. Reserved superblock slots in other groups are not implemented recovery backups.

## Format and journal layout

New normal images use signature `0x4a46564e`; the test-only format that discards regular payload uses `0x4a4d564e`. Legacy clean images retain their old signatures and do not gain historical recovery. Older binaries reject the new signatures. There is no in-place conversion tool.

The journal ABI uses explicit little-endian integers and CRC32C. Existing filesystem home structures retain their legacy native ABI; this does not establish cross-platform image interchange.

```mermaid
flowchart LR
    A["+0: stable anchor"] --> C["+1,+2: progress controls"] --> D["+3..+18: descriptors"] --> P["+19..+4093: full block images"] --> K["+4094,+4095: commit certificates"]
```

All offsets in this default-geometry figure are relative to journal start. The default extent is 4096 blocks; [larger journal geometry](large-transactions-design.md) defines the optional layouts. The default maximum protected payload is **4075 distinct blocks**, including metadata, or 16,691,200 bytes. File-data capacity is smaller because metadata consumes slots too.

| Structure | Fields and validation |
| --- | --- |
| Stable anchor | Magic, version 1 (legacy) or 2 (new), test-format flag, volume byte size, random 128-bit UUID, journal extent, CRC32C |
| Progress control, two copies | Magic, transaction ID, UUID, PREPARED or RETIRED, CRC32C |
| Descriptor area, 16/32/64 blocks | Magic, transaction ID, UUID, image count, DATA-mode flag; 16-byte entries with home block, metadata/data kind, payload CRC32C |
| Payload | Complete final 4 KiB images, one per distinct home block |
| Commit certificate, two copies | Magic, transaction ID, UUID, image count, CRC32C of the entire descriptor area, DATA-mode flag, certificate CRC32C |

The anchor is published and flushed last during formatting. UUID binding rejects stale records from previous formats. Transaction IDs increase without wraparound. Both control copies must be retired before a later transaction can overwrite the shared payload area.

Descriptor size follows journal geometry. CRC32C uses an immutable byte lookup table with the same reflected polynomial and initial/final XOR as the original bitwise implementation. Version-1 images remain readable; there is no automatic in-place upgrade. New version-2 images also implement [persistent cleanup](persistent-cleanup-design.md) and [bounded durable TX receipts](transaction-outcomes-design.md). The receipt page is the reserved inode-0 table slot; orphan/cleanup links use reserved inode words. Old version-1 binaries reject version 2.

## Runtime and persistence ordering

The public operation wrapper begins an automatic transaction, invokes nested implementation helpers, collects dirty buffers and the superblock, then commits. Inside an explicit user TX, the same wrapper collects images without publishing a commit. Internal flushes cannot commit the user group.

```mermaid
flowchart TD
    API["Supported public filesystem operation"] --> BEGIN["Automatic begin or join explicit TX"]
    BEGIN --> CACHE["Modify cache and allocation state"]
    CACHE --> TYPE{"Protected block?"}
    TYPE -->|"metadata, or all data in DATA mode"| O["Stage final image in overlay"]
    TYPE -->|"regular data in metadata mode"| HOME["Write ordinary data to home"]
    O --> COLLECT["Collect dirty cache and mutable superblock"]
    HOME --> COLLECT
    COLLECT --> USER{"Explicit TX active?"}
    USER -->|yes| RETURN["Return staged operation result"]
    USER -->|no| COMMIT["Durable commit and checkpoint"]
```

Reads consult the overlay after cache eviction. Repeated writes replace a staged image. Full-block images identical to home bytes need no payload slot. Large buffered writes drain bounded chunks outside the legacy cache lock while retaining the same transaction boundary.

```mermaid
sequenceDiagram
    participant FS as Filesystem
    participant LOG as Journal
    participant HOME as Home blocks
    FS->>HOME: Flush ordered regular-file data
    FS->>LOG: Write PREPARED A, flush then PREPARED B, flush
    FS->>LOG: Write descriptors and protected images
    FS->>LOG: Flush complete payload
    FS->>LOG: Write commit A, flush then commit B, flush
    FS->>HOME: Checkpoint all protected images
    FS->>HOME: Flush checkpoint
    FS->>LOG: Write RETIRED A, flush then RETIRED B, flush
    FS-->>FS: Release overlay and return success
```

A nonempty commit performs nine flushes. An empty protected set still flushes ordinary data, so `fsync` cannot silently skip a data-only overwrite. All persistence claims depend on the backend honoring its flush contract. On a regular host file, `fsync` tests the host filesystem boundary; it is not evidence of a physical SSD power-loss guarantee.

Two independently flushed commit certificates reduce sensitivity to a torn overwrite during commit publication. Either valid certificate for the selected prepared generation authorizes replay. Conflicting valid certificates are rejected. Checkpoint begins only after both publication flushes succeed.

## Recovery

Recovery runs before the legacy core mounts or consumes mutable allocation state. After recovery, the backend presents the recovered mounted state as clean to the legacy mount path, then journals the new mount state.

```mermaid
flowchart TD
    START["Open original image or device"] --> ANCHOR["Validate fixed anchor and device bounds"]
    ANCHOR --> CONTROL["Select highest valid control generation; RETIRED wins ties"]
    CONTROL --> STATE{"RETIRED?"}
    STATE -->|yes| MOUNT["Validate recovered superblock; mount"]
    STATE -->|no| CERT{"Matching valid commit certificate?"}
    CERT -->|no| DISCARD["Retire incomplete transaction; preserve home metadata"]
    CERT -->|yes| VERIFY["Validate entire descriptor and every image before home writes"]
    VERIFY --> VALID{"All targets, kinds, identities and checksums valid?"}
    VALID -->|no| REFUSE["Refuse mount; preserve evidence"]
    VALID -->|yes| REPLAY["Replay every protected image; flush home blocks"]
    REPLAY --> RETIRE["Retire both controls with flushes"]
    RETIRE --> MOUNT
    DISCARD --> MOUNT
```

Targets must be within the volume and outside the journal; duplicate destinations and invalid coverage kinds are rejected. Replay checks every image before writing any home image. A crash during replay leaves the committed log available for idempotent replay. A retired control is authoritative only after checkpoint durability, so partial retirement safely suppresses later redundant replay.

CRC32C detects corruption; it is not authentication. There is one stable anchor, no repair fallback for arbitrary anchor loss, and no claim of recovery from arbitrary corruption of both commit copies after checkpoint has started. Recovery is not an offline fsck, and does not repair pre-journal damage or validate every dynamic block's semantic ownership.

## Failure handling and support limits

The standalone implementation has one filesystem handle and owner thread, no
nested explicit user TX, and serialized filesystem execution. Version 2 supports
persistent open-unlink/replace, resumable truncate/delete, sparse growth, inline
string xattrs and fallocate. An aligned cache-bypass API joins the journal overlay;
producer threads can submit bounded futures to the owner. Historical AIO prepare
helpers remain unsupported. The payload-discard test format rejects DATA groups.

Mutation/storage failures make user TX abort-only; expected read-only lookup
misses do not. Budget overflow returns `-E2BIG` and requires abort. Overflow images
may finish the current legacy operation coherently but cannot be committed.
Sparse staging/classification indexes replace volume-proportional allocations,
and API boundaries preflight workspace. Exact operation-credit reservation and
strict bounded RAM admission for every legacy path remain unfinished.

Abort discards caches and remounts the committed image. A failed automatic operation uses the same rollback path. Metadata mode cannot undo ordinary data overwrites. O_SYNC uses the outer boundary; inside an explicit group it does not publish an independent commit. Explicit `sync`, `fsync` and `fdatasync` return `-EBUSY` while the group is active.

An I/O/flush failure during journal publication or checkpoint marks the handle RECOVERY_REQUIRED and rejects subsequent operations. Close can discard volatile caches and release an already failed handle, returning `-EIO`; reopen resolves the durable log. Legacy cache I/O failures and some legacy allocation/partial-mount failures still terminate the process because those core paths cannot unwind safely. This reference version does not promise complete recoverable errno behavior under all ENOSPC/ENOMEM conditions.

The historical multiprocess dataplane remains outside this implementation. The modern serialized primary reactor now has a separate [metadata-journal adapter](spdk-primary-journaling.md), with AIO fencing and its own explicitly stated coverage. Journal-format mounts must not bypass signature validation. The owner supports queued group commit, optional file-backend background checkpointing and bounded v2 outcome queries. Physical parallel SPDK execution, arbitrary-size/COW transactions and online journal resizing remain unimplemented.

## ext4, XFS and NVFUSE

These filesystems all depend on write-ahead ordering and persistent commit/recovery state. Their journals are not interchangeable formats.

```mermaid
flowchart TB
    subgraph EXT["ext4 / JBD2"]
        direction LR
        E1["Metadata buffers"] --> E2["Journal descriptors, block images, commit, revoke records"] --> E3["Checkpoint into ext4 home layout"]
    end
    subgraph XFS["XFS"]
        direction LR
        X1["Metadata log items; delayed logging / CIL"] --> X2["Typed log records and commit state; circular log"] --> X3["Recovery and metadata writeback; AIL / log tail"]
    end
    subgraph NV["NVFUSE reference version"]
        direction LR
        N1["Single writer; full-block overlay"] --> N2["Fixed BG0 slot, dual controls and certificates"] --> N3["Sync or optional file-worker checkpoint; retirement before reuse"]
    end
```

| Property | ext4 | XFS | NVFUSE reference implementation |
| --- | --- | --- | --- |
| Log location | Usually an internal journal inode; external journal supported | Internal log extent or external log device | Reserved 16/32/64 MiB extent in BG0 |
| Main representation | JBD2 descriptor and full-block metadata images, commit and revoke records | Typed metadata log items, regions and intent/done mechanisms | Full 4 KiB metadata images; optional regular data images |
| Ordinary file data | Commonly ordered mode; writeback and data-journal modes available | Regular data is not ordinarily journaled | Ordered regular writes; optional DATA user TX |
| Space reuse | Circular journal, sequence and revoke handling | Circular log, LSN/cycle and tail tracking | One reusable slot; dual retirement before reuse |
| Throughput machinery | Concurrent transactions, batching, asynchronous checkpoint | Delayed logging, CIL aggregation, scalable allocation groups | Owner-thread batching and optional file-backend background checkpoint |
| Recovery | Established JBD2 replay/revoke semantics | Established typed replay and deferred-operation recovery | Checksum-validated physical image replay |
| Arbitrary application multi-operation TX API | Not provided by ordinary POSIX interfaces | Not provided by ordinary POSIX interfaces | Explicit begin/commit/abort on the supported embedded handle |
| Maturity | Broad production deployment | Broad production deployment | New reference implementation with limited coverage |

NVFUSE has **no demonstrated advantage in metadata recovery robustness or general filesystem throughput over ext4/XFS**. Its concrete application benefit is an embedded transaction boundary that can atomically publish several filesystem operations and optionally their file bytes. Batching reduces this implementation's repeated journal overhead. That benefit is separate from the mandatory automatic metadata protection and is not a direct ext4/XFS speed comparison.

For authoritative format and architecture details, see the Linux kernel documentation for [ext4 journal/JBD2](https://docs.kernel.org/filesystems/ext4/journal.html), [XFS logging](https://docs.kernel.org/filesystems/xfs/xfs-logging.html), and [XFS delayed logging](https://docs.kernel.org/filesystems/xfs/xfs-delayed-logging-design.html).

## Source references

- [Journal engine and recovery](../nvfuse_journal.c), [disk constants and backend API](../include/nvfuse_journal.h).
- [Backend, format and mount ordering](../nvfuse_file.c), [cache metadata classification](../nvfuse_buffer_cache.c).
- [Automatic/user transaction boundaries](../nvfuse_tx.c), [public API wrappers](../nvfuse_tx_wrappers.c).
- [Independent persistence model](../tests/journal_engine_test.c), [filesystem TX oracle](../tests/transaction_test.c), [crash runner](../tests/run_transactions.py).


### Directory parent metadata

The parent-directory fix stores `i_parent` in the formerly reserved 32-bit inode
word at byte offset 60. The 4 KiB inode size, block offsets and journal record
format remain unchanged. Mkdir stores the parent even when the directory has no
data block; directory moves journal the namespace edge, parent word and any
materialized `..` together. Legacy directory parents are backfilled when reached
through their real parent's named entry.

## Current extensions

See [checkpoint ownership](checkpoint-concurrency-design.md) for queues and asynchronous completion, and [larger transactions](large-transactions-design.md) for resource and COW limits. FS metadata transactions remain automatic and independent of user TX API use.

The layouts and single-slot ordering above describe v1/v2. The optional primary
[compact v3 log](journal-compact-log-v3-design.md) uses append records and deferred
home materialization; new formats remain v2 by default.
