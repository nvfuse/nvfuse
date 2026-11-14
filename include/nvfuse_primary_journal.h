/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVFUSE_PRIMARY_JOURNAL_H
#define NVFUSE_PRIMARY_JOURNAL_H
#include "nvfuse_journal.h"
struct nvfuse_handle;
struct io_target;
struct io_job;
int nvfuse_primary_journal_prepare(struct nvfuse_handle *, int formatted);
int nvfuse_primary_journal_before_format(struct io_target *);
/* Caller serializes all filesystem operations and drains AIO first. */
int nvfuse_primary_journal_commit(struct nvfuse_handle *, int restart);
int nvfuse_primary_journal_stage(struct io_target *, struct io_job *);
/* Serialized owner only. Return1 when copied/read entirely by the overlay,
 * 0 for ordinary device I/O. Never publish an owner-produced CQ completion. */
int nvfuse_primary_journal_sync(struct io_target *, int64_t offset, size_t bytes,
                                void *buffer, unsigned type);
/* All-or-reference collection: return0 without staging if any image requires
 * device I/O. The caller retains stable buffer references until this returns. */
int nvfuse_primary_journal_collect(struct io_target *, const struct nvj_home_write *, unsigned);
int nvfuse_primary_journal_read_header(struct io_target *, void *, size_t);
void nvfuse_primary_journal_release(struct io_target *);
#endif
