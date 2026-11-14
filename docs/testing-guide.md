# NVFUSE validation guide

Choose a test by the path it actually exercises. Passing a raw NVMe test does
not establish NVFUSE correctness; passing an image test does not establish
primary API/SPDK performance. Complete the [user guide](user-guide.md) first.

## Test selection

| Question | Entry point | What it exercises |
| --- | --- | --- |
| Does the native image API work? | `make local-test` | Payload comparison, overwrite, namespace mutation and fresh-process reopen |
| More sizes/patterns and seeded operations? | `make local-framework` | Normal and metadata-only image modes, deterministic content model |
| Journal ordering and replay? | `make local-journal-test` | Persistence model plus original-image reopen/recovery |
| Boundary geometry and resource exhaustion? | `make local-boundary-test`, `make local-limit-test` | Production indirect mapper, journal boundaries, standalone inode/block reuse |
| Mixed operations and long sequences? | `make local-stress-test`, `make local-chaos-test` | Seeded model workloads and standalone crash/exhaustion scenarios |
| Transaction/checkpoint/queue regressions? | `make local-resilience-test`, `make local-roadmap-test` | Standalone ownership, journal/queue and API extensions |
| Memory/undefined-behavior errors? | `make local-sanitize` | ASan/UBSan native suites in a separate build tree |
| Basic mounted behavior? | `make fuse-adapter-test`, `make fuse-test` | Linux FUSE callbacks and actual image mounts |
| Broader mounted behavior/reference workload? | `make fuse-extended-test` | Extended FUSE tests including an independently built fsx |
| API/FUSE backend combinations? | `make backend-regression` | Three rounds of the four Linux image/kernel combinations, with sanitizers |
| All six combinations? | `make backend-regression-qemu` | Dedicated QEMU fixture, including the synchronous SPDK transport |
| Primary API/SPDK data path? | Modern fio engine and `tests/spdk/run_primary_fio.py` | Actual asynchronous filesystem requests and CRC verification |

Standalone exhaustion tests do not imply complete primary-path ENOSPC handling.
Functional regression targets are not benchmark results. Read individual runner
options before expanding a suite, especially if it formats or reserves devices.

## Native first checks

```sh
make local-test
make local-framework
make local-journal-test
```

The local smoke test uses a disposable 256 MiB image. The framework uses seeds
`1` and `0xc0ffee` by default and supports a new report directory:

```sh
python3 tests/run_framework.py --seeds 1,42 --keep-images \
  --report-dir build/test-results/my-framework-run
```

Reports include commands, logs, JSON and JUnit where provided by the runner.
Successful temporary images may be removed; failed images are retained for
inspection. Recover by reopening the retained image, not creating a replacement.

`nvfuse_file_open_mode(..., metadata_only=1)` is a **test-only** format. It keeps
allocation/mapping metadata while discarding regular-file payload writes and
returning zero-filled buffered reads. It has a different signature from a normal
image. Use normal images for corruption detection, FUSE, and application examples.

Boundary tests execute the production indirect mapper/truncator at its full
geometry without storing all file bytes. They do not mean that a full device
was physically filled. The physical-backend and normal-image tests provide
separate payload evidence. Similarly, simulated persistence or VM termination
does not establish the power-loss behavior of a physical controller.

For a Rocky Linux 9 container using an existing Docker/Colima installation:

```sh
sh scripts/test-rocky9.sh
```

Override `NVFUSE_DOCKER_CONTEXT` for a different Docker context. The script
builds its toolchain image, mounts source read-only, retains reports on the host,
and runs tests without network access. Architecture follows the container host;
Apple Silicon's ARM64 container is not an x86_64 test. The checked-in
[local CI workflow](../.github/workflows/local-tests.yml) runs native macOS and
Rocky Linux container jobs; it does not qualify a physical NVMe automatically.

## Linux FUSE and backend regression

With libfuse3, `/dev/fuse`, and `fusermount3` available:

```sh
make fuse
make fuse-adapter-test
make fuse-test
```

The mounted runner fails rather than silently treating an inaccessible FUSE
setup as a pass. For additional reference tooling, inspect and run
[`setup-fuse-reference-tools.sh`](../scripts/setup-fuse-reference-tools.sh), then
provide your independently built fsx path:

```sh
make fuse-extended-test FSX="$HOME/src/fs-reference/xfstests/ltp/fsx"
```

Use `make backend-regression` inside Linux for API/FUSE with image and kernel
backends. Its kernel tests use disposable loop-backed fixtures. The all-backend
QEMU runner adds standalone synchronous SPDK coverage. Neither establishes the
primary asynchronous reactor's throughput or its separate error contract.

For Mac users with an **already prepared** dedicated Linux QEMU environment:

```sh
python3 scripts/test-fuse-qemu.py
python3 scripts/test-fuse-qemu.py --sanitize
make backend-regression-qemu
```

The VM disk images, pinned-key SSH helper, credentials and SPDK checkout are
local prerequisites, not distributed in this repository. The first two commands
use ordinary guest image files; the all-backend regression also uses the
fixture-only QEMU NVMe. Install guest dependencies explicitly first. See
[Linux FUSE integration](linux-fuse-integration.md) for runner behavior.

## SPDK QEMU profiles

These profiles are restricted to the dedicated disposable QEMU NVMe fixture.
They may format it, change scoped driver/hugepage settings, inject faults, restart
the VM, and retain faulted disks. **Do not point them at a physical device.**

```sh
# Raw SPDK NVMe commands and actual asynchronous queues/completions:
python3 tests/run_framework.py --profile spdk --vm-dir build/vm/rocky9
# NVFUSE through the synchronous standalone SPDK transport:
python3 tests/run_framework.py --profile integration --vm-dir build/vm/rocky9
# Filesystem-integrated EIO, delay/timeout and removal:
python3 tests/run_framework.py --profile integration-faults --vm-dir build/vm/rocky9
# VM-cut scenarios on retained journal volumes:
python3 tests/run_framework.py --profile integration-powercuts --vm-dir build/vm/rocky9
```

The fixture checks the expected QEMU identity, serial `NVFUSEDEV0001`, 2 GiB
capacity, lack of mounts, and IOMMU isolation before binding. Any ARM VM-specific
IRQ compatibility adjustment belongs only to that VM, not general host setup.
A raw multiqueue pass is not a filesystem-parallelism claim. Standalone transport
errors can abort; expected fail-fast termination is not itself successful recovery.
Original-volume reopen and content verification must succeed separately.

## Primary fio and metadata measurements

Follow [primary fio setup](spdk-fio.md) using SPDK v26.09 and the exact fio 3.41
build. Start with CRC write/verify, then a separate-process CRC read of the same
files. Read workloads need previously populated data. Keep journal mode, sync
policy, file sizes, block size, queue depth, job count, NUMA placement, cache
state and device identity with every result. Earlier OFF-mode data is not an
ON-mode durability comparison.

The optional `tests/spdk/run_primary_fio.py` regression explicitly requires
`--format` and overwrites the selected volume. It does not bind devices or
reserve hugepages. Use an image/AIO bdev or an intentionally disposable NVMe,
never a volume containing data you want to retain. A failing step must stop
without reformatting away its evidence. The primary journal crash runner,
`tests/spdk/run_primary_journal.py`, specifically accepts a caller-owned
**bdev_aio image**; it is not a general physical-device fault injector.

For a bounded, mdtest-style metadata workload through the primary owner:

```sh
make -f Makefile.spdk metadata-bench SPDK_ROOT_DIR="$SPDK_DIR"
sudo env NVFUSE_SPDK_BDEV="$NVFUSE_TEST_BDEV" \
  NVFUSE_SPDK_CORE_MASK="$NVFUSE_TEST_CORES" NVFUSE_JOURNAL=1 \
  prlimit --core=0 --memlock=unlimited tests/spdk/metadata_bench \
  "$PWD/build/config/bdev.json" "$NVFUSE_TEST_BDF" /metadata-demo 4096 128
```

Use the environment/device prerequisites from the user guide and an absent
private `/metadata-demo` directory. The tool never formats. This is a custom
workload, not the mdtest/fs_mark executable. It measures create/close, stat,
open/close, readdir, rename, unlink, mkdir and rmdir. Its default sync boundary
is a batch of 128 operations; select batch `1` for per-operation sync. Do not
compare those durability conditions as if they were equivalent. It creates
empty files, so it is not a bulk-data corruption test. The empty fixture root
is retained. See [`metadata_bench.c`](../tests/spdk/metadata_bench.c) for options.

For XFS comparisons, use a real NVMe-backed XFS mount and match the durability
condition, data set and test duration. The six-profile fio runner in
[spdk-fio.md](spdk-fio.md#short-xfs-comparison-profiles) does not format or mount
XFS for you. API/SPDK versus XFS/libaio measures different complete access paths;
report CPU cost as well as IOPS/bandwidth. Avoid universal parity claims.

## Application and near-full stress

[`run_kernel_workload.py`](../tests/run_kernel_workload.py) can exercise parallel
Linux kernel untar/build/remove on a FUSE mount, with source-content and namespace
checks. Inspect `--help`; supply the requested archive/toolchain and choose the
backend explicitly. This is application compatibility/metadata stress, separate
from fio. It does not turn FUSE into the primary performance path.

[`fs_soak.c`](../tests/spdk/fs_soak.c) uses seeded payloads unique to each private
tree/file/offset. It supports full verification, exact directory enumeration,
rename/hardlink/truncate, deletion, and a seed-authenticated root before mutation.
It can build through `tests/spdk/Makefile.tools` as `APP=fs_soak` for the primary
runtime, or as a POSIX worker:

```sh
mkdir -p build/soak
cc -std=gnu11 -O2 -DNVSOAK_POSIX tests/spdk/fs_soak.c -o build/soak/fs-soak-posix
python3 tests/test_fs_soak.py
```

The oracle tests use temporary host directories, including wrong-seed refusal,
corrupt/swapped payload detection, unexpected names, and cooperative stop.
They do not themselves mount NVFUSE. Worker arguments are
`MODE ABSOLUTE_PRIVATE_ROOT SEED COUNT SIZE_BYTES SECONDS BATCH`; size zero
selects a repeating 4/8/16 KiB mix. `init` requires an absent root, and subsequent
modes must reuse its exact seed and the actual count created. A stopped or
space-limited create can produce fewer files than requested. Large primary
files use direct AIO; FUSE bulk sampling is explicitly partial verification.

The host-specific real-device supervisor used in the development lab is not
part of the source release. To orchestrate a near-full test with the reusable
worker, reserve one disposable device, keep API/FUSE ownership sequential,
and enforce explicit identity and capacity guards. A 95-97% target leaves a
reserve and does not exercise complete ENOSPC. Stop admission at the requested
duration, then finish verification/deletion and restore the environment;
total elapsed time can exceed the admission window. Record full versus sampled
verification separately. This is not a physical power-cut test.

## Result acceptance

Record actual completed operations, files/directories, payload bytes, peak usage,
cycle count, errors, reclamation deficits (including intentionally retained audit
roots), resource growth, and final device/environment state. Runtime elapsed
alone is not a pass. Do not mark an ongoing soak complete or claim memory-leak
coverage without periodic measurements. Keep failures and user cancellation
distinct. Complete ENOSPC, primary fault recovery, same-directory contention,
and physical power-loss qualification remain explicit tasks in [TODO](../TODO).

Store commands, JSON/JUnit, logs, failure images and benchmark samples under
`build/test-results/`, outside versioned design documentation. A useful failure
report includes the source revision/local patch, build flags, OS/CPU, backend,
controller/namespace identity, journal version, workload seed, first failing
operation and the retained-volume reopen result. Inspect shared logs for secrets
before publishing them.
