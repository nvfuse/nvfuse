/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_TEST_BACKEND_H
#define NVFUSE_TEST_BACKEND_H
#include "nvfuse_file.h"
#ifdef NVFUSE_SPDK_TRANSPORT
#include "nvfuse_nvme_transport.h"
static inline struct nvfuse_handle *test_open_mode(const char *path, int create,
                                                   uint64_t size, int metadata_only)
{
    if (strncmp(path, "pcie:", 5)) { errno = EINVAL; return NULL; }
    return nvfuse_nvme_fs_open(path + 5, create, size, metadata_only);
}
static inline struct nvfuse_handle *test_open(const char *path, int create, uint64_t size)
{
    return test_open_mode(path, create, size, 0);
}
#define nvfuse_file_open test_open
#define nvfuse_file_open_mode test_open_mode
#endif
#endif
