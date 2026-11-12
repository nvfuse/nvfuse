#ifndef NVFUSE_INDIRECT_TEST_ADAPTER_H
#define NVFUSE_INDIRECT_TEST_ADAPTER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct indirect_test;
struct indirect_ops {
    void *context;
    void *(*get)(void *, uint32_t, int);
    void (*put)(void *, uint32_t, void *, int);
    uint32_t (*allocate)(void *, uint32_t *, uint32_t);
    void (*release)(void *, uint32_t, uint32_t);
    uint64_t (*available)(void *);
};
struct indirect_test *indirect_test_open(struct indirect_ops);
void indirect_test_close(struct indirect_test *);
int indirect_test_map(struct indirect_test *, int32_t, uint32_t, uint32_t *, uint32_t *, int);
void indirect_test_truncate(struct indirect_test *, uint64_t);
uint32_t indirect_test_root(struct indirect_test *, unsigned);
int indirect_test_path(int32_t, uint32_t[4], uint32_t *);
#ifdef __cplusplus
}
#endif
#endif
