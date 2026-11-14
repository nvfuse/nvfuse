/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_INODE_SPACE_ADAPTER_H
#define NVFUSE_INODE_SPACE_ADAPTER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Runs actual bitmap scan/free/counter functions on one real-size group. */
uint64_t inode_group_cycle(uint32_t group, uint32_t groups, unsigned reserved);
int inode_space_edges(void);
uint64_t inode_space_full(uint32_t groups);
#ifdef __cplusplus
}
#endif
#endif
