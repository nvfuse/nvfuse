# Journaling and User TX Evaluation

Date: 2026-11-12

Implementation: working-tree changes based on `b5c1d85`; no release or production-readiness claim.

## Outcome

The standalone file and SPDK/NVMe backends now use automatic metadata redo transactions on newly formatted images. Optional user transactions provide metadata-only or metadata-plus-data groups. The exercised recovery paths reopen the original image without formatting it.

Correctness tests and local performance measurements support this synchronous reference implementation. They do not establish superiority over ext4/XFS or physical-device power-loss behavior. See the [metadata format and recovery design](metadata-journaling-design.md) and [user API contract](user-transactions-design.md).

## Executed validation

| Layer | Executed checks | Result |
| --- | --- | --- |
| Independent journal engine | 108 interruption/persistence cases at 35 write/flush boundaries; unflushed writes lost, first sector persisted, or complete write persisted; reverse-order flush persistence | Passed |
| Engine recovery | Eight interruption positions while replaying a committed log; reopen and verify complete protected home images | Passed |
| Engine validation | Corrupt committed payload refused before home writes; partial-write overlay; budget failure; poisoned-handle write rejection; data-only commit flush | Passed |
| File-image TX integration | DATA commit/abort; METADATA abort leaves regular bytes changed; invalid modes and budgets; nested/descriptor-busy begin; thread ownership; sync during TX; abort-only; large write beyond cache/log budget | Passed |
| Original-image process interruption | Ten explicit DATA-group windows and ten automatic rename windows; fresh-process verification twice per image | Passed |
| Independent disk accounting | Raw bitmap free-bit counts equal group/global counters; fixed metadata and journal remain allocated after the verified scenarios | Passed |
| Existing local smoke tests | Binary payload, unaligned writes, namespace mutations, repeated reopen, image/lock/format validation | Passed |
| Existing deterministic framework | Seeds 1 and 0xc0ffee; byte model, file/image format separation, metadata-only payload suppression, queue and core I/O failure behavior | Passed |
| AddressSanitizer / UndefinedBehaviorSanitizer | Existing local and deterministic tests, journal engine and TX integration | Passed on macOS ARM64 and Rocky Linux 9 ARM64 |
| Real SPDK path in QEMU | Existing NVMe filesystem stages and I/O counters; full VM restart for normal and payload-discard formats | Passed on Rocky Linux 9 ARM64 |
| NVMe user TX | Same functional TX oracle, plus payload/commit/checkpoint/retirement interruption and repeated original-volume reopen | Passed |
| QEMU I/O faults | Read EIO, write EIO, stall/resume, timeout, PCI device removal; original-volume payload verification after reboot before reformat | Passed |

The journal engine's persistence model separates volatile and durable storage; it stops the engine at a boundary with `-EIO`, discards volatile state, then reopens. This checks power-loss ordering independently of process-exit tests. A process exit alone is not a power-loss simulation because a host can persist dirty pages afterward.

The QEMU fault suite uses only its guarded, dedicated 2 GiB test NVMe. It preserves failed original disks before reinitialization and distinguishes error termination from successful recovery. VM tests are not physical SSD power-cut tests.

Failure evidence remains under `build/test-results/` locally. Representative reports are `journal-spdk-integration`, `journal-spdk-final`, `journal-spdk-reviewed`, and `journal-spdk-faults-r2`; timestamped `journal-tx-*` reports include per-process logs and mount times. Generated reports are ignored by git. A separate final native Linux ASan/UBSan run also passed all three suites; its host-side log is `/private/tmp/nvfuse-journal-linux-asan.log`. The first SPDK TX run failed its capacity assertion because a previously used device already contained the constant test bytes: identical-block elision legitimately avoided logging them. The test now uses fresh entropy; the failed run is retained in `journal-spdk-faults` and the rerun passed. An earlier cache-pressure timeout was corrected by draining bounded write chunks outside the cache lock.

## Measurement method

Host: Darwin ARM64 / macOS 27.0; Apple clang 21.0.0, `-O1 -g`. Backend: regular host file using pread/pwrite/fsync, warm process caches and fresh 128 MiB sparse images. These are host-file measurements, not raw NVMe throughput.

Each run creates 1024 files and writes 8192 bytes per file, then closes each descriptor. Explicit groups contain 1, 8 or 32 files. Each group ends with synchronous commit, checkpoint and retirement. Three fresh-image repetitions run per configuration. Formatting, initial mount and final unmount are outside measured throughput. Throughput includes operations and commit, so it includes the time that grouped operations wait for publication.

Automatic metadata mode wraps individual API calls. Explicit metadata mode has the same protected-data contract but allows several API calls to share a durable boundary. DATA mode is stronger: both regular-file bytes and metadata are protected. A clean-only legacy backend would provide a weaker guarantee and is not used as an equivalent baseline.

Raw samples are committed in [journal-reference-2026-11-12.json](benchmarks/journal-reference-2026-11-12.json). The table reports the median across three runs; latency columns are medians of each run's measured commit percentiles. A batch-32 run has only 32 commits, so its tail estimates are preliminary. Automatic API commits are included in throughput but are not separately timed by this driver.

## Results

| Mode / files per explicit TX | Files/s | Commit p50 ms | p95 ms | p99 ms | Flushes/run | Journal MiB/run | Protected home MiB/run |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Automatic metadata | 535 | — | — | — | 19,456 | 220.27 | 44.27 |
| Metadata / 1 | 954 | 0.985 | 1.098 | 1.197 | 9,216 | 120.14 | 32.14 |
| Metadata / 8 | 5,095 | 1.218 | 1.374 | 1.437 | 1,152 | 19.36 | 8.36 |
| Metadata / 32 | 11,553 | 2.034 | 2.269 | 2.280 | 288 | 8.05 | 5.30 |
| Data / 1 | 946 | 1.001 | 1.087 | 1.150 | 9,216 | 128.14 | 40.14 |
| Data / 8 | 4,062 | 1.652 | 1.825 | 1.909 | 1,152 | 27.36 | 16.36 |
| Data / 32 | 6,557 | 3.656 | 3.904 | 3.981 | 288 | 16.05 | 13.30 |

Metadata batches of 32 improve throughput by **12.1x** over one-file explicit metadata transactions in this workload. DATA batches of 32 improve throughput by **6.9x** over one-file DATA transactions. These improvements primarily amortize nine flushes and a fixed 16-block descriptor area per nonempty commit, and coalesce repeated changes to shared metadata. They are not ext4/XFS comparison results.

The 32-file metadata run writes 8.05 MiB to the journal, versus 120.14 MiB with one-file groups, and reduces flushes from 9216 to 288. DATA mode adds the 8 MiB regular payload to journal traffic. Protected home counters include checkpoint writes only; ordinary regular-data home traffic in metadata mode is excluded. Actual total device writes therefore exceed the metadata-mode home column. Counter values are successful logical submissions, not SSD NAND write amplification.

Observed abort time is approximately 5–6 ms for these small groups, dominated by cache teardown/remount. Peak staged blocks are 47 for metadata batches of 32 and 111 for DATA batches of 32 (188 KiB and 444 KiB), including the lifetime peak from initial mount. These are occupied images, not total allocated RAM; the engine preallocates about 16 MiB plus a volume-sized index.

Recorded file-image remounts after the tiny crash scenarios are on the order of 8–19 ms across these runs, including cache/IPC initialization and the new mount transaction. The timer does not isolate replay, the logs contain only small transaction groups, and the range is not a recovery scalability benchmark. Mount timing samples remain in the TX reports.

## Reproduction

```sh
make local-test local-framework local-journal-test
make local-sanitize
make -f Makefile.local journal-bench

# Full sanitizer coverage used during this evaluation:
UBSAN_OPTIONS=halt_on_error=1 make -f Makefile.local \
  LOCAL_BUILD=build/journal-asan \
  LOCAL_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  test journal-test framework-test

# Existing dedicated development VM only:
python3 tests/run_framework.py --profile integration --seeds 1
python3 tests/run_framework.py --profile integration-faults
```

The CI workflow now includes the journal suite in macOS and Rocky 9 container paths. This working session validates local executions, not a newly completed remote CI run.

## Limits and further evaluation

This is a single-handle, single-thread, synchronous reference implementation. It does not include multiprocess coordination, asynchronous checkpointing, orphan/deferred-delete records, persistent application outcome queries or an online legacy upgrade. Removing the last link to an open file, including rename replacement, is rejected with `-EBUSY`. Xattrs, fallocate, direct/AIO paths, sparse writes past EOF and truncate growth are excluded. Some core allocation/cache/partial-mount errors still fail fast rather than returning a recoverable errno.

Advance operation-credit reservation, strict RAM admission, sparse staging indexes and optimized CRC/descriptor serialization remain work items. The maximum log payload is 4075 blocks including metadata; this is not an arbitrary-size application transaction engine.

The accounting checks are narrower than a complete independent fsck. Exhaustive block ownership, every directory-index structure, all replacement/cross-directory rename combinations, arbitrary ENOSPC/ENOMEM cleanup, full-log recovery latency and long-running randomized transactional byte histories need additional coverage. Arbitrary media corruption, anchor redundancy, and real SSD/controller power-loss behavior have not been validated.

An ext4/XFS performance comparison needs matched Linux devices, sync policy, workload and equivalent application grouping/atomicity. No cross-filesystem throughput or reliability advantage is claimed here.
