#ifndef NVFUSE_FIO_RUNTIME_H
#define NVFUSE_FIO_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
struct nvfio_client;
struct nvfio_request;
struct nvfuse_handle;
/* Run a direct API workload on the existing serialized filesystem owner.
 * Submitted AIO is drained first; the callback controls its fsync policy. */
int nvfio_execute(int (*work)(struct nvfuse_handle *, void *), void *argument);
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
