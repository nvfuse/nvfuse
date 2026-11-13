# Interfaces and storage backends

**API + SPDK is the primary performance configuration.** The other combinations
provide ordinary application access, compatibility, development and testing.
Adding them must not introduce a common mutex, extra data copy, FUSE dependency,
or request-thread handoff into the existing API/SPDK reactor path.

The interface and storage axes are independent:

| Interface | Storage backend | Role and implementation |
| --- | --- | --- |
| API | SPDK | Primary performance path: existing direct API/SPDK engine. The standalone synchronous NVMe transport also supports integration/compatibility tests. |
| FUSE 3 | SPDK | Linux compatibility mount through the standalone synchronous NVMe transport |
| API | Kernel driver | Linux block node, exclusive open, buffered pread/pwrite and fsync |
| FUSE 3 | Kernel driver | Linux compatibility mount on the same block-node backend |
| API | Image file | Existing macOS/Linux development backend |
| FUSE 3 | Image file | Linux compatibility mount, including the QEMU development setup |

FUSE support is Linux-only. Native macOS image API tests remain supported;
macOS mounting is outside this implementation. `kernel` means that NVFUSE is
still a userspace filesystem, but block I/O passes through a Linux block-device
node such as `/dev/nvme0n1` or `/dev/loop0`. It does not install a kernel
filesystem driver. SPDK bypasses the kernel block driver for the selected PCI
NVMe controller.

## Shared core and performance boundary

The optional opener in [`nvfuse_backend.h`](../include/nvfuse_backend.h) chooses
storage at open time, then returns the existing `struct nvfuse_handle *`.
Applications continue calling the existing filesystem APIs. Backend selection
does not wrap every API operation or install a new scheduling layer.

The journal, transactions, filesystem algorithms and disk geometry remain
shared. The existing file API and fixture-only SPDK test API remain available.
The kernel-device opener reuses the existing descriptor I/O and journal path;
geometry discovery is factored into an open-time helper. It exclusively claims
the block node with `O_EXCL` and gets capacity with `BLKGETSIZE64`. It rejects
regular files and kernel-mounted/claimed devices, rather than treating an
advisory file lock as exclusive device ownership.

The primary SPDK reactor build remains separate. New FUSE/SPDK mounts
use the **existing standalone integration transport**, not that reactor: one
request owner thread, one NVMe queue, synchronous completion polling and DMA
bounce buffers. Selecting `spdk` through the new standalone opener uses this
same synchronous transport. That opener is useful for portability/integration;
it is not a claim of a new high-performance SPDK API engine. Performance work
on the primary API/SPDK engine remains independent of these compatibility
builds. No new FUSE/kernel/image requirement is imposed on its callers.

The generic NVMe opener selects a PCI BDF and namespace ID explicitly. It
supports conventional NVM namespaces with 512-byte or 4096-byte logical sectors
and no separate metadata. The pre-existing test opener still rejects anything
other than the exact disposable QEMU fixture. A process initializes SPDK for
one PCI controller; reopening another controller in that process returns
`EXDEV`. The generic opener does not bind drivers, allocate system hugepages,
or change VFIO permissions automatically.

The standalone runtime still permits **one filesystem handle per process**,
with all calls and close on the opening thread. FUSE stays foreground and
single-threaded to honor that contract. API and FUSE can open the same volume
sequentially; concurrent writers through the two interfaces or through aliasing
image/device paths are unsupported. Do not open a loop device's backing file
as an independent image while the loop device is in use.

## Builds

```sh
# Standalone API library: image files on macOS/Linux; kernel devices on Linux.
make -f Makefile.local

# Linux compatibility mount: image files and kernel devices; no SPDK required.
make fuse

# Additional Linux compatibility executable linked with modern SPDK.
# Requires an already built SPDK checkout and libfuse3 development headers.
make fuse-spdk SPDK_ROOT_DIR="$HOME/src/spdk"
```

The ordinary frontend is `build/fuse/nvfuse-fuse`. The optional SPDK-linked
frontend is `examples/fuse3/nvfuse-fuse-spdk`, with a separate core archive in
`build/fuse-spdk`. It accepts all three backends. Builds without SPDK return
`ENOTSUP` from the public opener and a clear frontend error for `spdk`.

For an API application linked to the standalone runtime:

```c
#include "nvfuse_backend.h"

struct nvfuse_backend_options options = {
    .kind = NVFUSE_BACKEND_KERNEL,
    .target = "/dev/loop0",
    .format = 0,                 /* Reopen only; never format implicitly. */
};
struct nvfuse_handle *fs = nvfuse_backend_open(&options);
if (!fs) {
    /* errno identifies invalid options, ownership conflicts or I/O errors. */
    return 1;
}
/* Use the existing nvfuse_* filesystem and transaction APIs here. */
int result = nvfuse_backend_close(fs);
```

For an image use `NVFUSE_BACKEND_IMAGEFILE` and a regular-file path. For the
standalone SPDK transport use `NVFUSE_BACKEND_SPDK`, a BDF such as
`0000:01:00.0`, and `.namespace_id = 1` (zero defaults to 1). The latter requires
the SPDK-linked core, transport object and SPDK libraries. The optional
[`Makefile.spdk`](../examples/fuse3/Makefile.spdk) and its `backend-test` target
show the working compiler/linker configuration. The original performance API
applications retain their existing SPDK initialization/build path.

For devices, `.format = 1, .size_bytes = 0` uses the full capacity rounded
to complete 128 MiB block groups. A nonzero `.size_bytes` explicitly bounds the
format area and must not exceed capacity. New image files require an explicit
size and a new path. Reopen discovers stored logical geometry, which may be smaller than physical
capacity, and requires `.size_bytes = 0`. Image files are never truncated by a
reopen. API format callers set root ownership/mode for their application; the
FUSE format command sets the invoking UID/GID and mode 0755.

## Mount and format commands

The existing image commands remain valid. The common syntax is:

```sh
mkdir -p /tmp/nvfuse-mount
build/fuse/nvfuse-fuse --backend imagefile --target build/dev.img /tmp/nvfuse-mount
build/fuse/nvfuse-fuse --backend kernel --target /dev/loop0 /tmp/nvfuse-mount
examples/fuse3/nvfuse-fuse-spdk --backend spdk --target 0000:01:00.0 \
    --nsid 1 /tmp/nvfuse-mount
fusermount3 -u /tmp/nvfuse-mount
```

Device access and SPDK environment setup must already permit the invoking user
to access the selected target. Root mounts normally need root clients under
FUSE's default access restrictions; `allow_other` is not enabled.

Mounting never formats. Formatting is a separate command:

```sh
build/fuse/nvfuse-fuse --backend imagefile --target build/new.img \
    --mkfs --size-mib 256
# The following examples overwrite the explicitly selected disposable device.
build/fuse/nvfuse-fuse --backend kernel --target /dev/loop0 \
    --mkfs --size-mib 256 --force
examples/fuse3/nvfuse-fuse-spdk --backend spdk --target 0000:01:00.0 \
    --nsid 1 --mkfs --size-mib 256 --force
```

`--force` is required for device formats. Its absence is rejected before an
open or any device I/O. It does not bypass `O_EXCL`, driver ownership, geometry,
or build restrictions. Kernel targets use a real block node, not a symlink,
for the public API/format operation; resolve stable device aliases first.
SPDK ownership and kernel ownership are mutually exclusive for a controller.
The frontend never changes those bindings by itself.

## Compatibility and limits

The standalone transport remains synchronous and the mounted POSIX limits in
[Linux FUSE integration](linux-fuse-integration.md) apply. Compatibility mounts
do not establish the performance or concurrency contract of the primary API/SPDK
runtime. Conventional NVM namespaces, one thread-affine filesystem owner and
exclusive controller/device access are required; SPDK-oF is outside this opener.

The regression entry points are `make backend-regression` for portable Linux
image/kernel combinations and `make backend-regression-qemu` for all six
combinations in the dedicated QEMU environment. Results and failed fixtures are
written under `build/test-results/`, outside versioned design documentation.

## Whole-device formatting

For kernel and generic SPDK device openers, `.format = 1, .size_bytes = 0`
discovers the selected device's capacity and uses all complete 128 MiB block
groups. Reopen (`.format = 0, .size_bytes = 0`) discovers the existing filesystem
geometry and never formats. Image creation still requires an explicit size.
A nonzero device size remains available to bound disposable test fixtures.
The primary API/SPDK fio configuration selects the full namespace directly;
see [primary SPDK fio](spdk-fio.md).

For example, on an explicitly approved unused device:

```sh
# Destroys the existing contents of the selected device.
sudo build/fuse/nvfuse-fuse --backend kernel --target /dev/APPROVED_DEVICE --mkfs --force
sudo build/fuse/nvfuse-fuse --backend kernel --target /dev/APPROVED_DEVICE /mnt/nvfuse
```

No `--size-mib` is needed for a device. The legacy block mapper is limited to
approximately 8 TiB; larger device formats are rejected rather than truncated
to an arbitrary small test area.
