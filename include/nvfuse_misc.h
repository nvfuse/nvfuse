#ifndef __NVFUSE_MISC_H__
#define __NVFUSE_MISC_H__
void nvfuse_rusage_diff(struct rusage *x, struct rusage *y, struct rusage *result);
void print_rusage(struct rusage *rusage, char *prefix, int divisor, double total_exec);
void nvfuse_rusage_add(struct rusage *x, struct rusage *result);
#endif
