#ifndef __NVFUSE_MISC_H__
#define __NVFUSE_MISC_H__

struct user_context {
	s64 file_size;
	s32 io_size;
	s32 qdepth;
	s32 is_read;
	s32 is_rand;

	s32 fd;
	s64 io_remaining;
	s64 io_curr;
	s8 *user_buf;
	s32 buf_ptr;
};

void nvfuse_rusage_diff(struct rusage *x, struct rusage *y, struct rusage *result);
void print_rusage(struct rusage *rusage, char *prefix, int divisor, double total_exec);
void nvfuse_rusage_add(struct rusage *x, struct rusage *result);

//s32 nvfuse_aio_test(struct nvfuse_handle *nvh, s32 direct);
void *nvfuse_aio_test_alloc_req(struct nvfuse_handle *nvh, void *_user_ctx);
void nvfuse_aio_test_callback(void *arg);
s32 nvfuse_metadata_test(struct nvfuse_handle *nvh, s8 *str, s32 meta_check, s32 count);
s32 nvfuse_fallocate_test(struct nvfuse_handle *nvh);
s32 nvfuse_type(struct nvfuse_handle *nvh, s8 *str);
void nvfuse_srand(long seed);

void nvfuse_rusage_diff(struct rusage *x, struct rusage *y, struct rusage *result);
void nvfuse_rusage_add(struct rusage *x, struct rusage *result);
void print_rusage(struct rusage *rusage, char *prefix, int divisor, double total_exec);
s64 nvfuse_rand(void);

#endif
