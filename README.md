# NVFUSE

NVFUSE is a userspace filesystem that applications can embed as a C library.
Its primary performance configuration is **the direct API with SPDK**. An
optional Linux FUSE 3 frontend provides mounted access for ordinary programs;
kernel block-device and image-file backends support compatibility and development.

The shared core retains B+tree directory indexes, an inode and indirect-block
layout, 128 MiB block groups, buffer/inode caches, and control-plane components.
Metadata redo journaling defaults **ON** in the modern API/SPDK runtime. New
journal formats default to v2; v3 is an explicitly selected experimental format.

## Start here

- [User guide](docs/user-guide.md): prerequisites, builds, a working C example,
  SPDK setup, first format, reopen, FUSE mounts, monitoring, and troubleshooting.
- [Validation guide](docs/testing-guide.md): local checks, backend regression,
  fio, metadata/application workloads, fault testing, and qualification limits.
- [Interface/backend matrix](docs/backend-matrix.md): implementation and ownership
  contracts for all six combinations.
- [Documentation index](docs/README.md): operational guides and core design references.
- [Release notes](CHANGELOG.md): prerelease scope, format compatibility and known limits.

## Choose an interface and backend

| Interface | SPDK NVMe — Linux | Kernel block device — Linux | Image file |
| --- | --- | --- | --- |
| Direct C API | **Primary asynchronous reactor**; also a separate synchronous compatibility transport | Standalone compatibility library | Standalone development library on macOS/Linux |
| FUSE 3 mount | Synchronous compatibility transport | Compatibility mount | Development/compatibility mount on Linux |

Selecting SPDK in the standalone `nvfuse_backend_open()` API uses the synchronous
transport. It does **not** select the primary asynchronous reactor. See the
[user guide](docs/user-guide.md#embed-the-primary-apispdk-runtime) for integration.

Use one exclusive filesystem owner per volume. API and FUSE can use the same
volume sequentially after clean close/unmount; simultaneous ownership is
unsupported. Native macOS image APIs work without FUSE. macOS/Finder mounting
is outside the current implementation.

## Quick start: no device or root required

Requirements: macOS or Linux, a C compiler (`clang` by default), Make, and
Python 3.9+. On macOS, install Xcode Command Line Tools.
Run commands from the repository root:

```sh
make local
make local-test

cc -std=gnu11 -D_GNU_SOURCE -DNVFUSE_FILE_BACKEND -Iinclude \
  examples/quickstart/image_api.c build/local/nvfuse.a -lpthread -lm \
  -o build/local/image-api

# Creation requires a NEW path. Reuse that exact path for the separate read.
./build/local/image-api create build/hello.img
./build/local/image-api read build/hello.img
```

The example creates a 256 MiB filesystem image, writes and syncs `/hello.txt`,
then reads and verifies it in a separate process. This is a functional starting
point; it does not measure NVMe performance. Repeated creation at an existing
path is rejected. See [the source](examples/quickstart/image_api.c).

## Primary API/SPDK and Linux mounts

Linux x86_64 primary runtime: build SPDK **v26.09** and fio **3.41** first, then:

```sh
make -f Makefile.spdk fio \
  SPDK_ROOT_DIR="$HOME/src/spdk" FIO_SOURCE_DIR="$HOME/src/fio"
```

This builds `build/spdk-primary/nvfuse.a` and
`examples/fio_plugin/modern/nvfuse_spdk.so`. It accesses NVFUSE files directly
without a mounted directory. Follow the [device preparation and first-run
instructions](docs/user-guide.md#primary-apispdk-on-linux) before running it.
Formatting is explicit and destroys the selected volume's contents.

Linux compatibility mounts:

```sh
make fuse   # Requires libfuse3 headers and /dev/fuse; image/kernel backends.
make fuse-spdk SPDK_ROOT_DIR="$HOME/src/spdk"  # Optional SPDK-linked mount.
```

Follow the [mount walkthrough](docs/user-guide.md#linux-fuse-mounts) for complete
commands. Device formats use the whole selected namespace/node, rounded down to
complete 128 MiB block groups; image creation requires an explicit size.

## Durability and current limits

Successful write completion alone does not guarantee persistence. Use explicit
sync boundaries and check their results. Primary fsync/fdatasync currently
commit filesystem-wide; periodic commits default to 100 ms and are not a hard
durability deadline. Recovery reopens the original journal volume without
formatting. See [primary journaling](docs/spdk-primary-journaling.md).

Optional application metadata/DATA transactions belong to the standalone
runtime and have a separate [API contract](docs/user-transactions-design.md).
They must not be inferred from primary metadata journaling. FUSE provides basic
POSIX compatibility, with [documented limits](docs/linux-fuse-integration.md#compatibility-limits),
rather than complete POSIX conformance. Some legacy allocation/cache failures
still abort the process; primary full-ENOSPC handling remains qualification work.
The current physical-cluster mapper rejects device formats above approximately
8 TiB. Physical power-loss durability and production qualification remain
separate from functional tests and process-exit recovery checks.

Run-specific logs, benchmark outputs, and temporary reports belong under
`build/test-results/`. See [TODO](TODO) for remaining qualification work.
No fixed XFS performance parity claim applies to every workload or durability mode.

## Performance snapshot

Measured at optimization checkpoint `6da7672` on Linux using the same 7.68 TB
NVMe namespace sequentially for XFS and NVFUSE API + SPDK, with metadata
journaling **ON, experimental v3**. Values are medians of three trials; these
are development measurements, not results for the default v2 format or FUSE.

| Workload | NVFUSE API + SPDK | XFS | NVFUSE vs. XFS |
| --- | ---: | ---: | ---: |
| fio 4 KiB random read | 1.048 M IOPS | 1.370 M IOPS | −23.5% |
| fio 4 KiB random write | 1.265 M IOPS | 1.087 M IOPS | +16.4% |
| fio 256 KiB sequential read | 6,735 MiB/s | 6,814 MiB/s | −1.2% |
| fio 256 KiB sequential write | 6,071 MiB/s | 5,836 MiB/s | +4.0% |
| LevelDB 1.23 single-key sync update | 35,430 ops/s | 32,250 ops/s | +9.9% |
| RocksDB 9.10.0 single-key sync update | 37,870 ops/s | 30,708 ops/s | +23.3% |

fio 3.41 used `direct=1`, four jobs, depth 32 per job, 20 GiB per file,
10-second trials and `end_fsync=1`; writes were not individually synced.
DB tests used prototype NVFUSE Env adapters versus native XFS Env, 100,000 keys,
1 KiB values, WAL enabled and `sync=true` per write/batch. Their datasets fit in
filesystem caches; 100-key batched updates were approximately 7% slower on
NVFUSE for both databases.

Both paths had four application CPUs; NVFUSE additionally used two runtime
cores. XFS ran first, and another device was under soak load. Cache budgets
differed (NVFUSE 512 MiB; XFS page cache unrestricted), so this is a complete
access-path comparison, not an equal-CPU/cache efficiency claim. See the
[validation guide](docs/testing-guide.md#primary-fio-and-metadata-measurements)
for measurement requirements. Physical power-loss durability was not tested.

## Repository layout

| Directory | Contents |
| --- | --- |
| `src/` | Filesystem core, backends, journaling and primary SPDK runtime |
| `src/spdk_runtime/` | Private SPDK runtime queue, cache, completion and writeback helpers |
| `include/` | Library headers |
| `examples/` | Application examples, fio adapter and Linux FUSE frontend |
| `tests/` | Regression, recovery and benchmark tools |
| `scripts/` | Build-environment, monitoring and test helpers |
| `docs/` | User guides and core design documents |
| `build/` | Generated build products and local test results |

Build entry points remain at the repository root. Libraries still build to
`build/local/nvfuse.a` and `build/spdk-primary/nvfuse.a`.

## Historical examples

Plain `make`, `scripts/setup.sh`, `scripts/disable_aslr.sh`, and the old
`examples/libfuse` / `examples/fio_plugin` integration belong to the legacy
build. They are not the modern installation procedure. In particular, the old
setup script can rebind multiple controllers; do not use it as a shared-host
quick start. Current entry points are the explicit local, FUSE, and
`Makefile.spdk` targets above.

## License

NVFUSE's project license remains **GNU GPL version 2 only**. See
[COPYING](COPYING) for the full license and [NOTICE](NOTICE) for third-party
attributions and file-specific terms. The e2fsprogs-derived hash and bitmap copies use
the GPL v2 option explicitly permitted by their original LGPL v2 license;
[COPYING.LIB](COPYING.LIB) retains that original text for provenance.

Commercial use is permitted under the applicable license conditions.
Applications linking NVFUSE must evaluate the GPL requirements for their
combined distribution; NVFUSE does not grant a proprietary-linking exception.

## Maintainer

Yongseok Oh — [yongseok.oh@linecorp.com](mailto:yongseok.oh@linecorp.com)

Historical copyright notices and contributor credits are preserved. See
[NOTICE](NOTICE) for attribution and the distinction between maintainer contact
and copyright ownership.
