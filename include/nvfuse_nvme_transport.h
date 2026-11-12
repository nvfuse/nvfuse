#ifndef NVFUSE_NVME_TRANSPORT_H
#define NVFUSE_NVME_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
/* An opaque boundary keeps real SPDK headers separate from standalone env
 * adapters. The transport uses DMA bounce buffers and synchronous completion
 * polling; the core's malloc-backed cache is never passed to NVMe directly. */
int nvfuse_nvme_open(const char *bdf, void **device, uint64_t *size);
int nvfuse_nvme_transfer(void *device, uint64_t offset, size_t bytes, void *buffer, int write_op);
int nvfuse_nvme_flush(void *device);
int nvfuse_nvme_close(void *device);
#ifdef NVFUSE_NVME_TEST_HOOKS
/* Test binaries only: observe a real command immediately before/after submit. */
void nvfuse_nvme_test_observer(void (*observer)(int submitted, int write_op, uint64_t lba));
#endif
struct nvfuse_handle;
/* Experimental QEMU-only API. format=1 explicitly reformats the logical
 * filesystem area on the dedicated NVFUSEDEV0001 device. No physical SSDs. */
struct nvfuse_handle *nvfuse_nvme_fs_open(const char *bdf, int format,
                                        uint64_t logical_size, int metadata_only);
#endif
