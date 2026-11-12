#ifndef NVFUSE_FILE_H
#define NVFUSE_FILE_H
#include <stdint.h>
struct nvfuse_handle;
/* create=true requires a new path; size must be a multiple of 128 MiB.
 * One handle per process; all filesystem calls must run on the calling thread. */
struct nvfuse_handle *nvfuse_file_open(const char *path, int create, uint64_t size);
/* Unmount, sync and release the image. Returns zero on success. */
int nvfuse_file_close(struct nvfuse_handle *nvh);
#endif
