# Live API/SPDK monitoring

`scripts/nvfuse-stat` is an iostat-style, read-only command for the modern API/SPDK fio adapter. It uses NVFUSE's own counters because Linux `iostat` cannot observe I/O submitted through a userspace-owned SPDK controller. It requires only Python 3 and does not require an SPDK RPC server.

## Enable and watch

Set `NVFUSE_SPDK_STATS=1` on the fio process, together with the existing explicit SPDK device configuration. This creates `/tmp/nvfuse-spdk-<pid>.stats`. It does not format or mount a device.

```sh
# Terminal 1: add this variable to the existing validated launch command.
sudo env NVFUSE_SPDK_STATS=1 NVFUSE_FORMAT=0 \
  NVFUSE_SPDK_JSON=/absolute/path/to/bdev.json \
  NVFUSE_SPDK_BDF=0000:dd:00.0 NVFUSE_SPDK_BDEV=Nvme0n1 \
  NVFUSE_SPDK_CORE_MASK=0xA NVFUSE_BUFFER_MB=128 \
  prlimit --core=0 --memlock=unlimited /path/to/fio \
  --ioengine=/path/to/nvfuse_spdk.so /path/to/existing-job.fio

# Terminal 2, on the same host:
./scripts/nvfuse-stat 1
./scripts/nvfuse-stat 1 10              # ten sampling intervals
./scripts/nvfuse-stat --pid 12345 1     # one process
./scripts/nvfuse-stat --json 1          # JSON lines for a collector
```

Core mask and worker affinity must match the actual machine topology; `0xA` is the validated test host's placement, not a portable recommendation. The selected bdev must already be safely bound and provisioned before launching fio.

On the physical test server, the verified command is installed as a user-directory symlink:

```sh
~/nvfuse-dev/bin/nvfuse-stat 1
```

It waits for an active instrumented process and automatically switches to newly started processes. Add `$HOME/nvfuse-dev/bin` to your own `PATH` to invoke `nvfuse-stat 1` directly. The device-aware test build is under `~/nvfuse-dev/run/stats-device-build-20261113/`.

## Multiple devices

The current engine opens one explicitly selected bdev per process. Run separate instances for separate devices; the command discovers all active records and prints one row per process/device, including PCI BDF and bdev name. For a physical NVMe target, the PCI field identifies the explicitly allowed controller. In image/AIO regression configurations, that address is only the PCI allowlist; use the bdev name to identify the actual image backend. The monitor does not discover arbitrary stacked-bdev topology. The same bdev name (for example `Nvme0n1`) can occur in different processes, so use the BDF to select a physical controller unambiguously.

```sh
~/nvfuse-dev/bin/nvfuse-stat 1                            # all active devices
~/nvfuse-dev/bin/nvfuse-stat --device 0000:dd:00.0 1      # selected controller
~/nvfuse-dev/bin/nvfuse-stat --device Nvme0n1 1          # every matching bdev name
~/nvfuse-dev/bin/nvfuse-stat --total 1                   # individual rows plus TOTAL
```

`--total` sums only the rows displayed in that interval, after filters. Total await is weighted by the corresponding read/write completion rates, not an arithmetic average of device latencies. It represents the displayed application requests, not all device traffic or a storage-pool utilization figure. This monitor does not add multi-device striping or multi-device mounting to one engine process.

## Columns

| Column | Meaning |
| --- | --- |
| `device`, `pid` | PCI BDF/bdev name and the writer process PID |
| `rIOPS`, `wIOPS` | Successful completed data requests per second, measured between published snapshots |
| `rMiB/s`, `wMiB/s` | Completed payload bytes per second / 2^20 |
| `r_await`, `w_await` | Mean request latency in microseconds from core API submission to FS completion processing; excludes frontend pending and worker-delivery queues |
| `infl` | Accepted API requests awaiting FS completion at snapshot time |
| `pend` | Requests waiting in the frontend submission rings |
| `ready` | Completed requests waiting for fio workers to retrieve them |
| `err/s` | Submission/completion failures per second |
| `state` | `run` or `stop` |

These are filesystem data-path counters. Metadata/device I/O, FUSE requests, kernel backends, image-file backends and arbitrary applications using the older API directly are not covered. `await` differs from both kernel iostat's device await and fio's complete end-to-end completion latency. Queue counts are instantaneous samples; they are not averages, maxima or a utilization percentage. Short fio runtime IOPS can differ from the displayed interval, especially across the ramp.

## Overhead and lifecycle

Statistics are disabled unless `NVFUSE_SPDK_STATS` is set to `1` or an absolute new file path. With collection enabled, the FS thread accumulates counters using its existing completion timestamps. A separate 100 ms SPDK poller publishes a small shared-memory record; there are no filesystem writes, printing, RPCs or locks per data request. Mapped page writeback is managed by the OS. The independent monitoring process reads only that record.

The record uses an aligned sequence counter to reject partial snapshots. Version 2 is little-endian Linux x86_64 and includes the PCI address and bdev name. The reader also accepts the earlier version-1 record, displaying its device as unknown. The reader checks size, magic, version and tick frequency, handles counter resets, and never changes SPDK or device state. The writer creates its file exclusively and does not follow symlinks or truncate existing paths. A collision causes a clear startup error. Files have mode 0644 subject to the launching process's umask; a restrictive umask may require the same user or sudo to read them.

A clean stop publishes final counters with state `stop` and leaves the small file available for inspection. Automatic discovery ignores stopped/dead processes. Use `--file /tmp/nvfuse-spdk-<pid>.stats` to read an explicit path. Old records can be removed after their writer has exited. They contain counters and PIDs, not file contents or credentials.
