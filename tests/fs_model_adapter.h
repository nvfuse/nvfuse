#ifndef NVFUSE_FS_MODEL_ADAPTER_H
#define NVFUSE_FS_MODEL_ADAPTER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *model_open(const char *, int, int);
int model_close(void *);
int model_mkdir(void *, const char *);
int model_rmdir(void *, const char *);
int model_create(void *, const char *);
int model_unlink(void *, const char *);
int model_rename(void *, const char *, const char *);
int model_truncate(void *, const char *, uint64_t);
int model_write(void *, const char *, uint64_t, void *, unsigned);
int model_read(void *, const char *, void *, unsigned);
int model_stat(void *, const char *, uint64_t *, unsigned *);
int model_counts(void *, uint64_t *, uint64_t *);
/* Synthetic near-limit directory: only rejection is exercised, no namespace
 * is materialized. Restore the real inode before returning. */
int model_directory_limit(void *);
#ifdef __cplusplus
}
#endif
#endif
