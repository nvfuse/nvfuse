/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_BACKEND_H
#define NVFUSE_BACKEND_H
#include <stdint.h>
struct nvfuse_handle;

/* Optional standalone opener. It does not wrap filesystem operations or the
 * original SPDK reactor. API + SPDK remains the primary performance path;
 * mounted/kernel/image modes prioritize compatibility and testability. */
enum nvfuse_backend_kind {
    NVFUSE_BACKEND_IMAGEFILE = 0,
    NVFUSE_BACKEND_KERNEL = 1,
    NVFUSE_BACKEND_SPDK = 2
};
struct nvfuse_backend_options {
    enum nvfuse_backend_kind kind;
    const char *target;          /* regular file, Linux block node, or PCI BDF */
    int format;                  /* explicit destructive format for devices */
    uint64_t size_bytes;          /* device format: zero uses whole device;
                                    image/test limit: multiple of 128 MiB;
                                    reopen: zero discovers existing geometry */
    uint32_t namespace_id;        /* SPDK only; zero selects namespace 1 */
};
/* One handle per process, all calls including close on its opening thread.
 * Reopen never formats. Image formatting requires a new path. Device formatting
 * defaults to the whole device (rounded to 128 MiB block groups); a nonzero
 * size explicitly bounds a test volume. Kernel devices are exclusively claimed
 * with O_EXCL; SPDK requires an already configured userspace PCI driver.
 * Returns NULL with errno set. Unsupported build/platform returns ENOTSUP. */
int nvfuse_backend_supported(enum nvfuse_backend_kind kind);
struct nvfuse_handle *nvfuse_backend_open(const struct nvfuse_backend_options *options);
int nvfuse_backend_close(struct nvfuse_handle *handle);
#endif
