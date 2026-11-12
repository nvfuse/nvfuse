#ifndef NVFUSE_FILE_H
#define NVFUSE_FILE_H
#include <stdint.h>
struct nvfuse_handle;
/* create=true requires a new path; size must be a multiple of 128 MiB.
 * One handle per process; all filesystem calls must run on the calling thread. */
struct nvfuse_handle *nvfuse_file_open(const char *path, int create, uint64_t size);
/* Test-only format: persist filesystem metadata, discard regular-file contents.
 * Images carry a distinct signature and cannot be opened in the other mode.
 * Only synchronous buffered file operations are supported in this mode. */
struct nvfuse_handle *nvfuse_file_open_mode(const char *path, int create,
                                         uint64_t size, int metadata_only);
/* Unmount, sync and release the image. Returns zero on success. */
int nvfuse_file_close(struct nvfuse_handle *nvh);
#endif
