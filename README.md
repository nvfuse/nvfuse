NVFUSE
======

NVFUSE is an embeddable file system as a library running in the user-space incorporated with [SPDK library](https://github.com/spdk/spdk) which is a user-space NVMe driver newly introduced by Intel. Using this file system applications can directly submit I/O requests to NVMe SSDs. Then, it provides high performance, high reliability as well as POSIX-like interfaces (e.g., nvfuse_open, nvfuse_read, nvfuse_write, and nvfuse_close functions). The primary performance interface is the direct API with SPDK. Optional Linux FUSE 3 mounts and kernel/image backends provide compatibility and development access. Key features are listed as follows.

 - Running in userspace without any interactions with kernel driver so that interrupts, context switches, and memory copies are eliminated. 
 - Embeddable file system as a library that introduces new interfaces are similar to POSIX API.
 - Simple file system layout that is identical to EXT family file system (e.g., EXT3).
 - Metadata redo journaling defaults ON in the modern serialized API/SPDK runtime. Optional application transactions remain a standalone-backend feature. See the support limits below; NVMe commands alone do not make multiple metadata writes atomic.

Local development on macOS and Linux
===================================

A separate file-image backend supports macOS and Linux development without SPDK,
DPDK, hugepages, root access, or FUSE. It runs the existing NVFUSE filesystem,
including its metadata, buffer cache and directory indexes, against a regular
file using `pread`, `pwrite` and `fsync`.

Requirements: `clang`, `make` and Python 3.9 or later. On macOS, install Xcode
Command Line Tools. Verified on Apple Silicon macOS and Rocky Linux 9 ARM64 in
Colima. Modern SPDK correctness integration and optional FUSE mounting are
available in the dedicated QEMU environment described below; performance-engine
qualification remains separate.

    make local
    make local-test
    make local-sanitize

`local-test` creates a disposable 256 MiB sparse image, then runs separate
processes to create and verify a binary file larger than 3 MiB, perform an
unaligned overwrite, verify 40 small files, rename and delete entries, and verify
those changes after remount. It also checks repeated mounts within one process
and rejects existing create targets, locked images, symlinks and invalid image
headers. Temporary images are removed automatically; failures exit nonzero.
`local-sanitize` runs the same tests with AddressSanitizer and
UndefinedBehaviorSanitizer. Legacy source warnings remain visible; the new
backend and test executable compile with `-Werror`.

Pull requests run `make local-test` on a GitHub-hosted macOS runner and both
regular and ASan/UBSan tests in a Rocky Linux 9 container on an Ubuntu x86_64
runner through `.github/workflows/local-tests.yml`. The workflow can also be
started manually from the Actions tab. SPDK CI remains follow-up work.

To run the Rocky Linux 9 tests through an existing Colima Docker VM:

    sh scripts/test-rocky9.sh

The script builds a toolchain image with `clang`, `compiler-rt`, `make` and
Python, then runs both the regular and ASan/UBSan tests. Source is mounted read
only; build products and disposable filesystem images stay inside a temporary
container. Tests run without network access. The toolchain image is retained
for reuse. Override `NVFUSE_DOCKER_CONTEXT` to use a different Docker context.
On Apple Silicon, the default Colima VM tests Linux ARM64; the GitHub Actions
Rocky 9 job tests x86_64. Rocky 8 requires separate validation.

To keep an image for debugging, use a new path:

    ./build/local/local_test create /tmp/nvfuse-dev.img
    ./build/local/local_test verify /tmp/nvfuse-dev.img
    ./build/local/local_test mutate /tmp/nvfuse-dev.img
    ./build/local/local_test final /tmp/nvfuse-dev.img

The executable's source is `tests/local_test.c`. Applications can link
`build/local/nvfuse.a`, compile with `-DNVFUSE_FILE_BACKEND -Iinclude`, open a
filesystem with `nvfuse_file_open()` from `include/nvfuse_file.h`, call the normal
`nvfuse_*` file APIs, and finish with `nvfuse_file_close()`. Creation requires a
new file path and an image size in multiples of 128 MiB; reopening never formats
an existing image. Use a different path for another fresh image.

This backend currently supports one filesystem handle per process and calls
from one thread. I/O executes synchronously, including batched cache writeback.
It does not validate physical NVMe performance, multiprocess IPC, or
cross-platform image interchange, and does not mount a volume in Finder. Use
disposable development images: underlying block-I/O errors or partial mount
failures abort the process because the legacy core cannot reliably unwind those failures.
The original SPDK build commands below remain separate from this local mode.
For SPDK v26.09 and fio 3.41, use the [primary API/SPDK fio engine](docs/spdk-fio.md).
It exercises NVFUSE AIO without a FUSE mount. The primary runtime defaults to
[metadata journaling](docs/spdk-primary-journaling.md); the experimental
[compact v3 log](docs/journal-compact-log-v3-design.md) requires an explicit new
format. For live IOPS, bandwidth, latency and queues, enable
`NVFUSE_SPDK_STATS=1` and use [live monitoring](docs/spdk-live-monitoring.md).

An optional Linux FUSE 3 frontend is available with `make fuse` for mounting a
normal image inside the QEMU Linux development guest. It shares the existing
core and has a separate build. See [Linux FUSE integration](docs/linux-fuse-integration.md)
for setup, commands, tests, and capability limits. The native macOS local tests
and the original SPDK build remain independent of FUSE.
The optional `make fuse-spdk` build adds a synchronous SPDK-backed mount.
See the [interface/backend matrix](docs/backend-matrix.md) for all six
combinations, their performance roles, API selection and validation.
Use `make backend-regression-qemu` for three functional rounds of all six
combinations in the dedicated VM, or `make backend-regression` for the four
portable Linux combinations. The [backend matrix](docs/backend-matrix.md)
describes the compatibility boundary. These defaults run no performance benchmarks.

Metadata journaling and optional user transactions
-------------------------------------------------

New standalone images reserve a 16 MiB metadata redo journal in BG0. Reopening
validates and replays the original journal before mounting; reformatting is not
recovery. Ordinary supported operations obtain automatic internal transactions.
Legacy clean images remain readable through their old mode and have no journal.

Applications can optionally group operations using `nvfuse_tx_begin`,
`nvfuse_tx_commit` and `nvfuse_tx_abort` from `include/nvfuse_tx.h`:

- `NVFUSE_TX_METADATA`: atomic metadata; ordinary file bytes may survive abort.
- `NVFUSE_TX_DATA`: atomic metadata and regular-file bytes using full-block redo.

Begin requires no open descriptors, one owning thread, and no nested TX.
Capacity is 4075 distinct protected blocks including metadata; zero budget uses
that maximum. Abort invalidates descriptors opened inside the group. Explicit
sync inside a TX returns `-EBUSY`. Direct/AIO, xattrs, fallocate, sparse writes
past EOF and truncate growth are outside this reference implementation.
Legacy cache errors and some allocation failures still terminate the process.

    make -f Makefile.local journal-test
    make -f Makefile.local journal-bench

The journal tests combine a separate volatile/durable persistence model with
fresh-process original-image replay. Reports and benchmark samples are saved
under `build/test-results/`; failure images are retained in the reported temp
folder. Existing `local-test` and `local-framework` also exercise new journal
images by default. The standalone SPDK integration runs the TX oracle on the
dedicated QEMU NVMe and checks recovery after injected I/O failures.

See [disk format, ordering, recovery diagrams and ext4/XFS comparison](docs/metadata-journaling-design.md),
[the user TX API contract](docs/user-transactions-design.md),
[checkpoint ownership](docs/checkpoint-concurrency-design.md), and
[durable transaction outcomes](docs/transaction-outcomes-design.md).

Metadata boundary and reclamation tests
---------------------------------------

    make local-boundary-test
    make local-limit-test
    make local-stress-test
    make local-resilience-test
    make local-chaos-test

The C++17 boundary oracle executes the production C indirect mapper/truncator
at the full 1024-way geometry, including physical block `0xffffffff`, and
reclaims/reuses the complete tree twice without storing file payload. It also
checks journal capacity and final-slot recovery. The supplementary C image
suite exhausts real inode/block resources, deletes the complete test directory,
compares free counters to the initial baseline, and repeats allocation/deletion.
The stress suite sweeps every inode bitmap slot of the maximum journal-volume
geometry twice, compares mixed operations against an independent namespace and
content model, and exercises 70 near-full crash windows with recovery/reuse.
Python runners retain JSON, JUnit and logs under `build/test-results`.

Deterministic test framework
----------------------------

Run the extended filesystem and file-backend tests on macOS or Linux:

    make local-framework

The default seeds are `1` and `0xc0ffee`. Override them, keep successful images,
or choose a report directory with:

    python3 tests/run_framework.py --seeds 1,42 --keep-images \
        --report-dir build/test-results/my-run

Each run writes `run.json`, per-case logs, `results.json`, and `junit.xml`.
Failed images remain available for diagnosis; successful images are removed
unless `--keep-images` is set. Reports are also retained on the host when using
`sh scripts/test-rocky9.sh`. PR CI runs this local suite on macOS and Rocky 9,
including the Rocky 9 ASan/UBSan build.

The test-only `nvfuse_file_open_mode(path, create, size, 1)` API creates a
**metadata-only image**. It preserves inode sizes, allocation bitmaps, directory
indexes, indirect blocks, and symlink contents, while discarding regular-file
payload writes before they enter the data cache. Buffered reads of regular
files return zeros immediately and after remount. Async and direct I/O paths
are rejected in this mode. The image carries a distinct on-disk signature:
normal and metadata-only opens reject each other's images. This mode is for
metadata workloads; use normal images to detect payload corruption.

The suite compares identical 32 MiB file workloads in both modes to verify
physical storage savings, and checks create/rename/delete, file sizes, indirect
block boundaries, block/inode reclamation, and fresh-process remounts. Normal
mode additionally compares a seeded shadow model across 500 overwrites per
seed, nine transfer sizes, zero/ones/alternating/random patterns, EOF reads,
and invalid offsets. File-backend queues are synchronous: their capacity,
completion identity, wraparound, and invalid-request tests do not claim real
parallel I/O.

For actual SPDK asynchronous queues and QEMU failure injection, use the
dedicated local ARM64 VM prepared with SPDK v26.09, its DPDK submodule,
`~/.venvs/spdk`, and the pinned-key `ssh.sh` helper:

    python3 tests/run_framework.py --profile spdk --vm-dir build/vm/rocky9

For filesystem integration on that same disposable NVMe, use:

    python3 tests/run_framework.py --profile integration --vm-dir build/vm/rocky9

This experimental transport links the standalone, single-process NVFUSE core
to SPDK v26.09's NVMe API. It uses one synchronous queue and a DMA bounce
buffer; the core's heap buffers are never submitted directly to NVMe. It
reuses the file-backend test workloads: 16 filesystem stages per seed cover
data comparison, remounts, rename/delete, normal and metadata-only formats,
and rejection of the wrong format mode. Separate VM restarts verify retained
file contents and metadata, followed by deletion and another remount. Each
device close reports actual NVMe read, write, and flush command counts.
This is not a migration of the legacy multi-process bdev/event reactor.

To inject failures while NVFUSE is mounted, use:

    python3 tests/run_framework.py --profile integration-faults --vm-dir build/vm/rocky9

Use `--fault-cases hotremove` to repeat one case. The removal test checks
QEMU's PCI inventory before and after unplug, rather than accepting the
`device_del` acknowledgement alone.

This suite discovers the physical LBA of an existing file's first data block,
then injects sector-specific read/write EIO through QEMU's blkdebug layer.
Test-only barriers coordinate actual NVMe command submission with QEMU read
throttling and device removal. The cases cover a stalled read resumed before
the deadline (followed by full data comparison and reboot), a read exceeding
the 10-second transport deadline, and removal with a submitted read pending.
The synchronous NVFUSE transport does not poll SPDK's hotplug scanner; unplug
can therefore terminate through the I/O deadline rather than an immediate
removal notification.
Legacy cache paths still abort on block-I/O failure. Tests require SIGABRT and
the corresponding transport error, then reopen the original journal volume
after VM restart and verify its payload before reformatting. The integration
profile also exercises explicit metadata/DATA transactions and original-image
replay after interruption at journal persistence boundaries.
Faulted disks are retained as compact qcow2 images in the report directory
before another test reformats the disposable disk. A final format and data
round trip check that the device and environment can be reused.

`--profile all` runs local, raw SPDK, filesystem integration, and filesystem
fault suites.
`tests/spdk/start_vm.sh` starts the existing
VM images with HVF, SMMUv3, QMP, a dedicated hotpluggable PCIe port, and a
blkdebug-backed NVMe. The SPDK suite verifies the exact QEMU vendor/device,
serial `NVFUSEDEV0001`, 2 GiB capacity, absence of mounts, and an isolated IOMMU
group before binding that device only. It writes to the disposable NVMe image,
reboots the VM to apply error rules, and shuts it down after testing. Do not
point these tests at a physical device. The VM images, credentials, and SPDK
checkout are local prerequisites, not included in this repository.

| Coverage | Local file backend | SPDK + QEMU |
| --- | --- | --- |
| Sizes, offsets, patterns, data comparison | Buffered, including unaligned bytes | Sector-aligned NVMe commands |
| Queue depth and multiple queues | Synchronous queue contract | 1/2/4 queues, depths 1/8/32/64, actual completion counts |
| Reopen and reconnect | Fresh filesystem processes | Ten fresh SPDK processes |
| Capacity and invalid requests | Range rejection and fail-fast short reads | First/last LBA and invalid read/write ranges |
| I/O failures | Fail-fast block-I/O contract | Raw NVMe failure handling; NVFUSE read/write EIO, timeout/resume, and removal (`integration-faults`) |
| Recovery | Original-image journal replay and an independent persistence model | Raw transient-error retry; original journal-volume recovery and payload verification after reboot |
| NVFUSE metadata lifecycle | Normal and metadata-only images | Both formats through NVMe; VM restart persistence (`integration`) |

The QEMU ARM platform lacks interrupt remapping. Inside this isolated VM only,
the test temporarily enables VFIO's IRQ compatibility setting while retaining
DMA IOMMU protection; it restores the setting, hugepage reservation, and Linux
driver on exit. A timed-out command is resumed to verify data, and an unplugged
controller is recovered by restarting the VM. Multiple queues currently
exercise the raw NVMe test program; the NVFUSE transport remains synchronous.
Hot re-add without reboot, physical SSD power-loss durability, and parallel
NVFUSE I/O are not claimed. Journal recovery is limited to the supported
standalone paths documented above. Filesystem integration is reported as skipped unless selected with
`--profile integration`, `--profile integration-faults`, or `--profile all`.

Prerequisites
=============
In order to build NVFUSE, some relevant libraries must be configured. 

[DPDK](http://dpdk.org/doc/quick-start) and [SPDK](https://github.com/spdk/spdk) are required.

    1) Move onto https://github.com/spdk/spdk
    2) DPDK and SPDK must be installed based on their install guide explained in README.

Ubuntu/Debian:
	
    # sudo apt-get install -y gcc make libaio-dev libfuse-dev
	# sudo apt-get install -y gcc g++ libcunit1-dev libaio-dev libssl-dev

SPDK compile:

	# git clone https://github.com/spdk/spdk.git
	# cd ~/spdk
	# git submodule update --init
	# ./configure
	# make

Building
========

Once the prerequisites are installed, run 'make' within the NVFUSE directory to build the NVFUSE library and examples.

    # make SPDK_ROOT_DIR=/home/ysoh/spdk DPDK_DIR=/home/ysoh/spdk/dpdk/build

Before conducting an NVFUSE application, the kernel NVMe driver must be unloaded and some hugepages must be allocated. It can be done with the automation script like the below command line.

    # sudo scripts/setup.sh config

Disabling ASLR (Address Space Layout Randomization) is also required like this.

	# sudo scripts/disable_aslr.sh

Move to examples directory and execute helloworld. 

    # cd examples/helloworld
    # sudo ./start_helloworld.sh

Make use of performance benchmark.
	# cd examples/perf
	# sudo ./start_perf.sh

After finalizing an NVFUSE application, the kernel NVMe driver and hugepages can be reverted with the automation script.

    # sudo scripts/setup.sh reset

| Examples Not Working (To Be Enabled) |
|--|
|control_plane_proc|
|dpdk_simple_mp|
|fio_plugin|
|libfuse|
|multi_process|
|raw_block_device_perf|
|regression_test|
|create_1m_files|
|fio-nvfuse|
|fsync_test|
|kernel_filesystem|
|mkfs|
|reactor|

To do
=====

 - File system consistency management via light-weight journaling
 - Integration with scalable file system such as Ceph and GlusterFS

Contact
=======
Yongseok Oh (yongseok.oh@sk.com)
