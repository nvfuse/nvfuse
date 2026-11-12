# Roadmap stages 1–5: implementation and validation

Date: 2026-11-12. Changes based on `8ed7b482ecaff7e2a237eceded3c28faf6d84ea8`
on `local-dev-backend`, PR #5. Physical-device qualification (stage 6) is deferred.
This report supersedes the implementation exclusions in the earlier
[reference evaluation](journaling-evaluation.md).

## Implemented behavior

| Stage | Implemented and exercised | Remaining boundary |
| --- | --- | --- |
| 1. Continuous validation | macOS/Rocky PR tests include roadmap regressions; weekly soak/chaos and opt-in QEMU workflows preserve artifacts | Scheduled activation requires merge; dedicated QEMU runner/variables need provisioning |
| 2. Resource/error handling | Sparse staging/classification indexes; formula-based fixed metadata; preflight workspace; recoverable journal allocation and direct-payload errors; no read-only flush | Strict credits and recoverable unwind of every legacy cache/B+tree/mount error remain unfinished |
| 3. File semantics | Persistent open-unlink/replace; resumable unlink/truncate; sparse growth/zero-fill; inline string xattrs; fallocate | No binary POSIX xattr API or automatic v1 upgrade |
| 4. Performance/concurrency | CRC32C table; optional background file-image checkpoint; multi-producer futures, owner-thread group commit; aligned filesystem-cache bypass | FS execution remains serialized; no background checkpoint on SPDK, host O_DIRECT promise or physical parallel qpair execution |
| 5. User TX | Expected lookup misses preserve active TX; optional larger redo extents; durable keys and outcome query | Finite DATA budget, 127 retained receipts, no COW or unlimited streaming groups |

Ordinary API calls still maintain filesystem metadata consistency without user
transactions. User metadata/DATA transactions add optional grouping; they are
not required for namespace recovery.

New images use journal anchor v2. New readers accept v1 with its original
geometry and restrictions; old readers reject v2. No existing image is silently
upgraded. See [cleanup format](persistent-cleanup-design.md),
[journal geometry](large-transactions-design.md),
[checkpoint and queue ownership](checkpoint-concurrency-design.md), and
[durable outcomes](transaction-outcomes-design.md).

## Executed evidence

Generated reports below are under `build/test-results/` and intentionally ignored
by git. Reports contain commands, exit status, per-process logs and raw-checker
output. Counts are runner checks, not a count of independent assertions.
The committed [case manifest](benchmarks/roadmap-validation-2026-11-12.json)
lists the principal rerun cases and their aggregate results.

| Coverage | Evidence | Result |
| --- | --- | --- |
| New roadmap suite | `roadmap-20261112-213345-899303` | 274 checks passed, including 63 crash scenarios, two fresh-process reopens per scenario, and independent raw accounting |
| ASan/UBSan roadmap suite | `roadmap-20261112-213347-408892` | Same 274 checks passed |
| Existing resilience suite | `resilience-20261112-213224-379994` | 230 checks passed |
| ASan/UBSan existing resilience | `resilience-20261112-212743-364256` | 230 checks passed |
| Existing deterministic framework | `20261112-211902-198376` | 34 checks passed |
| Existing journal/TX integration | `journal-tx-20261112-212832-013895` | 63 checks passed; engine persistence tests also passed |
| Exhaustion and file/inode/directory limits | `limits-20261112-211913-894359` | 105 checks passed |
| Full address/journal boundaries | `boundaries-20261112-212350-432058` | Both boundary executables passed; full uint32 inode sweep also passed in stress suite |
| Mixed model and near-full interruption | `stress-20261112-212234-608475` | 544 checks passed |
| ASan/UBSan full regression then rebuilt affected suites | `build/roadmap-asan-final.log`, `build/roadmap-asan-latest.log` | Baseline suites passed; stale-linked queue run failed, rebuilt roadmap/resilience/journal/boundaries passed |
| Queue ThreadSanitizer | `build/roadmap-tsan-queue-final.log` | 3072 producer jobs passed across three commit modes, mixed direct/buffered I/O, abort/cancel/drain and remount |
| Checkpoint ThreadSanitizer | `build/roadmap-tsan-journal-final.log` | Gated worker proves early durable response; 32 slot reuses, worker EIO/replay and close drain passed |
| QEMU NVMe faults | `roadmap-vm-faults` | 35 checks passed across read/write EIO, stall/resume, timeout and device removal, including near-full preparation |
| QEMU whole-VM termination | `roadmap-vm-powercuts` | 47 checks passed across nine kill scenarios |
| Further mixed soak | `roadmap-soak` | Two million choices: one million per normal/payload-discard format, cold verification and raw accounting passed |
| Further process-crash continuation | `roadmap-chaos` | 60 crashes, 192 checks passed across three seeds and both image formats |

The QEMU tests reopen the original image and verify complete directory
enumeration, parent/path accessibility, file contents and EOF. Recovery is not
merely a successful mount. QEMU termination leaves the host page cache powered;
the separate volatile/durable journal model checks lost/reordered writes.

The new suite covers open descriptors after unlink and rename replacement,
rollback of descriptors, bounded cleanup interruptions, partial truncate tails,
sparse direct/single/double/triple-indirect boundaries, maximum file offset,
shrink followed by growth/write in a user group, xattr bounds/full area/abort,
fallocate preservation/zero-fill, direct-payload injected read/write EIO with
same-handle reuse, v1 compatibility and larger image geometry. Invalid cleanup
list pointers, negative/overflowing sizes and impossible cursors are refused
before reclamation; opening a healthy image in that same process verifies unwind.

Journal tests additionally exercise allocation failures, classification preflight,
CRC compatibility, unsupported versions, worker-creation synchronous fallback,
maximum-volume sparse RAM below 20 MiB with the default journal, and complete
32/64 MiB payload capacity including physical anchor skipping. Overflow,
last-payload failure and post-commit checkpoint failure are followed by reopen.
Large DATA crash cases atomically overwrite 24 MiB plus metadata. Outcome tests
cover lost responses, retained-key retries, all 127 entries and explicit reuse.

Failed iterations are retained. The partial-tail regression was fixed by
protecting tail zeroing as structural metadata even in metadata mode. The
same-inode queue regression was fixed by refreshing a descriptor's cached size
before buffered writes. The first final sanitizer queue failure linked an older
library; rebuilding the changed API object and rerunning passed. These failures are not counted as passing runs.

## Performance

Raw samples: [roadmap-2026-11-12.json](benchmarks/roadmap-2026-11-12.json).
Each configuration has three fresh 128 MiB images, 1024 files and 8192 payload
bytes per file on macOS ARM64, clang `-O1 -g`, host pread/pwrite/fsync. Baseline
uses the preserved pre-change executable; later rows use the new executable on
the same host. This is a host-file microbenchmark, not raw SSD or ext4/XFS data.
Values below are medians of samples (including each sample's commit p95).

| Mode / group size | Baseline files/s | New sync files/s | Async files/s | Sync p95 ms | Async p95 ms | Logical write ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Automatic metadata | 539 | 1007 | 957 | — | — | 34.068 |
| Metadata / 1 | 932 | 1705 | 1667 | 0.620 | 0.448 | 20.034 |
| Metadata / 8 | 4780 | 7667 | 7439 | 0.829 | 0.577 | 4.465 |
| Metadata / 32 | 10665 | 17434 | 17337 | 1.431 | 0.956 | 2.668 |
| DATA / 1 | 895 | 1639 | 1614 | 0.674 | 0.426 | 21.034 |
| DATA / 8 | 3822 | 6499 | 6650 | 1.085 | 0.745 | 5.465 |
| DATA / 32 | 5762 | 10215 | 10469 | 2.032 | 1.419 | 3.668 |

CRC/table and reference-path changes improve measured synchronous throughput
about 1.6–1.9x here. This is an aggregate before/after result, not isolated proof
of a single optimization. Grouping still amortizes nine durability flushes per
nonempty group: metadata groups 1/8/32 use 9216/1152/288 flushes per run.
Skipping empty read-only barriers reduces automatic-mode flushes from 19456 to
18432. Total logical write ratio includes journal, protected checkpoint and
ordinary direct-home bytes divided by application bytes, not SSD NAND writes.
Baseline did not count ordinary home bytes, so a baseline total ratio is omitted.

Async commit lowers response latency but its throughput gain is inconsistent;
it remains optional and off by default. It starts a worker per group, and the
next FS operation joins before reusing the single slot. The benchmark queries
stats after commit, draining the last worker, so total throughput includes
checkpoint completion. A 32-file group has only 32 latency samples; tail estimates
and small throughput differences should not be generalized.

## Reproduction and residual work

```sh
make local-test local-framework local-journal-test local-limit-test \
  local-boundary-test local-stress-test local-resilience-test local-roadmap-test
make -f Makefile.local LOCAL_BUILD=build/roadmap-asan \
  LOCAL_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  test framework-test journal-test limit-test boundary-suite stress-test \
  resilience-test roadmap-test
make -f Makefile.local build/local/journal_bench
python3 tests/run_journal_bench.py build/local/journal_bench
python3 tests/run_journal_bench.py build/local/journal_bench --async
python3 tests/run_framework.py --profile integration-faults --near-full-faults \
  --seeds 42 --vm-dir build/vm/rocky9
python3 tests/run_framework.py --profile integration-powercuts \
  --seeds 42 --vm-dir build/vm/rocky9
```

Stage 2 is partial: `file_transfer` still aborts for legacy cache-transfer errors,
and some core allocation/mount paths cannot unwind. Preflight plus recoverable
direct I/O is not a claim that arbitrary ENOMEM/EIO keeps every handle usable.
Finishing that work requires auditing return propagation and lock/cache ownership
through the original core. Physical parallel SPDK queues need per-request DMA
ownership and completion drain. COW needs persistent allocation quarantine and
reclamation; the current implementation evaluates that design and implements
bounded full-data redo instead. These are explicit remaining tasks, not passes.

The baseline PR CI run `43211689897` passed macOS and Rocky Linux. Roadmap commit `214abc2` started CI run `43219342096`; both jobs were still running
at report time. This is separate from the passing local evidence. Periodic schedules are definitions until merged;
the opt-in dedicated QEMU runner is not provisioned by this change.
