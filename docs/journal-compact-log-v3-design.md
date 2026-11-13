# Experimental compact append log (journal v3)

Status: opt-in implementation validated with native persistence oracles, real
SPDK private-image recovery/fault tests and the explicitly approved physical
NVMe test namespace. The default new format remains v2. Open never upgrades a
v1/v2 volume. Primary formatting selects v3 only with
`NVFUSE_JOURNAL_FORMAT_VERSION=3`; the environment does not override an existing
anchor. Existing readers reject anchor version 3.

## Persistence contract

One serialized filesystem owner submits block writes and flushes. A successful
flush makes all previous completed writes persistent. Completed persistent bytes
are not subsequently lost or corrupted by another write to a disjoint extent.
Writes can tear at sector boundaries, persist out of order before flush, and
fail after partial persistence. CRC32C detects tears with its ordinary checksum
collision limitations; this is not protection against arbitrary malicious media
or undetected corruption. A physical power cut has not been qualified.

An acknowledged sync must survive. An interrupted sync may appear entirely or
not at all. Ordinary data is ordered before the metadata log when data was
written. Flush callbacks must actually implement this contract; an SPDK option
that suppresses flushes on volatile-cache devices does not satisfy it.

## On-disk layout

The discovery anchor keeps its existing fixed address, geometry, UUID and
checksum, with explicit version 3. Two checksummed epoch controls occupy logical
slots 1 and 2. Records append from slot 3 to the end of the reserved extent, using
the existing mapping that skips the discovery anchor. There is no in-epoch
record overwrite and no independently updated per-commit head pointer.

Each record has a 128-byte header: magic, epoch, volume UUID, consecutive sequence,
previous-record CRC, rounded block count, used byte count, entry count, protection
flags and CRC32C of the entire zero-padded record (with its CRC field zeroed).
Each entry names one home block, its metadata classification and a 32-bit bitmap,
followed by the selected 128-byte chunks. The first image of a home block in an
epoch is always complete. Later records contain absolute replacement chunks,
not XOR patches. Duplicate destinations within one record are invalid.

A complete, checksummed record represents a commit; a successful write followed
by a successful log flush is the acknowledgement boundary. The record can span
multiple bounded I/Os. Until the flush succeeds, success is never returned.
A partial record cannot authenticate its complete contents. Recovery accepts a
contiguous, UUID/epoch/sequence/previous-CRC-matched prefix and stops at an
incomplete append tail. A validly checksummed but semantically invalid record
fails mount. All accepted records are validated and merged before any home
write occurs. The initial full image ensures replay of patches never depends on
possibly partially checkpointed home bytes.

## Memory and visibility

The ordinary transaction overlay remains separate from a bounded committed
image overlay. Reads resolve active images, then the newest committed image,
then home. Writes to a block still present in either overlay cannot bypass it,
including blocks reused from metadata into data. Active discard clears only the
active overlay. The committed overlay stores complete latest images; records
store only changed chunks relative to those images. No memory allocation is
needed after log publication to retain the committed images.

The committed overlay and staging record buffer are bounded by journal geometry.
They currently add roughly 32 MiB for a 16 MiB journal, in addition to existing active
staging. Their allocated sizes are included by `nvj_memory_bytes`.

## Checkpoint and epoch reuse

When remaining record space or committed-image capacity cannot reserve the next
transaction, checkpoint the previous committed overlay. Active images remain
separate. Write each latest home image, complete all scatter writes and flush
home. Only then publish epoch E+1 in control 1 and flush, then in control 2 and
flush. Reuse old record bytes only after both controls have completed. Clear the
committed overlay and append from slot 3 under the new epoch.

If a crash occurs during home writes or before a valid new control exists,
recovery selects epoch E and replays its intact accepted prefix. If either new
control exists, its prior home flush completed, so selecting E+1 is safe. Both
controls must be replaced before record reuse, preventing a later torn control
from falling back to an older epoch whose records have been overwritten. Epoch
wrap is rejected. Failure stops mutation and requires recovery.

This is synchronous bounded reclamation under log pressure, with deferred home
writes across commits. It does not add a background checkpoint thread or claim
zero checkpoint tail latency. `nvj_checkpoint` explicitly materializes committed
records when no transaction is active. Reopen replays and durably advances to a
fresh epoch before admitting mutations, including after an incomplete tail.

## Compatibility and qualification

New-format selection remains v2 unless the primary formatter explicitly selects
v3. Existing v1/v2 volumes retain their original protocol. V3 readers validate
and recover an existing v3 volume without a format-selection environment flag;
older readers reject the v3 anchor. There is no implicit or in-place migration.

The implementation uses bounded synchronous reclamation. Its tail latency and
memory footprint depend on journal geometry and workload. Recovery models and
process-exit tests do not establish physical power-cut tolerance of a particular
controller. Retained-device recovery must reopen the original volume, never
replace it with a newly formatted fixture.

See the [primary journal adapter](spdk-primary-journaling.md) for owner/AIO
fencing, runtime options and the SPDK flush-policy guard. The compact engine is
implemented in [`nvfuse_journal_v3.inc`](../nvfuse_journal_v3.inc).
