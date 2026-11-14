# NVFUSE user guide

This guide starts with a regular image file, then covers the primary API/SPDK
runtime and optional Linux mounts. Commands run from the NVFUSE repository root
unless a different working directory is shown. Clone the repository URL supplied
by the project, enter the checkout, and keep build trees for different runtimes
separate. There is no system-wide SDK/package installation step in this guide.

## Choose the path

| Need | Configuration | Entry point |
| --- | --- | --- |
| Embed NVFUSE with asynchronous NVMe I/O | **Primary API + SPDK**, Linux | `Makefile.spdk`, modern runtime/fio engine |
| Try the C APIs without a device | API + image, macOS/Linux | `Makefile.local`, `nvfuse_backend_open()` |
| Use an existing Linux block node | API + kernel driver | Standalone `nvfuse_backend_open()` |
| Access files from shell commands/programs | FUSE 3 + image/kernel/SPDK, Linux | `make fuse` or `make fuse-spdk` |

The [backend matrix](backend-matrix.md) explains the six combinations. A FUSE
mount and the standalone SPDK opener use a synchronous transport. The primary
reactor is a different build and integration path. Kernel backend means userspace
NVFUSE issuing I/O to a Linux block node, not a kernel NVFUSE filesystem module.

## Prerequisites

| Path | Requirements |
| --- | --- |
| Native image API | C compiler (`clang` is the Makefile default), Make, Python 3.9+ for runners |
| Boundary/model tests | Also a C++17 compiler |
| Linux FUSE image/kernel | Native dependencies, libfuse **3** development headers, pkg-config, `fusermount3`, accessible `/dev/fuse` |
| Primary API/SPDK | Linux x86_64, GCC/toolchain, built **SPDK v26.09** including its submodules, libaio/UUID development libraries, sufficient hugepages, exclusive userspace controller access |
| Primary fio engine | Also built **fio 3.41**; build the plugin against that same source/configuration |
| FUSE + SPDK | FUSE prerequisites plus built SPDK and exclusive device access |

On macOS install Xcode Command Line Tools (`xcode-select --install`). The native
image API does not need root, SPDK, DPDK, hugepages, or a filesystem extension.
Linux mounting can run in a VM; macFUSE/Finder mounting is not implemented.

For an image API/FUSE development environment:

```sh
# Debian/Ubuntu:
sudo apt-get install clang make python3 pkg-config fuse3 libfuse3-dev
# OR Rocky Linux (enable the distribution's development repositories if needed):
sudo dnf install clang make python3 fuse3 fuse3-devel pkgconf-pkg-config
```

SPDK has additional distribution-specific build dependencies. Follow the
[SPDK build instructions](https://github.com/spdk/spdk#building) for the selected
version. A typical source build, in separate checkouts, is:

```sh
git clone --branch v26.09 --recurse-submodules https://github.com/spdk/spdk.git "$HOME/src/spdk"
(cd "$HOME/src/spdk" && ./configure && make -j4)
git clone --branch fio-3.41 https://github.com/axboe/fio.git "$HOME/src/fio"
(cd "$HOME/src/fio" && ./configure && make -j4)
```

Install missing dependencies reported by configure before continuing. The default
SPDK build must include the event/bdev/NVMe components and modules used by
`examples/fio_plugin/modern/Makefile`. This project does not download a prebuilt
SPDK library or automatically configure the host's device drivers.

## First filesystem: API + image

```sh
make local
make local-test
cc -std=gnu11 -D_GNU_SOURCE -DNVFUSE_FILE_BACKEND -Iinclude \
  examples/quickstart/image_api.c build/local/nvfuse.a -lpthread -lm \
  -o build/local/image-api
./build/local/image-api create build/hello.img
./build/local/image-api read build/hello.img
```

The final lines should include `PASS created and synced` and
`PASS reopened and verified`. The image has 256 MiB logical capacity; allocated
host bytes can be smaller. Creation refuses an existing path. To start another
example, choose a new image path. The read command never formats.

The [complete C example](../examples/quickstart/image_api.c) opens
`NVFUSE_BACKEND_IMAGEFILE`, creates a file using `nvfuse_openfile_path()`, writes
and fsyncs it, closes the descriptor and filesystem, then verifies its exact
contents in a second process. Compile standalone applications with
`-DNVFUSE_FILE_BACKEND -Iinclude` and link `build/local/nvfuse.a`, pthreads and libm.
Do not mix this archive or its defines with primary SPDK objects.

`nvfuse_backend_options` has these open-time settings:

| Setting | Creation/format | Reopen |
| --- | --- | --- |
| `kind` | IMAGEFILE, KERNEL, or SPDK (when linked) | Same backend, or a supported sequential access path |
| `target` | New image path, real Linux block-node path, or SPDK BDF | Existing target |
| `format` | `1`, explicit initialization | **`0`** |
| `size_bytes` | Image: explicit multiple of 128 MiB. Device: zero uses whole capacity; nonzero explicitly bounds a test fixture. | **`0`**, discover stored geometry |
| `namespace_id` | Standalone SPDK only; zero defaults to 1 | Same namespace |

One standalone handle per process is supported. All calls, including close,
run on its opening thread. API-created roots do not automatically receive the
FUSE formatter's invoking-user ownership; set ownership/mode through the API
if ordinary mounted clients will need write access.

## Primary API/SPDK on Linux

### Build and prepare an exclusive test namespace

```sh
export SPDK_DIR="$HOME/src/spdk"
export FIO_DIR="$HOME/src/fio"
make -f Makefile.spdk fio SPDK_ROOT_DIR="$SPDK_DIR" FIO_SOURCE_DIR="$FIO_DIR"
mkdir -p build/config build/test-results/first-spdk
```

Outputs are `build/spdk-primary/nvfuse.a` and
`examples/fio_plugin/modern/nvfuse_spdk.so`. `make spdk-primary` and
`make spdk-fio` are convenience targets. Plain `make` is the historical build.

Before binding a controller, identify an unused namespace by **serial, PCI BDF,
namespace ID and capacity**. Exclude the OS controller, mounted filesystems,
swap, LVM/RAID members, holders, and any other active users. Binding a controller
affects all its namespaces, so check the entire controller rather than one node.
Useful read-only inventory commands are:

```sh
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT,SERIAL
findmnt
cat /proc/swaps
lspci -Dnnk
lscpu -e
```

Provision userspace access following [SPDK system configuration](https://spdk.io/doc/system_configuration.html).
Use VFIO with an appropriate IOMMU group where available. The UIO fallback
requires an environment that permits it; do not weaken host security settings
as a generic install step. Restrict any **upstream SPDK** setup invocation to the
selected controller (`PCI_ALLOWED`), inspect its version's hugepage effects,
and record the original driver, driver override and per-NUMA hugepage counts.
The repository's historical `scripts/setup.sh` is not a scoped provisioning tool.

The primary runtime requests **2048 MiB** of SPDK memory. Ensure enough *free*
hugepages on the selected NUMA node(s), and choose at least two available CPU
cores, preferably local to the device. It polls those cores; high CPU usage can
be expected while idle. Driver binding and hugepage provisioning require
administrator rights or preconfigured permissions and are operator steps.

### Select the whole namespace

Replace the BDF placeholder below with the controller you identified. Save this
as `build/config/bdev.json`; the same BDF must appear in the environment:

```json
{
  "subsystems": [{
    "subsystem": "bdev",
    "config": [{
      "method": "bdev_nvme_attach_controller",
      "params": {
        "name": "Nvme0", "trtype": "PCIe", "traddr": "0000:BB:DD.F"
      }
    }]
  }]
}
```

`Nvme0n1` means namespace 1 on that attached controller. Select the actual bdev
if your namespace ID differs. No small partition/split is inserted. Formatting
uses complete 128 MiB block groups over the selected namespace; metadata and a
journal consume space. The current mapper rejects device formats larger than
approximately 8 TiB. A format creates NVFUSE metadata, not a partition table or
an NVMe low-level format; `lsblk` need not identify a mounted filesystem for an
SPDK-owned controller.

### First format, write and verify

Set these shell variables after checking the target. Placeholders are deliberate
and will not work unchanged:

```sh
export NVFUSE_TEST_BDF=0000:BB:DD.F
export NVFUSE_TEST_BDEV=Nvme0n1
export NVFUSE_TEST_CORES=0x3
```

`0x3` selects cores 0 and 1; replace it to match the host's allowed CPU/NUMA
placement. The first enabled core owns bdev I/O and the second owns filesystem
operations. Keep application workers on other cores for performance measurement.

**The following command destroys existing contents of the selected namespace.**
Use it only for an intentionally new, disposable volume. It writes and verifies
`/fio-large` inside NVFUSE, without a FUSE mount:

```sh
sudo env NVFUSE_SPDK_JSON="$PWD/build/config/bdev.json" \
  NVFUSE_SPDK_BDF="$NVFUSE_TEST_BDF" NVFUSE_SPDK_BDEV="$NVFUSE_TEST_BDEV" \
  NVFUSE_SPDK_CORE_MASK="$NVFUSE_TEST_CORES" \
  NVFUSE_JOURNAL=1 NVFUSE_FORMAT=1 NVFUSE_SPDK_STATS=1 \
  prlimit --core=0 --memlock=unlimited \
  "$FIO_DIR/fio" examples/fio_plugin/modern/api-verify.fio \
  --output-format=json --output="$PWD/build/test-results/first-spdk/write.json" \
  > build/test-results/first-spdk/write.log 2>&1
```

New journal formats default to **v2**. To deliberately create an experimental v3
volume, additionally set `NVFUSE_JOURNAL_FORMAT_VERSION=3` on the format command.
The physical soak uses v3, but that does not change the general format default.
See [v3 design and qualification](journal-compact-log-v3-design.md).

### Reopen and verify existing data

Use a separate fio process, the same target, **`NVFUSE_FORMAT=0`**, and no format
step. This reads previously written data rather than replacing the volume:

```sh
sudo env NVFUSE_SPDK_JSON="$PWD/build/config/bdev.json" \
  NVFUSE_SPDK_BDF="$NVFUSE_TEST_BDF" NVFUSE_SPDK_BDEV="$NVFUSE_TEST_BDEV" \
  NVFUSE_SPDK_CORE_MASK="$NVFUSE_TEST_CORES" \
  NVFUSE_JOURNAL=1 NVFUSE_FORMAT=0 NVFUSE_SPDK_STATS=1 \
  prlimit --core=0 --memlock=unlimited \
  "$FIO_DIR/fio" examples/fio_plugin/modern/api-verify.fio \
  --rw=read --verify_only=1 --output-format=json \
  --output="$PWD/build/test-results/first-spdk/read.json" \
  > build/test-results/first-spdk/read.log 2>&1
```

Check the fio return code, each job's `error`, verification output, and logs for
`NVFUSE_JOURNAL enabled=1` with the expected version and capacity. Checksum success
and clean reopen test persistence, not physical SSD power-loss durability.
The [fio reference](spdk-fio.md) documents alignment, queue/job limits and the
six sequential/random comparison profiles.

### Monitor and stop

From a second shell on the same host:

```sh
./scripts/nvfuse-stat 1
./scripts/nvfuse-stat --device "$NVFUSE_TEST_BDF" 1
./scripts/nvfuse-stat --total 1
```

If using a different shell, set the BDF there too. Records are enabled by
`NVFUSE_SPDK_STATS=1`. These counters cover the modern API runtime's data requests,
not all metadata/device I/O, FUSE traffic, or kernel/image backends. See
[monitor semantics](spdk-live-monitoring.md) before interpreting latency or totals.

Let the application finish and check its return code. For a custom application,
drain requests, sync, close and stop the runtime cooperatively. Forced termination
is a recovery event, not a clean shutdown. Before rebinding to the kernel, verify
that all owners have exited and any FUSE mount is unmounted. Restore the selected
controller's original driver/override and the recorded hugepage counts without
changing other devices. The frontend does not perform this restoration for you.

## Embed the primary API/SPDK runtime

`build/spdk-primary/nvfuse.a` contains the primary core/reactor, not the standalone
opener. The current in-tree application integration reference is
[`src/nvfuse_spdk_runtime.c`](../src/nvfuse_spdk_runtime.c) with
[`nvfuse_spdk_runtime.h`](../include/nvfuse_spdk_runtime.h). It is not an installed,
stable system SDK. The [modern engine Makefile](../examples/fio_plugin/modern/Makefile)
and [API tool Makefile](../tests/spdk/Makefile.tools) show the required SPDK link
configuration; `-lnvfuse` alone does not provide SPDK/event module registration.

The lifecycle is:

1. Set the explicit bdev/BDF environment and call `nvfio_start(json, bdf, 0)` to
   reopen. Its final argument is the format flag; use `1` only for intentional
   initialization. Environment `NVFUSE_FORMAT` is interpreted by the fio engine,
   not a substitute for this argument in an embedded caller.
2. Dispatch bounded namespace/metadata work with `nvfio_execute(callback, arg)`.
   The callback receives the filesystem handle on its owning thread. Keep sync
   boundaries within journal credits; default geometry permits at most 4075
   distinct protected blocks, including metadata.
3. For asynchronous payload I/O, use `nvfio_client_alloc`, `nvfio_open`, DMA
   buffers and `nvfio_submit`/`nvfio_poll`. The runtime serializes mapping/metadata
   on one owner and retains the separate I/O reactor. Do not call filesystem
   operations concurrently from arbitrary application threads.
4. Drain and consume completions, sync, close descriptors, release requests/DMA
   buffers/clients, then call `nvfio_stop()`. Start another process for another
   runtime lifecycle; this reference is not restartable in the same process.

See [`metadata_bench.c`](../tests/spdk/metadata_bench.c) for owner callbacks,
[`fio_nvfuse.c`](../examples/fio_plugin/modern/fio_nvfuse.c) for asynchronous
submission/completion, and [primary journaling](spdk-primary-journaling.md) for
ordering and durability. Optional `nvfuse_tx_*` application transactions are a
separate standalone-runtime feature.

For buffered application adapters, `nvfio_execute_serialized(callback, arg)`
can run core filesystem calls on the application thread under the same exclusive
filesystem gate. SPDK bdev I/O still runs on its reactor. Callbacks must not call
SPDK thread/channel APIs or recursively enter synchronous runtime wrappers. Live
AIO clients automatically select the owner dispatch path, which fences accepted
pending submissions and in-flight I/O before the callback. The original
`nvfio_execute` remains available for callbacks that require the SPDK owner.

Use `nvfio_pread(fd, buffer, bytes, offset)` for buffered positional reads.
Already loaded pages can be copied under the gate; cold pages and active AIO
use the owner. Descriptor sizes are revalidated after serialized callbacks,
including truncate, close/reopen and AIO lifecycle changes. This uses the existing
buffer cache. `O_DIRECT` descriptors must use the AIO interface instead.
All callbacks and reads must finish before the last `nvfio_stop()`.

`NVFUSE_API_CACHE_READ=0` disables caller cache copies and
`NVFUSE_API_SIZE_EPOCH=0` disables descriptor-size validation reuse for diagnosis;
both default to `1`. `NVFUSE_API_GATE_SPIN_US` bounds spinning before blocking on the filesystem
serialization mutex (default `10`, range `0..1000`; `0` blocks immediately).
This trades a small amount of caller CPU time for fewer sleep/wakeup delays
under concurrent foreground and background DB operations. It does not change
filesystem serialization or sync durability.
`NVFUSE_API_SPIN_US` controls bounded spinning before an
owner-dispatched call sleeps (default `10`, range `0..1000` microseconds).
These options do not disable the journal, WAL writes or explicit sync barriers.

## Linux FUSE mounts

### Image-file mount without SPDK

Use a fresh image separate from the C example so its root ownership matches the
mounting user. Install libfuse3 dependencies, make `/dev/fuse` accessible, then:

```sh
make fuse
mkdir -p build/fuse/mount
./build/fuse/nvfuse-fuse --mkfs build/fuse/mount-demo.img --size-mib 256
./build/fuse/nvfuse-fuse --image build/fuse/mount-demo.img build/fuse/mount
```

The last command stays in the **foreground**. In another shell at the same
repository root:

```sh
printf 'hello from Linux\n' > build/fuse/mount/hello.txt
cat build/fuse/mount/hello.txt
ls -la build/fuse/mount
fusermount3 -u build/fuse/mount
```

Wait for the frontend to exit successfully. To reopen, run only the mount
command. Images must be outside the mount point. New image sizes must be
multiples of 128 MiB; `--mkfs` refuses an existing image path.

### Kernel block-device mount

After verifying an unused Linux block node, replace `/dev/APPROVED_UNUSED_DEVICE`
below with its real node path (resolve `/dev/disk/by-id` symlinks first).
This path retains the Linux NVMe/block driver and needs no SPDK setup:

```sh
make fuse
sudo mkdir -p /mnt/nvfuse
# DESTRUCTIVE: initialize the entire selected block node, once.
sudo ./build/fuse/nvfuse-fuse --backend kernel \
  --target /dev/APPROVED_UNUSED_DEVICE --mkfs --force
# Subsequent mounts reopen; omit --mkfs and --force.
sudo ./build/fuse/nvfuse-fuse --backend kernel \
  --target /dev/APPROVED_UNUSED_DEVICE /mnt/nvfuse
```

In a second shell, use `sudo` for clients of this root-owned mount, for example
`sudo ls -la /mnt/nvfuse`, then `sudo fusermount3 -u /mnt/nvfuse`. `--force`
authorizes the device format but does not bypass exclusive-open or geometry
checks. No `--size-mib` is needed to use the whole device.

### SPDK-backed mount

Prepare exclusive SPDK controller access and hugepages as above. Close the
primary runtime completely before opening the same volume through FUSE:

```sh
make fuse-spdk SPDK_ROOT_DIR="$SPDK_DIR"
sudo mkdir -p /mnt/nvfuse
sudo prlimit --core=0 --memlock=unlimited \
  ./examples/fuse3/nvfuse-fuse-spdk --backend spdk \
  --target "$NVFUSE_TEST_BDF" --nsid 1 /mnt/nvfuse
```

This **reopens** the existing filesystem. If initializing a new disposable
namespace through this frontend instead, omit the mount point and explicitly
add `--mkfs --force`; it then formats and exits. Do not format an existing
primary volume just to switch interfaces. Replace namespace 1 if appropriate.
Use root clients and `sudo fusermount3 -u /mnt/nvfuse`, then wait for successful
frontend exit before restarting the API. Driver rebinding is needed only when
switching between SPDK and a kernel-block-node backend, not between the two
SPDK interfaces.

The frontend uses one request thread, synchronous SPDK I/O and bounce buffers.
It supports basic file/directory/link/xattr operations, with 115-byte filename
components, 16-bit UID/GID and second-resolution timestamps. ACL/advisory locks,
full POSIX conformance, and macOS mounts are not provided. Read the
[compatibility contract](linux-fuse-integration.md#request-handling-and-semantics),
including `.fuse_hidden*` handling for deleted open files. Root mounts do not
implicitly allow other users (`allow_other` is disabled).

## Journal, errors and troubleshooting

| Symptom | Check/action |
| --- | --- |
| Existing image rejected during creation | Choose a new image path; reopen with format disabled to retain data. |
| Journal mode/version mismatch | Reopen in the stored mode; changing an environment variable is not a migration. |
| SPDK allocation/startup failure | Free hugepages, NUMA locality, memlock, available cores, configured bdev name, and exclusive controller ownership. |
| `spdk` backend unavailable | Use the SPDK-linked frontend; the ordinary FUSE executable does not link SPDK. |
| Permission denied on root FUSE mount | Run clients/unmount with the mounting user's privileges; root mounts normally require root clients. |
| FUSE mount detection differs by user | Inspect `findmnt` or `/proc/self/mountinfo`; stat-based detection can fail when FUSE denies that user's access. |
| No SPDK traffic in `iostat`/`lsblk` | The controller bypasses the kernel block path; use `nvfuse-stat` with runtime counters enabled. |
| Statistics-file collision | Select a new absolute `NVFUSE_SPDK_STATS` path; remove old records only after the writer has exited. |
| Commit failure, abort or unexpected process exit | Stop mutation, retain volume/logs, reopen the original journal volume for supported replay; never format as a recovery step. |
| Long format on a large device | NVFUSE initializes per-group metadata; sparse host-image allocation does not represent physical-device format cost. |

Primary full-ENOSPC and some legacy cache/allocation paths still contain
fail-fast assertions. Leave free space during exploratory workloads. Check
sync/close errors; ordinary write completion alone is not durable metadata or
an atomic application transaction. Qualification tasks are tracked in [TODO](../TODO),
and the [validation guide](testing-guide.md) explains what each test proves.
