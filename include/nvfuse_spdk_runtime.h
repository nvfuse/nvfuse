#ifndef NVFUSE_SPDK_RUNTIME_H
#define NVFUSE_SPDK_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
struct nvfio_client;
struct nvfio_request;
struct nvfuse_handle;
/* Run a direct API workload on the existing serialized filesystem owner.
 * Submitted AIO is drained first; the callback controls its fsync policy. */
int nvfio_execute(int (*work)(struct nvfuse_handle *, void *), void *argument);
/* Buffered positional read. Loaded cache pages may be copied by the caller
 * under the filesystem gate; cache misses and active AIO use the owner.
 * O_DIRECT descriptors use nvfio_submit instead and return -EINVAL here.
 * Calls must finish before the last nvfio_stop, as with nvfio_execute. */
int nvfio_pread(int fd, void *buffer, size_t bytes, uint64_t offset);
/* Core NVFUSE callbacks only; do not call SPDK thread/channel APIs or reenter
 * synchronous nvfio wrappers. Runs under the filesystem serialization gate on
 * the caller, or falls back to the owner while AIO clients are live. */
int nvfio_execute_serialized(int (*work)(struct nvfuse_handle *, void *), void *argument);
/* Buffered writes use the existing serialized executor and cache. Eligible
 * regular data may be written in the background; overlapping mutations and
 * metadata publication drain outstanding writes. No SPDK client AIO may overlap.
 * fsync drains these writes before the existing journal durability boundary. */
int nvfio_pwrite(int fd, const void *data, size_t bytes, uint64_t offset);
int nvfio_fsync(int fd);
#ifdef NVFIO_TEST
/* Deterministic ordering hooks; absent from ordinary runtime builds. */
int nvfio_test_buffered_wb_enabled(void);
int nvfio_test_buffered_wb_pending(void);
uint64_t nvfio_test_buffered_wb_offset(void);
int nvfio_test_buffered_wb_entered(void);
void nvfio_test_buffered_wb_pause(int pause);
int nvfio_test_api_pending(void);
/* Pause periodic publication for this process; explicit fsync still commits. */
int nvfio_test_pause_periodic_journal(void);
int nvfio_test_control_call(int (*work)(struct nvfuse_handle *, void *), void *argument);
#endif
int nvfio_start(const char *json, const char *bdf, int format);
struct nvfio_client *nvfio_client_alloc(unsigned queues);
int nvfio_client_free(struct nvfio_client *client);
int nvfio_open(const char *path, int create);
int nvfio_close(int fd);
int nvfio_sync(struct nvfio_client *client, int fd);
struct nvfio_request *nvfio_request_alloc(void *user);
void nvfio_request_free(struct nvfio_request *request);
void *nvfio_request_user(struct nvfio_request *request);
int nvfio_request_error(struct nvfio_request *request);
int nvfio_submit(struct nvfio_client *client, struct nvfio_request *request, int fd, void *buf, size_t size, uint64_t offset, int write);
int nvfio_poll(struct nvfio_client *client, unsigned max, struct nvfio_request **completed);
void *nvfio_dma_alloc(size_t size);
void nvfio_dma_free(void *buffer);
int nvfio_stop(void);
#endif
