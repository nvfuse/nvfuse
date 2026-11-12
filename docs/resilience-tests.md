# Filesystem resilience campaign

## Scope and status

This campaign extends the metadata boundary tests with six resilience areas.
It tests filesystem-internal consistency and ordinary filesystem APIs. User TX
atomicity is covered separately in `user-transactions-design.md` and the existing
transaction suite; ordinary operations must remain consistent without calling
`tx_begin`.

The baseline was committed as `1013fb4` (`Add exhaustive metadata boundary and
crash recovery tests`). The tests and directory fixes described here follow that
commit. Execution date: 2026-11-12; native host: macOS ARM64; VM: Rocky Linux 9
ARM64, SPDK and a disposable QEMU NVMe namespace.

All six areas were exercised within the limits below. This is **not** a claim
that all filesystem semantics or hardware failure modes are correct. The previously discovered parent-directory defect is now fixed and covered by
recovery and enumeration regressions.

| Area | Executed test | Observed result and boundary |
| --- | --- | --- |
| Long mixed workload and repeated crashes | One million choices in each payload mode; an additional 600,000 choices with 60 process crashes and a persistent independent oracle | Namespace, contents, remount and complete resource reclamation passed. Crash segments end with a rename; this does not crash every possible operation. |
| Stored-data durability | 20 MiB + 123 byte file, unaligned append, double-indirect truncate and cross-parent replacement rename; 9 cut points each | All 54 cuts (including directory creation and empty/populated directory moves) recovered the expected old/new contents on two fresh-process opens, with independent raw metadata validation. |
| Cache, space, memory and I/O pressure | Data exceeds the 16 MiB cache; journal allocation failures; near-full SPDK image with read/write EIO, stall/resume, timeout and hot removal | 15 journal ENOMEM points passed. Five NVMe fault scenarios passed recovery of the original images with less than 8 MiB free. Arbitrary core-cache allocation failures are not covered. |
| Adversarial directories | 256 genuinely colliding names, repeated create/delete, collision rename/replacement, colliding subdirectories, 115/116-byte names, 128 nested directories, five 2,048-file B+tree cycles | Passed after focused fixes. Parent traversal, empty/populated directory moves, legacy backfill and descendant-move rejection passed. |
| Supported concurrency | Eight foreign threads versus the owner, plus independent-process competing opens | 480,000 foreign calls rejected with `-EPERM` during 1,000 owner write/readbacks; 16 process opens rejected by the image lock. ThreadSanitizer passed. Parallel mutation on one handle is outside the current contract. |
| QEMU power loss substitute | SIGKILL the complete VM at two near-full metadata barriers and seven stored-data/directory-move barriers | Nine VM cuts passed repeated recovery and raw metadata checks. Physical-device power loss is explicitly deferred by the user. The host kernel and page cache remain powered. |

## Workload and crash oracles

The C++ model uses its own directory/file map, byte vectors, resource baseline,
and seeded PRNG. Mutations use the real C API; expected state is not derived
from the filesystem's resulting metadata. Soak mode checks affected objects on
each step, the full namespace/content model every 1,000 choices and after each
10,000-choice remount. The default short mode still verifies the full model on
every choice and remounts every 100 choices.

The repeated-crash runner preserves the PRNG state and independent expected
namespace/content model in a sidecar file. Each segment starts by verifying the
previous recovered state, continues the workload, then crashes a rename at a
selected journal boundary. It does not reformat between crash segments. The
final segment deletes all workload objects and compares free blocks/inodes to
the original baseline. Both stored-data and metadata-only modes are exercised.

The stored-data driver uses ordinary API calls, not user transactions. Its base
file crosses the single/double-indirect boundary and exceeds the cache size.
Every byte follows a deterministic offset-dependent pattern. The replacement
rename target has a different pattern. Recovery compares all bytes, file sizes,
EOF, source/target names and the unaffected file.

| Cut point | Required recovered state |
| --- | --- |
| `prepare-flush`, `payload-write`, `payload-flush` | Old state |
| `commit-flush`, `checkpoint-write`, `checkpoint-flush`, `retire-flush` | New state |
| Successful ordinary mutation, immediately before `fsync` | New state under the current eager implicit-commit implementation |
| Successful `fsync` | New state |

The before-fsync expectation follows NVFUSE's current implicit commit behavior;
it is not a general POSIX promise. Each crash image is opened twice in separate
processes to check replay and repeat-mount stability.

The raw checker independently checks ownership, bitmaps, free counters, indirect
mappings and directory/index consistency. Stored payload is checked by the
pattern/model oracle. `--skip-payload` is used for patterned images and for live
metadata-only crash workloads: recycled physical blocks may retain old metadata
bytes, while logical metadata-only regular-file reads correctly return zero.
Final fully reclaimed metadata-only images are checked without this exception.

## Focused fixes

The inode remains 4,096 bytes and block offsets, the journal format and pointer
widths are unchanged. The directory parent now uses one previously reserved
32-bit inode word, as detailed below.

1. A B+tree hash hit with a different filename previously asserted. Lookup now
   falls back to a name comparison scan. The linear scan now advances its entry
   iterator, scans all allocated slots and clears the released buffer pointer.
2. The two collision-count bits previously wrapped after enough same-hash names,
   allowing deletion to remove a still-needed index key. The count now saturates;
   deletion recounts surviving names when saturated. Recounting reuses already
   locked directory buffers and the caller's inode context. The last survivor
   retains linear lookup until its key is removed.
3. Public path resolution previously copied a complete parent path into a
   116-byte stack buffer and then into legacy 256-byte buffers. It now validates
   the complete path and all components before copying, then walks components
   with a filename-sized buffer. Paths over 4,096 bytes and components of 116
   bytes or more are rejected. Intermediate components must be directories.
4. Dot-name index lookups now return before allocating a B+tree master, avoiding
   an unreleased allocation/buffer on that path.

The collision oracle discovers a pair of distinct 12-byte ASCII strings with
identical production CRC32C results. Concatenation constructs 256 distinct
96-byte names with the same full 64-bit directory hash. No fake hash function
or reduced hash width is injected. It checks surviving names after permuted
removal, remounts after each file removal, repeats three file cycles, exercises
rename/replacement and deletes colliding subdirectories. Each directory mode
must restore the initial free-block and free-inode baseline.

## Parent-directory fix and enumeration verification

The old `nvfuse_make_first_directory()` wrote the current inode into both `.`
and `..`. Delayed allocation also left empty directories without a directory
block from which a parent could be read. A new `i_parent` field reuses the
32-bit reserved inode word at byte offset 60. Inode size, all other offsets,
maximum directory count and empty-directory block allocation are unchanged.
New directories store the parent at mkdir time; the root stores itself.

Lookup handles `.` and `..` independently of the B+tree and physical directory
block, including empty directories. Directory block materialization writes the
saved parent into `..`. Cross-parent rename updates the parent word and any
materialized `..` inside the same implicit metadata transaction as the namespace
move. Rename checks ancestry before changing entries, rejects moves into the
source/subtree and type-mismatched replacements, and handles same-name and
same-inode no-ops. Directory hardlinks and mutation of `.`/`..` are rejected.

For existing images, the reserved word may be zero and a materialized `..` may
point to self. An ordinary named lookup through the real parent's directory
entry provides an authoritative parent edge. It backfills the parent word and
repairs the materialized `..` through the operation's normal metadata
transaction. The tests deliberately create both legacy empty and materialized
directories, reopen, access through their parents and reopen again. This is
lazy backfill, not an offline repair of every inode at mount. A legacy inode
used directly by number without first resolving it through its parent may
still lack parent information; `..` then fails instead of inventing a parent.
Older binaries ignore the reserved word and retain their original `..` bug;
downgrades are not certified.

`readdir` no longer reads the same inode recursively while holding its lock for
`.`. It synthesizes both dot entries for blockless directories and releases the
current inode before reading a child's type. Root `getattr` now reports the real
inode, size and child count instead of a fixed stub. New regular files include
`S_IFREG` in their mode; `getattr` also infers it for legacy regular inodes that
stored permission bits alone.

The new shared tree oracle:

1. Recursively enumerates every reachable directory with `opendir`/`readdir`.
2. Verifies names are unique, inode numbers/types agree with `getattr`, and the
   reported entry count and end-of-directory are correct.
3. Checks `.` and `..` and resolves each directory's parent path.
4. Opens every listed regular file, reads all bytes and checks EOF/close.
5. Runs the test-specific content oracle as well; the mixed C++ oracle compares
   each directory's complete listing with its independent expected-name set.

Before API traversal in stored-data crash recovery, a separate cold metadata
walk checks the persisted parent word and physical `..` against namespace
edges. This prevents lazy lookup repair from concealing a non-atomic directory
move. The independent raw checker also validates nonzero parent fields against
incoming directory edges and materialized dot entries.

The parent regression now passes:

```sh
build/local/directory_resilience_test /tmp/nvfuse-parent.img parent
```

It covers empty and populated directories, root traversal, both kinds of
cross-parent moves, descendant-cycle rejection, same-name rename, missing-source
rename, forbidden dot deletion, legacy backfill, remount and complete resource
reclamation. Nine journal cut points are exercised for each of six operations:
append, truncate, replacement rename, populated directory move, empty directory
move and mkdir. Each of the 54 crash images is verified twice, including full
API enumeration/access and independent raw metadata checks.

## Fault and power-cut boundaries

The SPDK pressure helper fills a dedicated image with patterned regular-file
data, leaving at most 8 MiB free and forcing cache turnover. Sector-specific
QEMU blkdebug rules inject read/write EIO at an existing file payload block.
QMP throttles pending reads for resume/timeout tests and removes the dedicated
NVMe controller for hot removal. Resume must compare the complete original
payload. Other injected failures currently terminate with the expected fail-fast
SIGABRT; reopening the unchanged faulted image must replay the journal and
compare the original payload before any reformatting. These results do not imply
graceful online recovery or arbitrary core ENOMEM handling.

Journal ENOMEM tests intercept allocation functions only in the test translation
unit. They cover open allocations, both overlay-growth reallocations, the commit
descriptor and replay allocations. Failed precommit work is discarded, old home
blocks remain intact, and retry/replay/new transactions succeed.

VM cuts stop at a known barrier, SIGKILL only the fixture's QEMU PID and retain
an original qcow2 snapshot before recovery. Two cuts use a full metadata-only
image at `payload-flush` and `checkpoint-write`. Three append cuts use stored data at
`payload-flush`, `checkpoint-write` and `after-fsync`; four additional cuts move
empty/populated directories at `payload-flush` and `checkpoint-write`. Recovery
checks cold stored parents before full API enumeration and byte verification. Device serial, PCI IDs,
namespace size, mount state and IOMMU-group checks remain enabled. VM teardown
restores the fault configuration and stops the fixture.

This models loss of guest RAM and abrupt VM/device-process termination. It does
not model loss of the host page cache, SSD volatile caches, torn sectors or real
power-loss-protection behavior. Separate volatile/durable journal-engine tests
cover deterministic persistence ordering; physical-device qualification remains
pending.

## Commands

```sh
make local-resilience-test
make local-chaos-test

# ASan/UBSan; macOS does not support LeakSanitizer.
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  make -f Makefile.local LOCAL_BUILD=build/asan \
  LOCAL_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  resilience-test

# Persistent oracle across 10 segments, 3 seeds, both payload modes.
python3 tests/run_chaos.py --cycles 10 --segment-rounds 10000

# Long clean soak: payload mode 0 or 1, full check/remount intervals.
build/local/mixed_model_test /tmp/nvfuse-soak.img 314159265 1000000 0 1000 10000

python3 tests/run_framework.py --profile integration-powercuts --seeds 42 \
  --vm-dir build/vm/rocky9 --report-dir build/test-results/resilience-vm-powercuts
python3 tests/run_framework.py --profile integration-faults --near-full-faults \
  --seeds 42 --vm-dir build/vm/rocky9 \
  --report-dir build/test-results/resilience-vm-pressure-faults
```

Use fresh image and report paths. Python orchestration emits JSON, JUnit and
per-command logs, retaining images on failure. Native/CI targets include the
bounded resilience suite; the longer chaos campaign is an explicit target.
The test code is C++17 for the independent namespace/collision oracle, C for API
and journal allocation tests, and Python for crash/VM orchestration.

## Execution evidence

Paths below are local artifacts under `build/test-results/`, not versioned disk
images. Early failures remain available; the later passing run supersedes them.

| Report directory | Result |
| --- | --- |
| `soak-million-stored` | 1,000,000 choices, seed 314159265, stored payload; full cleanup and raw check passed |
| `soak-million-metadata` | 1,000,000 choices, seed 271828183, metadata-only; full cleanup and raw check passed |
| `chaos-20261112-195618-106069` | 600,000 choices, 60 process crashes, both modes; all replay/oracle/raw checks passed |
| `resilience-20261112-201247-440898` | Native: 120 passing command checks, including 27 stored-data crashes and expanded collision cases |
| `resilience-20261112-201311-144832` | ASan/UBSan: same 120 checks passed, including 256-name collisions and 16 competing process opens |
| `chaos-20261112-201330-433501` | ASan/UBSan: 14 additional crash segments, 14,000 choices and replay/oracle checks passed |
| `resilience-tsan` | Setup and supported-concurrency test passed under ThreadSanitizer |
| `resilience-vm-powercuts` | 27 command/event checks passed, including five whole-VM SIGKILL scenarios |
| `resilience-vm-pressure-faults` | Five near-full NVMe fault scenarios and original-image recovery passed |
| `stress-20261112-200813-216765` | Existing strict 3,000-choice mixed suite passed for three seeds and both modes after the production fixes |
| `stress-20261112-201430-362197` | Existing 70 near-full crash windows passed again after the production fixes |
| `journal-tx-20261112-201412-063520` | Existing explicit/implicit TX integration regression passed; standalone journal engine also passed 108 persistence cases |
| `resilience-known-defects` | Reproduced the original parent defect; retained initial collision/name/depth failure logs (historical, before the parent fix) |

The million-choice and full 60-cut runs preceded the directory safety patches.
The strict mixed, 70-cut boundary, 27-cut stored-data, sanitizer and VM campaigns
were run after the relevant production changes. Rocky Linux CI targets are
updated to include the native resilience suite; this does not claim a completed
CI run. The executed Linux VM evidence is the SPDK campaign above.


### Parent and full-tree follow-up

The following reports supersede the earlier parent-defect limitation. Initial
failed build/check iterations remain as evidence; final passing runs are listed
here. Native reports now contain 230 passing checks, including 54 crash cuts,
108 fresh-process content/tree verifications and 54 raw metadata checks.

| Report directory | Result |
| --- | --- |
| `parent-tree-complete` | Native 230-check suite, including cold stored-parent validation, full tree enumeration/readback, parent/legacy/move regressions |
| `resilience-20261112-203955-725153` | ASan/UBSan 230-check suite with cold stored-parent and full-tree verification |
| `parent-tree-chaos-asan` | 14 crash segments, 14,000 choices, exact independent directory listings and byte contents after recovery |
| `parent-tree-mixed` | Three seeds, both payload modes, 1,000 choices each with exact directory-list/content comparison |
| `parent-tree-boundary-crashes` | All 70 existing near-full crash windows with added full API tree enumeration/readback |
| `parent-tree-vm-faults` | Five near-full NVMe fault cases; recovered original images enumerate four directories and open/read all four listed files, including nested payloads and the pressure file |
| `parent-tree-vm-powercuts` | Nine whole-VM SIGKILL cases; metadata, append/fsync and empty/populated directory moves, with full tree/readback recovery |

The root-format initialization was also checked with native limit/framework
regressions. The complete native and sanitizer runs include same-name handling
and repair of a bad physical `..` when the parent word is already correct. QEMU fault tests were run
with the enumeration fix; subsequent QEMU cuts additionally use cold parent
checks before lookup. Physical-device power loss remains deferred.
