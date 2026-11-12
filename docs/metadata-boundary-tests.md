# Metadata Address-Space and Reclamation Tests

Date: 2026-11-12

## Objective

Exercise the ends of representable metadata structures, rather than treating exhaustion of a small image as the filesystem's maximum. Test allocate, map, partially truncate, delete, verify complete reclamation, then allocate again. Regular-file payload storage is unnecessary for these structural checks.

C++17 implements the virtual block store and independent allocation oracle. A thin C adapter calls the production indirect mapping/truncation implementation. Python orchestrates subprocesses and retains logs. Smaller metadata-only filesystem images separately exercise real bitmaps, directory indexes, journals and remount behavior.

## Boundary matrix

| ID | Target | Required boundary and oracle |
| --- | --- | --- |
| I1 | Logical block paths | First/last direct, single, double and triple slots; transitions; negative and first invalid logical block; exact leaf-contiguity count |
| I2 | Complete indirect tree | Allocate every reachable logical block with the real 1024-way fanout; validate every contiguous mapping span; delete all data and indirect blocks; verify zero outstanding ownership |
| I3 | Partial truncate | Aligned and unaligned cuts on both sides of each transition, final leaf, sparse branches; retain exactly the expected data and ancestors |
| I4 | Reuse | Allocate after deletion, including the former final address; reject duplicate allocation/free and references to released blocks |
| A1 | Physical block IDs | Values around 0x7fffffff and 0x80000000, and final 0xffffffff; never wrap allocation into reserved block zero |
| A2 | Out-of-range requests | Invalid logical addresses and integer extrema must fail without allocating or touching pointers outside the inode |
| B1 | Allocation bitmap | Bit, machine-word, bitmap-page and group boundaries; free/reallocate; counters equal bit counts |
| N1 | Inode space | Last inode in a group, next group, last available inode; exhaustion/reuse; delayed directory-index inode requirement |
| D1 | Directory index | Fill nodes through splits/root growth; remove entries through merges/root shrink; compare every surviving name and reclaim owned blocks |
| J1 | Journal capacity | Capacity minus one, exact capacity, capacity plus one; repeated-image coalescing; no partial commit on overflow |
| J2 | Journal reuse/recovery | Last payload and descriptor positions, commit/checkpoint/retirement interruption, repeated replay, reuse after recovery |

A 32-bit physical block number does not mean a single file can hold 2^32 blocks. The current inode has 11 direct pointers and one root for each indirect depth, with 1024 pointers per 4 KiB block. Its mapping capacity is 11 + 1024 + 1024^2 + 1024^3 = 1,074,791,435 data blocks. Full file bytes equal 4,402,345,717,760; the buffered API currently treats MAX_FILE_SIZE as exclusive.

## Test layers and limits

The C++ virtual block store replaces block storage and resource accounting, not the production mapping or truncate algorithms. It omits data bytes and compresses metadata pages, retaining real 32-bit pointers and 1024-way geometry. Synthetic physical addresses test signed/unsigned transitions without formatting a multi-terabyte volume.

A model result is not proof that mkfs, real inode allocation, real bitmap I/O, journaling or a physical device supports a maximum-size volume. These need separate integration evidence. In particular, testing a physical block-ID end does not exhaust every possible inode ID or the entire real volume bitmap.

The metadata-only image suite remains supplementary: 128/256 MiB images exhaust file count, sharded file count, directory count and logical file space, perform ENOSPC rollback/reuse, check raw ownership/accounting, and remount twice. Each workload lives below `/arena`: after exhaustion, every child and the arena itself are deleted, free block/inode counters must exactly match the initial baseline, and the same image is filled and deleted again. Refill must reach the identical capacity. Its observed counts are image-specific, not format-wide maxima. An independent checker rejects a deliberately damaged bitmap.

## Change policy

Fix local bounds, signedness, pointer initialization and reclamation defects with focused regressions. If correctness requires changing the disk format, pointer width, journal architecture or fundamental metadata ownership, report the design issue before implementing that change.

## Execution and evidence

Implementation status, commands and measured outcomes are recorded below as tests are executed. Planned matrix entries must not be presented as passed solely because another layer passes.

### Commands

```sh
make local-boundary-test
make local-limit-test
make local-stress-test
make -f Makefile.local test framework-test journal-test
UBSAN_OPTIONS=halt_on_error=1 make -f Makefile.local LOCAL_BUILD=build/asan \
  LOCAL_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  test framework-test journal-test limit-test boundary-suite stress-test
```

`boundary-test` runs only path/sparse checks; `boundary-full-test` runs the full
indirect tree twice. `boundary-suite` runs both the full tree and the C++ journal
capacity suite with subprocess timeouts and JSON/JUnit/log retention. The normal
macOS and Rocky 9 CI jobs run both suites; the Rocky job also runs sanitizers.
These CI additions have not themselves been executed remotely in this change.

### Verified coverage

| Matrix | Executed coverage | Remaining limitation |
| --- | --- | --- |
| I1–I4 | Real production path decoding, all indirect slots, 14 aligned/unaligned sparse truncate cuts, complete deletion and two full reuse cycles | Storage/cache and allocation accounting are supplied by the C++ model |
| A1–A2 | Signed physical transition, physical `0xffffffff`, negative paths, logical capacity and INT32_MAX rejection; invalid create leaves allocation unchanged | Does not establish formatter/volume support for all 2^32 block IDs |
| B1, N1 | Real 128/256 MiB exhaustion and rollback; additionally every inode bitmap slot of all 131,071 possible journal-volume groups allocated/freed twice through production routines | Full-volume bitmap storage and namespace are not materialized in the adapter sweep |
| D1 | Real directory splits, all-name lookup, delete/refill; synthetic count rejection at 0x7fffffff, 0x80000000 and 0xffffffff | The configured per-directory count guard exceeds all available inodes in a supported journal volume; no full namespace at that count is claimed. Adversarial hash collisions and independent internal B+tree parsing remain unexecuted |
| J1 | 4,074 / 4,075 / 4,076 unique images, exact capacity commit, overflow rejected without home mutation, 100 writes coalesced under budget 1 | Fixed current journal geometry only |
| J2 | At exact capacity: crash after final descriptor, final payload, second commit certificate, final checkpoint; all home bytes checked after two opens and slot reuse | Backend models fully persistent writes; sector tearing/retirement interruption are covered separately by the existing smaller C engine suite |

The full indirect test allocates **1,074,791,435 data blocks plus 1,050,627
metadata blocks**, verifies all mapping spans, and returns to zero outstanding
ownership on each cycle. Its logical file capacity is about 4.40 TB without
allocating that payload on the host. An initial standalone run took 6.432 / 6.484
seconds per cycle; the sanitizer run took about 23 seconds per cycle. These are
model execution times, not filesystem throughput measurements.

### Defects found and fixes

- New indirect branches incorrectly asserted that their first populated slot
  was zero. Last-address and sparse allocation require nonzero slots; use the
  already validated offset when initializing each new page.
- Partial truncate released a shared buffer before the subsequent shared-chain
  loop used/released it. Mark it dirty at detachment and leave final release to
  that loop. The ownership oracle detected the invalid later release.
- Resource exhaustion asserted or lost the allocator error. Standalone journal
  mode now returns ENOSPC, frees partial reservations, and frees the mapping
  workspace on failure. Directory creation checks the delayed index inode too.
- Buffered write chunking could hide an invalid original end offset. Validate
  the original range against the exclusive MAX_FILE_SIZE before chunking.

No disk-format, pointer-width or journal-architecture change was needed.

### Reclamation interpretation

The B+tree node allocator retains free nodes inside its index file while the
owning directory exists. Deleting root children alone therefore need not return
all blocks to the volume bitmap (17 blocks were retained in the first probe).
The independent checker still accounts for these blocks through the live index
inode. The final workload deletes its own parent directory as well, requiring
complete reclamation of that index and exact restoration of the volume counters.
This avoids misclassifying reusable, owned index capacity as a leaked block.

The first probes and failing images remain under
`build/test-results/limits-20261112-190420-894912` and
`build/test-results/limits-first`; assertion/ownership diagnostics from the
initial model runs are in `/private/tmp/nvfuse-boundary-{first,second,third}.log`.
Generated evidence is local and is not committed.

### Current image results (with the disposable arena)

| Image | Empty files | Files across 32 parents | Empty directories | Largest file at block exhaustion (bytes) |
| --- | ---: | ---: | ---: | ---: |
| 128 MiB | 4,085 | 4,021 | 4,085 | 100,519,936 |
| 256 MiB | 8,181 | 8,117 | 8,181 | 217,829,376 |

All eight workloads passed two complete allocate/delete cycles, two initial
fresh-process verification opens, independent raw checks after each phase,
and deliberate bitmap-corruption rejection. Counts include the cost of the
arena and its index, so they differ from the earlier root-only probes.

Normal-run evidence:

- `build/test-results/limits-20261112-190529-193530`: all eight image cases and complete reclamation/refill.
- `build/test-results/boundaries-20261112-190646-076174`: full indirect tree twice and journal capacity/final-slot crash cases.
- `build/test-results/20261112-190637-074007`: existing local framework regression (SPDK integration explicitly skipped).
- `build/test-results/journal-tx-20261112-190642-869039`: transaction modes and 20 explicit/implicit crash windows; existing engine suite also passed 108 crash/persistence cases and 8 replay interruptions.

All evidence above was collected on macOS ARM64 with Clang. SPDK evidence from the subsequent extension is recorded separately below; these local results alone do not establish device behavior.

Sanitizer evidence (`-fsanitize=address,undefined`, UBSan halt-on-error):

- `build/test-results/limits-20261112-190657-350594`: all 105 image-suite checks passed, including complete cleanup/refill.
- `build/test-results/20261112-190643-557213`: 33 framework checks passed; optional SPDK integration skipped.
- `build/test-results/journal-tx-20261112-190651-317249`: transaction integration passed; engine crash/replay matrix also passed.
- `build/test-results/boundaries-20261112-190947-897183`: final full indirect tree (two cycles) and journal capacity/recovery suites passed with sanitizers.

## Mixed workloads, inode maxima and near-full crash extension

### Inode and namespace limits

With this build's 4 KiB inode and 128 MiB group, each group contains 4,096
inode slots. The journal formatter requires complete 32,768-block groups and
at most UINT32_MAX blocks, so the largest accepted geometry is **131,071 groups**,
**536,866,816 inode slots**, and final inode number **536,866,815**.
Eight slots are statically reserved; the formatted/mounted root index occupies
one additional inode. A root-only namespace can therefore have at most
536,866,807 additional empty-file/empty-directory inodes before accounting for
any additional parent/index inodes. This is a geometry-derived upper bound,
not a claim that all those names have been created in one mounted volume.

`inode_space_test --full` executes the actual `nvfuse_scan_free_ibitmap`,
`nvfuse_release_ibitmap`, `nvfuse_dec_free_inodes` and `nvfuse_inc_free_inodes`
functions from `nvfuse_core.c`. Only buffer storage is substituted. It visits
**every slot of every group**, retains all groups simultaneously at exhaustion,
checks every scan returns zero, frees in reverse order, checks every bit/counter,
and repeats once. The final version performs **1,073,733,616 allocations and
corresponding frees** in the main two cycles, excluding the eight static reserved
slots, plus two extra final-inode reuse probes. All 536,866,808 allocatable inode
bits remain set simultaneously before reclamation; the global free counter must
reach exactly zero and then return exactly to capacity.
A separate last-slot-to-first-slot probe covers bitmap hint wraparound.

The final adapter retains every real-size bitmap (about 512 MiB total) and group
descriptor in memory, with the actual production global counter. It does not run
full inode-cache initialization, global group selection or directory insertion
for hundreds of millions of files. The real image suite covers those paths at
128/256 MiB scale. A full namespace on the approximately 16 TiB format maximum
has **not** been materialized.

`MAX_FILES_PER_DIR` is 0x7fffffff, which exceeds the total inode supply in any
supported journal volume. Actual namespaces hit inode exhaustion first.
Synthetic root counts at 0x7fffffff, 0x80000000 and UINT32_MAX exercise the real
create/mkdir rejection guards without writing fabricated metadata to disk.
Both calls must return ENOSPC, preserve counts/pointers and release references;
normal operations, remount and complete reclamation must then succeed.

An additional regression releases/reuses the final inode **before** issuing a
failed creation. A failed implicit operation reloads the allocator hint and
could otherwise mask a one-past-volume hint defect. This check runs on each
real-image count workload, as well as on the synthetic maximum inode space.

### Independent mixed-operation oracle

`mixed_model_test.cpp` maintains its own namespace and byte arrays. Three
seeds (1, 42 and 0xc0ffee), 3,000 choices each, run in stored-payload and
metadata-only modes: **18,000 operation choices** total. Every operation kind
must actually execute at least once. Operations include mkdir, empty-file
creation, unlink, cross-parent rename (including replacement), overlapping
write/append, partial truncate, empty rmdir and rejection of nonempty rmdir.
After each choice it checks every candidate name, surviving file size/content,
and directory child count against the model. Every 100 choices it remounts;
finally it deletes the entire arena and requires exact free-counter restoration
before and after another remount. An independent raw check follows each run.

The C adapter uses public mutation boundaries. Read-only oracle inspections
call the production implementation functions directly to avoid an implicit
transaction rollback/remount for every expected ENOENT. This keeps the model
independent without changing product transaction behavior.

### Near-full ordinary-operation crash matrix

The file-image suite covers seven mutations: file create/delete at inode
exhaustion, directory create/delete at inode exhaustion, and large-file
extend/delete/partial-truncate at block exhaustion. A reserved inode/block is
released first where creation needs it. No explicit user transaction is used.

Each mutation is interrupted at ten hook positions: both prepare barriers,
first payload write, payload barrier, both commit-certificate barriers, first
checkpoint write, checkpoint barrier, and both retirement barriers: **70 crash
windows**. `_exit(86)` skips close/unmount. For each window, two fresh-process
recoveries must produce the expected complete old/new namespace and size.

The raw checker independently walks allocation bitmaps, inode mappings and
directory entries, rejects duplicate/unowned allocated blocks, and checks free
counters. It also hashes semantic inode/pointer/name/allocation state (excluding
timestamps and journal generations) and compares against independently prepared
clean old/new executions. Each recovered image then executes another
allocation/deallocation and passes another raw check. This adds **post-replay
reuse** to the earlier crash matrix.

The crash model is process termination with the host OS still running. It does
not simulate a host power outage or arbitrary torn sectors; the existing C
journal engine suite provides the separate volatile/durable/sector model.

### Additional fixes and test-development failures

The directory count guards used equality alone and returned while retaining
the parent inode reference. They now reject count values at or above the limit,
release the reference and return ENOSPC. No format or architectural change was
required. The synthetic count is restored before any transaction can persist it.

The first sanitizer adapter run reported a null mock handle: a production
counter helper evaluates the preallocation field before its standalone-mode
check. The adapter now provides a valid handle; this was a fixture defect.
The first SPDK C++ build lacked the C++ runtime at link time; its test makefile
now explicitly links libstdc++. Both failed attempts are retained separately.

### Additional commands

```sh
make local-stress-test
python3 tests/run_stress.py --sections mixed inodes
python3 tests/run_stress.py --sections crash
python3 tests/run_framework.py --profile integration --seeds 42 --vm-dir build/vm/rocky9
python3 tests/run_framework.py --profile integration-powercuts --seeds 42 --vm-dir build/vm/rocky9
```

The normal macOS and Rocky CI jobs include `stress-test`; Rocky also runs it
under ASan/UBSan. Remote CI has not been executed during this local task.

### Extension results

- `build/test-results/stress-20261112-192539-107128`: complete normal suite; all 70 crash windows include repeat recovery, semantic-state comparison and post-recovery allocation/free.
- `build/test-results/stress-20261112-192620-315001`: complete ASan/UBSan suite with the same recovery/reuse checks.
- `build/test-results/stress-20261112-192833-893323`: final normal mixed/inode rerun, including all three synthetic directory count values.
- `build/test-results/stress-20261112-192835-068498`: final ASan/UBSan mixed/inode rerun, including all three synthetic directory count values.

### Actual SPDK/NVMe and VM termination results

`build/test-results/boundary-spdk-validation-2` passed on the dedicated 2 GiB
QEMU NVMe through the real SPDK transport:

- The identical C++ oracle, seed 42, 3,000 choices in each payload mode (6,000 total).
- All eight 128/256 MiB exhaustion cases, twice filled/verified/deleted, with exact initial free-counter restoration and equal refill capacity.
- Twelve near-full process-crash windows: file creation, directory deletion and large-file partial truncate, each at payload flush, commit flush, checkpoint write and retirement flush. Every case recovered twice and allocated/freed again.
- Existing user-transaction functional/crash checks, all 16 existing filesystem stages for seed 42, and persisted stored/metadata-only data across separate VM boots.

`build/test-results/boundary-vm-powercuts` passed two stronger VM termination
cases at block exhaustion. The workload pauses at a journal hook, and the host
sends SIGKILL to the dedicated QEMU PID, bypassing guest shutdown and QEMU
cleanup. Before commit (payload flush), recovery restored the complete old file;
after the first checkpoint write, it restored the complete new partial-truncate
state. Both recovered twice, and the independent host checker inspected actual
NVMe bytes with QEMU stopped. The original faulted disks are retained as sparse
qcow2 snapshots in the report.

This is VM loss with the **host kernel/page cache still powered**. It does not
establish survival of a physical host power failure, torn device sectors or
firmware that violates flush guarantees. Those persistence variants remain
covered at engine-model level, not by this emulated device run.

A reused NVMe can contain stale data in blocks whose payload is omitted.
`metadata_check --skip-payload` therefore checks ownership, names, mappings and
accounting without asserting physical payload bytes are zero. Logical zero
reads are separately checked through the filesystem; the native freshly-created
sparse-image checks still require physical zero payload.

### Final inode wrap correction

The maximum-space regression reproduced an out-of-range group request after
allocating the final inode: `sb_last_allocated_ino` becomes one past the volume,
and a later free/reallocate used that value as an unnormalized group index.
`nvfuse_find_free_inode` now wraps the group selection by `sb_bg_num` before
reading a descriptor. The saved failure is
`/private/tmp/nvfuse-inode-wrap-failure.log` (assertion before any invalid group
read). A two-group fixture and the maximum-group simultaneous allocation test
both cover the regression. No on-disk format change was needed.

- `build/test-results/stress-20261112-193656-889025`: final normal simultaneous-bitmap and mixed oracle checks after the wrap fix.
- `build/test-results/stress-20261112-193659-824047`: final ASan/UBSan full stress suite after the wrap fix, including all 70 crash windows.


### Final executed checks after the inode wrap fix

- `build/test-results/limits-20261112-193856-703109`: all 105 normal image checks, including final-inode reuse before the first failed creation.
- `build/test-results/limits-20261112-194009-075736`: all 105 ASan/UBSan image checks with that same regression.
- `build/test-results/boundary-final-inode-linux`: Rocky Linux full simultaneous bitmap sweep in normal and ASan/UBSan builds; final-inode reuse through actual NVMe for files, directories and sharded files; independent raw metadata check after VM shutdown.
- `build/test-results/20261112-194119-135613`, `journal-tx-20261112-194125-499277`, and `boundaries-20261112-194129-294764`: final normal framework, transaction and full indirect/journal regression.
- `build/test-results/20261112-194309-538942`, `journal-tx-20261112-194316-241502`, and `boundaries-20261112-194321-866370`: final ASan/UBSan regression of the same suites.

The dedicated VM was shut down after all checks. No physical NVMe device was
used. None of these results claims materialization of a half-billion-name
namespace or loss of host power. Those remain distinct from the tested full
inode-bitmap space and the actual 128/256 MiB filesystem workloads.
