/* SPDX-License-Identifier: GPL-2.0-only */

#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_tx_internal.h"

struct snapshot {
    struct nvfuse_file_table files[MAX_OPEN_FILE];
    uint32_t cwd, root_dir, root;
};
static int supported(struct nvfuse_handle *h) {
    if(!h || !h->nvh_target || !h->nvh_target->journal) return -EOPNOTSUPP;
    if(!pthread_equal(h->nvh_target->tx_thread,pthread_self())) return -EPERM;
    if(nvj_recovery_required(h->nvh_target->journal)) return -EIO;
    return 0;
}
static int save(struct nvfuse_handle *h) {
    struct io_target *t=h->nvh_target;
    if(!t->tx_snapshot) t->tx_snapshot=malloc(sizeof(struct snapshot));
    if(!t->tx_snapshot) return -ENOMEM;
    struct snapshot *s=t->tx_snapshot;
    memcpy(s->files,h->nvh_sb.sb_file_table,sizeof(s->files));
    s->cwd=h->nvh_cwd_ino; s->root_dir=h->nvh_root_dir; s->root=h->nvh_root_ino;
    return 0;
}
static int rollback(struct nvfuse_handle *h) {
    struct snapshot *s=h->nvh_target->tx_snapshot;
    int rc=nvfuse_file_reload(h);
    if(rc) return rc;
    if(s) {
        for(unsigned i=0;i<MAX_OPEN_FILE;i++) {
            struct nvfuse_file_table *f=h->nvh_sb.sb_file_table+i;
            f->ino=s->files[i].ino; f->size=s->files[i].size; f->used=s->files[i].used;
            f->rwoffset=s->files[i].rwoffset; f->flags=s->files[i].flags;
        }
        h->nvh_cwd_ino=s->cwd; h->nvh_root_dir=s->root_dir; h->nvh_root_ino=s->root;
    }
    return 0;
}
static void collect(struct nvfuse_handle *h) {
    nvfuse_check_flush_dirty(&h->nvh_sb,DIRTY_FLUSH_FORCE);
    int rc=nvfuse_sync_superblock(&h->nvh_sb);
    if(rc) nvj_fail(h->nvh_target->journal,rc<0?rc:-EIO);
}
int nvfuse_tx_enter(struct nvfuse_handle *h) {
    if(!h || !h->nvh_target) return -EINVAL;
    struct io_target *t=h->nvh_target;
    if(!t->journal) return 0;
    int rc=supported(h); if(rc) return rc;
    if(nvj_error(t->journal)) return -ECANCELED;
    rc=nvj_prepare_metadata(t->journal,nvj_capacity(t->journal));
    if(rc) { if(t->tx_user) nvj_fail(t->journal,rc); return rc; }
    if(!t->tx_depth && !t->tx_user) {
        rc=save(h); if(rc) return rc;
        rc=nvj_begin(t->journal,0,0); if(rc) return rc;
    }
    t->tx_depth++; return 0;
}
static int leave(struct nvfuse_handle *h,int result,int lookup) {
    struct io_target *t=h->nvh_target;
    if(!t->journal) return result;
    if(result<0 && !(lookup && (result==-ENOENT || result==-ENOTDIR || result==-ENODATA || result==-ERANGE)))
        nvj_fail(t->journal,result);
    if(--t->tx_depth) return result;
    collect(h);
    int rc=nvj_error(t->journal);
    if(t->tx_user) return rc?rc:result;
    if(rc) {
        int undo=rollback(h);
        return undo?undo:rc;
    }
    rc=nvj_commit(t->journal);
    if(rc && !nvj_recovery_required(t->journal)) {
        int undo=rollback(h); if(undo) return undo;
    }
    if(!rc) rc=nvfuse_orphan_reap(h);
    return rc?rc:result;
}
int nvfuse_tx_leave(struct nvfuse_handle *h,int result) { return leave(h,result,0); }
int nvfuse_tx_leave_lookup(struct nvfuse_handle *h,int result) { return leave(h,result,1); }
int nvfuse_tx_begin(struct nvfuse_handle *h,enum nvfuse_tx_mode mode,unsigned budget) {
    int rc=supported(h); if(rc) return rc;
    struct io_target *t=h->nvh_target;
    if(t->tx_user || t->tx_depth || t->tx_queues) return -EBUSY;
    if(mode!=NVFUSE_TX_METADATA && mode!=NVFUSE_TX_DATA) return -EINVAL;
    if(t->metadata_only && mode==NVFUSE_TX_DATA) return -EOPNOTSUPP;
    if(budget>nvj_capacity(t->journal)) return -EINVAL;
    for(unsigned i=START_OPEN_FILE;i<MAX_OPEN_FILE;i++)
        if(h->nvh_sb.sb_file_table[i].used) return -EBUSY;
    rc=save(h); if(rc) return rc;
    rc=nvj_begin(t->journal,mode==NVFUSE_TX_DATA,budget);
    if(!rc) t->tx_user=1;
    return rc;
}
int nvfuse_tx_commit(struct nvfuse_handle *h) {
    int rc=supported(h); if(rc) return rc;
    struct io_target *t=h->nvh_target;
    if(!t->tx_user || t->tx_depth) return -EINVAL;
    collect(h);
    rc=nvj_error(t->journal);
    if(rc) return rc; /* Abort-only, explicit abort is still required. */
    rc=nvj_commit(t->journal);
    if(!rc) { t->tx_user=0; t->tx_keyed=0; rc=nvfuse_orphan_reap(h); }
    return rc;
}
int nvfuse_tx_abort(struct nvfuse_handle *h) {
    int rc=supported(h); if(rc) return rc;
    if(!h->nvh_target->tx_user || h->nvh_target->tx_depth) return -EINVAL;
    return rollback(h);
}
int nvfuse_tx_status(struct nvfuse_handle *h) {
    if(!h || !h->nvh_target || !h->nvh_target->journal) return -EOPNOTSUPP;
    struct io_target *t=h->nvh_target;
    if(!pthread_equal(t->tx_thread,pthread_self())) return -EPERM;
    if(nvj_recovery_required(t->journal)) return NVFUSE_TX_RECOVERY_REQUIRED;
    if(!t->tx_user) return NVFUSE_TX_IDLE;
    return nvj_error(t->journal)?NVFUSE_TX_ABORT_ONLY:NVFUSE_TX_ACTIVE;
}
int nvfuse_tx_get_stats(struct nvfuse_handle *h,struct nvj_stats *out) {
    int rc=supported(h); if(rc) return rc;
    if(!out) return -EINVAL;
    *out=*nvj_stats(h->nvh_target->journal); return 0;
}
int nvfuse_tx_set_hook(struct nvfuse_handle *h,nvj_hook hook,void *arg) {
    int rc=supported(h); if(rc) return rc;
    nvj_set_hook(h->nvh_target->journal,hook,arg); return 0;
}

int nvfuse_tx_set_async_checkpoint(struct nvfuse_handle *h,int enabled) {
    int rc=supported(h);if(rc) return rc;
    if(h->nvh_target->spdk_transport) return -EOPNOTSUPP;
    if(h->nvh_target->tx_user || h->nvh_target->tx_depth) return -EBUSY;
    return nvj_set_async_checkpoint(h->nvh_target->journal,enabled);
}

/* Queue groups retain pre-existing descriptors in the same snapshot machinery.
 * This internal entry point does not change the public user-TX begin contract. */
int nvfuse_tx_begin_group(struct nvfuse_handle *h,enum nvfuse_tx_mode mode) {
    int rc=supported(h);if(rc) return rc;
    struct io_target *t=h->nvh_target;
    if(t->tx_user || t->tx_depth) return -EBUSY;
    if(mode!=NVFUSE_TX_METADATA && mode!=NVFUSE_TX_DATA) return -EINVAL;
    if(t->metadata_only && mode==NVFUSE_TX_DATA) return -EOPNOTSUPP;
    rc=save(h);if(rc) return rc;
    rc=nvj_begin(t->journal,mode==NVFUSE_TX_DATA,0);
    if(!rc) t->tx_user=1;
    return rc;
}
