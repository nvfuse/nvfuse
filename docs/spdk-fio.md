# FIO through the primary NVFUSE SPDK API

FIO can access files through NVFUSE's public asynchronous API without a FUSE mount. The modern engine uses the primary SPDK bdev reactor, not the synchronous compatibility transport or FIO's raw SPDK engine.

```
fio -> nvfuse_spdk.so -> NVFUSE AIO API -> SPDK bdev -> NVMe
```

## Build

Validated dependencies: SPDK v26.09 and fio 3.41 on Linux x86_64. Build fio from its source tree first (`./configure && make`); the engine must use the same fio source/configuration as the executable. SPDK needs its event, bdev, NVMe, malloc, accel, iobuf and socket components.

```sh
make -f Makefile.spdk fio \
  SPDK_ROOT_DIR=/absolute/path/to/spdk \
  FIO_SOURCE_DIR=/absolute/path/to/fio
```

This produces `build/spdk-primary/nvfuse.a` and `examples/fio_plugin/modern/nvfuse_spdk.so`. The archive contains the primary filesystem/reactor sources and no `NVFUSE_FILE_BACKEND` objects. `make spdk-primary` and `make spdk-fio` are convenience targets. The historical FIO plugin and INI configuration in the parent directory remain legacy examples; use the modern engine for SPDK 26.09.

## Device configuration

Bind only an explicitly selected test controller to an appropriate SPDK userspace driver and reserve hugepages before launching the engine. Do not run a global driver setup script on a shared storage server. Controller identity, namespace capacity and current usage must be checked before binding; exclude the OS controller.

Select the complete namespace in SPDK JSON. Device capacity is discovered automatically; no split or test-size cap is inserted:

```json
{
  "subsystems": [{
    "subsystem": "bdev",
    "config": [
      {"method": "bdev_nvme_attach_controller", "params": {
        "name": "Nvme0", "trtype": "PCIe", "traddr": "0000:01:00.0"
      }}
    ]
  }]
}
```

The selected bdev is `Nvme0n1`, exposing the whole namespace. Formatting initializes NVFUSE across the device; this is not an NVMe low-level format or a partition table. The current on-disk layout rounds capacity down to complete 128 MiB block groups, so less than 128 MiB at the end is outside the filesystem. Metadata also consumes space. Devices beyond the current signed 32-bit physical-cluster mapper limit (approximately 8 TiB) are rejected before format instead of silently truncated. The engine requires these explicit environment variables:

| Variable | Meaning |
| --- | --- |
| `NVFUSE_SPDK_JSON` | Absolute path to SPDK JSON configuration |
| `NVFUSE_SPDK_BDF` | The only PCI address allowed by the SPDK environment |
| `NVFUSE_SPDK_BDEV` | Exact bdev name that NVFUSE may open |
| `NVFUSE_JOURNAL` | Default `1`: metadata redo journal on the primary reactor. `0` is explicit legacy diagnostic mode. ON/OFF volumes require separate formats; modes cannot silently switch an existing volume. See [primary journaling](spdk-primary-journaling.md). |
| `NVFUSE_JOURNAL_BATCH_IO` | `1` (default) coalesces consecutive journal/home writes up to 128 KiB; `0` selects the per-block reference. Flush ordering and format are unchanged. |
| `NVFUSE_JOURNAL_COMPARE_READS` | `0` (primary default) stages complete protected images without reading old home contents. `1` restores compare/dedup reads. Partial writes always preserve old bytes; direct staging can use more journal credits/traffic. |
| `NVFUSE_JOURNAL_CRC` | `auto` (default) selects available CPU CRC32C instructions with a software fallback; `software` forces the byte-table reference. The disk checksum and journal traffic are unchanged. Set at process startup. |
| `NVFUSE_JOURNAL_PROFILE` | `1` enables owner commit-stage timers; default `0`. Keep OFF for ordinary fio performance measurements. |
| `NVFUSE_JOURNAL_MS` | Background owner commit interval, default `100` ms, accepted range1–1000. Submitted AIO is drained across all clients before commit. |
| `NVFUSE_FORMAT` | Only the exact value `1` enables destructive filesystem format; unset or `0` reopens existing data |
| `NVFUSE_SPDK_CORE_MASK` | SPDK core mask with at least two enabled cores; default `0x3`. First enabled core owns I/O; second owns the filesystem API. Select device-local physical cores for measurements. |
| `NVFUSE_SPDK_STATS` | Unset/`0` disables collection. `1` publishes `/tmp/nvfuse-spdk-<pid>.stats`; an absolute new path selects a custom record. Watch with `scripts/nvfuse-stat 1`. |
| `NVFUSE_SPDK_POLL_QUEUES` | `1` (default) uses SPSC polled data queues; `0` retains coalesced-message transport for comparison. Lifecycle and metadata calls remain synchronous. |
| `NVFUSE_BUFFER_MB` | Default `auto`: 0.1% of selected bdev capacity, rounded down to MiB and clamped to 16–512 MiB. An integer 16–512 overrides automatic sizing. Invalid values fail before SPDK startup. |

The engine places bdev I/O on SPDK thread `nvfuse_io` on the first enabled core and filesystem API calls on `nvfuse_fs` on the second enabled core. With the default mask these are cores 0 and 1. Choose device-local physical cores from the actual host topology and pin fio workers separately. It requests 2048 MiB of SPDK memory. Reserve sufficient hugepages on the selected NUMA node before starting the runtime.

The cache size is process-wide and shared by all jobs. Direct I/O bypasses file-data caching but still requires block-mapping metadata. Automatic sizing uses the complete selected bdev's capacity, independently of fio file size. For example, 100 GiB selects 102 MiB; a 7.68 TB namespace selects the 512 MiB cap. This cap reserves at most 25% of the fixed 2048 MiB SPDK memory request for buffer payloads; cache bookkeeping and other allocations also consume memory, so this is not a guarantee that every workload fits. The engine logs mode, device bytes, uncapped target, cap and selected budget as `NVFUSE_CONFIG`. Use `NVFUSE_BUFFER_MB=128` to reproduce the earlier explicit 128 MiB measurements, or `NVFUSE_BUFFER_MB=16` for the historical default.

## Run and verify

For the first run on an approved disposable full-device bdev, the following explicitly formats it and writes/verifies a 1 GiB file. The selected device is reformatted; all previous filesystem contents become unusable. Substitute paths, controller and bdev only after checking device identity and usage.

```sh
sudo env \
  NVFUSE_SPDK_JSON=/absolute/path/to/bdev.json \
  NVFUSE_SPDK_BDF=0000:01:00.0 \
  NVFUSE_SPDK_BDEV=Nvme0n1 \
  NVFUSE_FORMAT=1 \
  prlimit --core=0 --memlock=unlimited \
  /absolute/path/to/fio/fio examples/fio_plugin/modern/api-verify.fio \
  --output-format=json --output=/absolute/path/to/write-verify.json
```

For a separate-process persisted-data check, reuse the same environment with `NVFUSE_FORMAT=0` and add `--rw=read --verify_only=1`. This reopens the existing filesystem; never reformat to make a failed recovery check pass. Use a separate fio process for each job.

`filename=/fio-large` is a path inside NVFUSE. It does not refer to a host file and does not require a mounted directory. FIO's JSON result is written to the host path provided by `--output`; NVFUSE/SPDK diagnostics go to the process log and should be captured too.

## Repeatable regression

After preparing the explicit test bdev, run the destructive regression suite below. It replaces the selected NVFUSE filesystem and writes three test files. It never binds devices or adjusts hugepages. Use an image/AIO bdev for development, or a separately approved disposable NVMe bdev. A 512 MiB image is sufficient for the default 64 MiB large-file range.

```sh
sudo prlimit --core=0 --memlock=unlimited python3 tests/spdk/run_primary_fio.py \
  --fio /absolute/path/to/fio/fio \
  --engine "$PWD/examples/fio_plugin/modern/nvfuse_spdk.so" \
  --config /absolute/path/to/bdev.json \
  --bdf 0000:01:00.0 --bdev Nvme0n1 \
  --format --size 64M --report-dir /absolute/path/to/new-results
```

The suite covers 4 KiB/QD8, 64 KiB/QD8 and 1 MiB/QD1 writes with CRC verification, separate-process reopen checks, sequential and random reads, and 4 KiB/QD32 random writes with CRC verification. It also checks four jobs with 256 KiB/QD32 CRC writes, separate-process CRC rereads and random mixed I/O. Reports retain each fio command, JSON output and runtime log. A failing stage stops the suite without reformatting or deleting evidence. Report directories must be new.

## Supported workload limits

- Up to four fio jobs in one process with `thread=1` and one file per job. Jobs share one mounted filesystem, while each has separate AIO/completion queues and a distinct filename. Filesystem API work is serialized on the filesystem thread. Multiprocess access and shared-file concurrent writes are not supported.
- `direct=1`; read/write requests, offsets and buffers must be 4096-byte aligned. FIO allocates payload buffers from SPDK DMA memory. The engine submits through `nvfuse_aio_queue_submission` and completes through `nvfuse_io_getevents`.
- `iodepth <= 128` per job and `iodepth * (largest block size / 4096) <= 2048` per job, bounding worst-case fragmented backend requests across up to four 512-entry queue lanes. Examples: 4 KiB/QD32, 256 KiB/QD32 and 1 MiB/QD8. Unsupported settings fail setup.
- Sequential/random read and write, CRC verification, fsync and fdatasync requests are supported. TRIM and other unsupported directions return `EOPNOTSUPP`.
- FIO's configured size describes the intended test file range. Read jobs must target a previously populated range. The engine does not implement general file enumeration or host file allocation.
- Each fio job publishes data requests and consumes completions through bounded SPSC rings. The filesystem poller services up to 32 requests per client per pass. At most 128 requests per job may be outstanding, including pending and completed requests. Open, close, fsync and lifecycle control still cross a synchronous pthread-to-SPDK message boundary.
- Data queues between the filesystem and I/O threads use SPSC polling by default. `NVFUSE_SPDK_POLL_QUEUES=0` switches only that transport back to mutexes and coalesced notifications. All mapping and metadata ownership remains on one filesystem thread; this does not enable parallel filesystem writers.

The primary reactor now defaults to [metadata journaling](spdk-primary-journaling.md), with owner-side overlay staging and AIO fencing. V1/v2 use synchronous home checkpointing; experimental v3 defers home materialization until log pressure requires it. Legacy device/cache fail-fast behavior remains. Recovery coverage and limitations are described separately from portable user transactions; process-exit tests do not establish physical SSD power-loss protection. Earlier primary fio measurements used journaling OFF.

## Short XFS comparison profiles

`scripts/benchmark-filesystem-fio.py` runs the six requested profiles in separate fio processes against an already formatted filesystem. It does not format, bind devices or mount anything. `--prepare` fully writes four files first; `--size=20g` means 80 GiB total. The default timed duration is 10 seconds per profile. Use 300 seconds for a later longer run.

```sh
# XFS: directory must be a mount on the selected test device.
python3 scripts/benchmark-filesystem-fio.py \
  --fio /absolute/path/to/fio/fio --engine libaio \
  --directory /mnt/xfs-test --prepare --runtime 10 \
  --report-dir /absolute/path/to/new-xfs-results

# API/SPDK: configure the environment from the earlier example, with
# NVFUSE_FORMAT=0. The directory / refers to the NVFUSE namespace.
python3 scripts/benchmark-filesystem-fio.py \
  --fio /absolute/path/to/fio/fio \
  --engine "$PWD/examples/fio_plugin/modern/nvfuse_spdk.so" \
  --directory / --prepare --runtime 10 \
  --report-dir /absolute/path/to/new-nvfuse-results
```

Both use `direct=1`, `thread=1`, `numjobs=4`, `iodepth=32`, group reporting and distinct per-job files. The three sequential profiles use 256 KiB blocks with 0%, 100% and 75% reads; the three random profiles use 4 KiB blocks with the same ratios. This compares complete access paths (NVFUSE API/SPDK versus XFS/libaio), including their different CPU/runtime overhead. It is not an isolated comparison of filesystem algorithms. The primary adapter's four application jobs still share a single filesystem thread and a single bdev I/O thread.

For live IOPS, bandwidth, API latency and queue counts, see
[iostat-style monitoring](spdk-live-monitoring.md). Diagnostic tools such as
`tests/spdk/instrument_hotpath.py` and `tests/spdk/instrument_primary.py` belong
in an isolated copy; use uninstrumented binaries for throughput comparisons.
Nested timers overlap and must not be added as independent costs. Store run
logs and machine-readable measurements under `build/test-results/`.
