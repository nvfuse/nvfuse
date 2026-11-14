/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_FILE_H
#define NVFUSE_FILE_H
#include <stdint.h>
struct nvfuse_handle;
/* New images use the standalone redo-journal format.
 * create=true requires a new path; size must be a multiple of 128 MiB.
 * One handle per process; all filesystem calls must run on the calling thread. */
struct nvfuse_handle *nvfuse_file_open(const char *path, int create, uint64_t size);
/* Test-only format: persist filesystem metadata, discard regular-file contents.
 * Images carry a distinct signature and cannot be opened in the other mode.
 * Only synchronous buffered file operations are supported in this mode. */
struct nvfuse_handle *nvfuse_file_open_mode(const char *path, int create,
                                         uint64_t size, int metadata_only);
/* New version-2 images may reserve 4096/8192/16384 journal blocks (16/32/64MiB).
 * Reopen discovers geometry from the anchor; no existing image is resized. */
struct nvfuse_handle *nvfuse_file_open_config(const char *,int create,uint64_t size,
                                            int metadata_only,unsigned journal_blocks);
/* Unmount, sync and release the image. Returns zero on success.
 * An active healthy user TX returns -EBUSY and retains the handle.
 * Closing a poisoned handle discards caches, releases it and returns -EIO. */
int nvfuse_file_close(struct nvfuse_handle *nvh);
#endif
