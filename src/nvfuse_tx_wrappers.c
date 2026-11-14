/* SPDX-License-Identifier: GPL-2.0-only */

/* Public standalone operation boundaries. Nested implementation calls use
 * the same transaction through nvfuse_tx_dispatch.h. */
#include "nvfuse_portable.h"
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_tx_internal.h"
#include <fcntl.h>
static int valid_fd(struct nvfuse_handle *h,unsigned fd) {
    return fd>=START_OPEN_FILE && fd<MAX_OPEN_FILE && h->nvh_sb.sb_file_table[fd].used;
}
extern s32 nvfuse_opendir_impl(struct nvfuse_handle *nvh, const char *path);
s32 nvfuse_opendir(struct nvfuse_handle *nvh, const char *path) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_opendir_impl(nvh, path);
    return nvfuse_tx_leave_lookup(h,result);
}
extern s32 nvfuse_openfile_impl(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, s32 flags,
		    s32 mode);
s32 nvfuse_openfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, s32 flags,
		    s32 mode) {
    struct nvfuse_handle *h=sb->sb_nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_openfile_impl(sb, par_ino, filename, flags, mode);
    return (flags&(O_CREAT|O_TRUNC))?nvfuse_tx_leave(h,result):nvfuse_tx_leave_lookup(h,result);
}
extern s32 nvfuse_openfile_path_impl(struct nvfuse_handle *nvh, const char *path, int flags, int mode);
s32 nvfuse_openfile_path(struct nvfuse_handle *nvh, const char *path, int flags, int mode) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_openfile_path_impl(nvh, path, flags, mode);
    return (flags&(O_CREAT|O_TRUNC))?nvfuse_tx_leave(h,result):nvfuse_tx_leave_lookup(h,result);
}
extern s32 nvfuse_openfile_ino_impl(struct nvfuse_superblock *sb, inode_t ino, s32 flags);
s32 nvfuse_openfile_ino(struct nvfuse_superblock *sb, inode_t ino, s32 flags) {
    struct nvfuse_handle *h=sb->sb_nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_openfile_ino_impl(sb, ino, flags);
    return (flags&(O_CREAT|O_TRUNC))?nvfuse_tx_leave(h,result):nvfuse_tx_leave_lookup(h,result);
}
extern s32 nvfuse_closefile_impl(struct nvfuse_handle *nvh, s32 fid);
s32 nvfuse_closefile(struct nvfuse_handle *nvh, s32 fid) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    if(!valid_fd(h,fid)) return nvfuse_tx_leave(h,-EBADF);
    int result=nvfuse_closefile_impl(nvh, fid);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_readfile_impl(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset);
s32 nvfuse_readfile(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count, nvfuse_off_t roffset) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    if(!valid_fd(h,fid)) return nvfuse_tx_leave(h,-EBADF);
    int result=nvfuse_readfile_impl(nvh, fid, buffer, count, roffset);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_readfile_aio_impl(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count,
			nvfuse_off_t roffset);
s32 nvfuse_readfile_aio(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count,
			nvfuse_off_t roffset) {
    struct nvfuse_handle *h=nvh;
    if(h->nvh_target->journal) return -EOPNOTSUPP;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_readfile_aio_impl(nvh, fid, buffer, count, roffset);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_readfile_aio_directio_impl(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count,
				 nvfuse_off_t roffset);
s32 nvfuse_readfile_aio_directio(struct nvfuse_handle *nvh, u32 fid, s8 *buffer, s32 count,
				 nvfuse_off_t roffset) {
    struct nvfuse_handle *h=nvh;
    if(h->nvh_target->journal) return -EOPNOTSUPP;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_readfile_aio_directio_impl(nvh, fid, buffer, count, roffset);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_writefile_impl(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf, u32 count,
		     nvfuse_off_t woffset);
s32 nvfuse_writefile(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf, u32 count,
		     nvfuse_off_t woffset) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    if (!h->nvh_target->journal)
        return nvfuse_tx_leave(h,nvfuse_writefile_impl(nvh,fid,user_buf,count,woffset));
    if(fid<START_OPEN_FILE || fid>=MAX_OPEN_FILE || !h->nvh_sb.sb_file_table[fid].used)
        return nvfuse_tx_leave(h,-EBADF);
    if(woffset<0 || woffset>=MAX_FILE_SIZE || count>=MAX_FILE_SIZE-woffset)
        return nvfuse_tx_leave(h,-EINVAL);
    if(nvj_version(h->nvh_target->journal)<2 && woffset>h->nvh_sb.sb_file_table[fid].size)
        return nvfuse_tx_leave(h,-EOPNOTSUPP);
    /* Reject an impossible DATA write before mutating mappings or descriptors.
     * Metadata shares the same budget, so even this payload lower bound may
     * leave a smaller actual usable range. Identical-image elision is optional. */
    if(nvj_full_data(h->nvh_target->journal) && count &&
       ((uint64_t)(woffset&(CLUSTER_SIZE-1))+count+CLUSTER_SIZE-1)/CLUSTER_SIZE>=nvj_budget(h->nvh_target->journal))
        return nvfuse_tx_leave(h,-E2BIG);
    /* Legacy eviction can flush while holding the cache lock. Drain between
     * bounded chunks outside that lock, keeping one outer transaction. */
    unsigned done=0;
    do {
        unsigned chunk=count-done;
        if(chunk>65536) chunk=65536;
        int result=nvfuse_writefile_impl(nvh,fid,user_buf+done,chunk,woffset+done);
        if(result<0) return nvfuse_tx_leave(h,result);
        done+=(unsigned)result;
        nvfuse_check_flush_dirty(&h->nvh_sb,DIRTY_FLUSH_FORCE);
        if(nvj_error(h->nvh_target->journal))
            return nvfuse_tx_leave(h,nvj_error(h->nvh_target->journal));
        if((unsigned)result<chunk) break;
    } while(done<count);
    return nvfuse_tx_leave(h,(int)done);
}
extern s32 nvfuse_writefile_directio_prepare_impl(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf,
				      u32 count, nvfuse_off_t woffset);
s32 nvfuse_writefile_directio_prepare(struct nvfuse_handle *nvh, u32 fid, const s8 *user_buf,
				      u32 count, nvfuse_off_t woffset) {
    struct nvfuse_handle *h=nvh;
    if(h->nvh_target->journal) return -EOPNOTSUPP;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_writefile_directio_prepare_impl(nvh, fid, user_buf, count, woffset);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_createfile_impl(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, inode_t *new_ino,
		      mode_t mode, dev_t dev);
s32 nvfuse_createfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, inode_t *new_ino,
		      mode_t mode, dev_t dev) {
    struct nvfuse_handle *h=sb->sb_nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_createfile_impl(sb, par_ino, filename, new_ino, mode, dev);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_rmfile_impl(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename);
s32 nvfuse_rmfile(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename) {
    struct nvfuse_handle *h=sb->sb_nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_rmfile_impl(sb, par_ino, filename);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_rmfile_path_impl(struct nvfuse_handle *nvh, const char *path);
s32 nvfuse_rmfile_path(struct nvfuse_handle *nvh, const char *path) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_rmfile_path_impl(nvh, path);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_unlink_impl(struct nvfuse_handle *nvh, const char *path);
s32 nvfuse_unlink(struct nvfuse_handle *nvh, const char *path) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_unlink_impl(nvh, path);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_rmdir_impl(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename);
s32 nvfuse_rmdir(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename) {
    struct nvfuse_handle *h=sb->sb_nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_rmdir_impl(sb, par_ino, filename);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_rmdir_path_impl(struct nvfuse_handle *nvh, const char *path);
s32 nvfuse_rmdir_path(struct nvfuse_handle *nvh, const char *path) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_rmdir_path_impl(nvh, path);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_mkdir_impl(struct nvfuse_superblock *sb, const inode_t par_ino, const s8 *dirname,
		 inode_t *new_ino, const mode_t mode);
s32 nvfuse_mkdir(struct nvfuse_superblock *sb, const inode_t par_ino, const s8 *dirname,
		 inode_t *new_ino, const mode_t mode) {
    struct nvfuse_handle *h=sb->sb_nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_mkdir_impl(sb, par_ino, dirname, new_ino, mode);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_rename_impl(struct nvfuse_handle *nvh, inode_t par_ino, s8 *name, inode_t new_par_ino,
		  s8 *newname);
s32 nvfuse_rename(struct nvfuse_handle *nvh, inode_t par_ino, s8 *name, inode_t new_par_ino,
		  s8 *newname) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_rename_impl(nvh, par_ino, name, new_par_ino, newname);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_rename_path_impl(struct nvfuse_handle *nvh, const char *from, const char *to);
s32 nvfuse_rename_path(struct nvfuse_handle *nvh, const char *from, const char *to) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_rename_path_impl(nvh, from, to);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_hardlink_impl(struct nvfuse_superblock *sb, inode_t par_ino, s8 *name, inode_t new_par_ino,
		    s8 *newname);
s32 nvfuse_hardlink(struct nvfuse_superblock *sb, inode_t par_ino, s8 *name, inode_t new_par_ino,
		    s8 *newname) {
    struct nvfuse_handle *h=sb->sb_nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_hardlink_impl(sb, par_ino, name, new_par_ino, newname);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_hardlink_path_impl(struct nvfuse_handle *nvh, const char *from, const char *to);
s32 nvfuse_hardlink_path(struct nvfuse_handle *nvh, const char *from, const char *to) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_hardlink_path_impl(nvh, from, to);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_mknod_impl(struct nvfuse_handle *nvh, const char *path, mode_t mode, dev_t dev);
s32 nvfuse_mknod(struct nvfuse_handle *nvh, const char *path, mode_t mode, dev_t dev) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_mknod_impl(nvh, path, mode, dev);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_mkdir_path_impl(struct nvfuse_handle *nvh, const char *path, mode_t mode);
s32 nvfuse_mkdir_path(struct nvfuse_handle *nvh, const char *path, mode_t mode) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_mkdir_path_impl(nvh, path, mode);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_truncate_path_impl(struct nvfuse_handle *nvh, const char *path, nvfuse_off_t size);
s32 nvfuse_truncate_path(struct nvfuse_handle *nvh, const char *path, nvfuse_off_t size) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_truncate_path_impl(nvh, path, size);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_ftruncate_impl(struct nvfuse_handle *nvh, s32 fid, nvfuse_off_t size);
s32 nvfuse_ftruncate(struct nvfuse_handle *nvh, s32 fid, nvfuse_off_t size) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    if(!valid_fd(h,fid)) return nvfuse_tx_leave(h,-EBADF);
    int result=nvfuse_ftruncate_impl(nvh, fid, size);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_symlink_impl(struct nvfuse_handle *nvh, const char *link, inode_t parent, const char *name);
s32 nvfuse_symlink(struct nvfuse_handle *nvh, const char *link, inode_t parent, const char *name) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_symlink_impl(nvh, link, parent, name);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_symlink_path_impl(struct nvfuse_handle *nvh, const char *target_name, const char *link_name);
s32 nvfuse_symlink_path(struct nvfuse_handle *nvh, const char *target_name, const char *link_name) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_symlink_path_impl(nvh, target_name, link_name);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_readlink_ino_impl(struct nvfuse_handle *nvh, inode_t ino, char *buf, size_t size);
s32 nvfuse_readlink_ino(struct nvfuse_handle *nvh, inode_t ino, char *buf, size_t size) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_readlink_ino_impl(nvh, ino, buf, size);
    return nvfuse_tx_leave_lookup(h,result);
}
extern s32 nvfuse_getattr_impl(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf);
s32 nvfuse_getattr(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_getattr_impl(nvh, path, stbuf);
    return nvfuse_tx_leave_lookup(h,result);
}
extern s32 nvfuse_fgetattr_impl(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf, s32 fd);
s32 nvfuse_fgetattr(struct nvfuse_handle *nvh, const char *path, struct stat *stbuf, s32 fd) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    if(!valid_fd(h,fd)) return nvfuse_tx_leave(h,-EBADF);
    int result=nvfuse_fgetattr_impl(nvh, path, stbuf, fd);
    return nvfuse_tx_leave(h,result);
}
#if NVFUSE_OS == NVFUSE_OS_LINUX
extern s32 nvfuse_access_impl(struct nvfuse_handle *nvh, const char *path, int mask);
s32 nvfuse_access(struct nvfuse_handle *nvh, const char *path, int mask) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_access_impl(nvh, path, mask);
    return nvfuse_tx_leave_lookup(h,result);
}
#endif
extern s32 nvfuse_readlink_impl(struct nvfuse_handle *nvh, const char *path, char *buf, size_t size);
s32 nvfuse_readlink(struct nvfuse_handle *nvh, const char *path, char *buf, size_t size) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_readlink_impl(nvh, path, buf, size);
    return nvfuse_tx_leave_lookup(h,result);
}
extern s32 nvfuse_statvfs_impl(struct nvfuse_handle *nvh, const char *path, struct statvfs *buf);
s32 nvfuse_statvfs(struct nvfuse_handle *nvh, const char *path, struct statvfs *buf) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_statvfs_impl(nvh, path, buf);
    return nvfuse_tx_leave_lookup(h,result);
}
#if NVFUSE_OS == NVFUSE_OS_LINUX
extern s32 nvfuse_chmod_path_impl(struct nvfuse_handle *nvh, const char *path, mode_t mode);
s32 nvfuse_chmod_path(struct nvfuse_handle *nvh, const char *path, mode_t mode) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_chmod_path_impl(nvh, path, mode);
    return nvfuse_tx_leave(h,result);
}
#endif
#if NVFUSE_OS == NVFUSE_OS_LINUX
extern s32 nvfuse_chown_impl(struct nvfuse_handle *nvh, const char *path, uid_t uid, gid_t gid);
s32 nvfuse_chown(struct nvfuse_handle *nvh, const char *path, uid_t uid, gid_t gid) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_chown_impl(nvh, path, uid, gid);
    return nvfuse_tx_leave(h,result);
}
#endif
#if NVFUSE_OS == NVFUSE_OS_LINUX
extern s32 nvfuse_utimens_impl(struct nvfuse_handle *nvh, const char *path, const struct timespec ts[2]);
s32 nvfuse_utimens(struct nvfuse_handle *nvh, const char *path, const struct timespec ts[2]) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_utimens_impl(nvh, path, ts);
    return nvfuse_tx_leave(h,result);
}
#endif
extern s32 nvfuse_fdatasync_impl(struct nvfuse_handle *nvh, int fd);
s32 nvfuse_fdatasync(struct nvfuse_handle *nvh, int fd) {
    struct nvfuse_handle *h=nvh;
    if(h->nvh_target->tx_user) return -EBUSY;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    if(fd<START_OPEN_FILE || fd>=MAX_OPEN_FILE || !h->nvh_sb.sb_file_table[fd].used)
        return nvfuse_tx_leave(h,-EBADF);
    nvfuse_check_flush_dirty(&h->nvh_sb,DIRTY_FLUSH_FORCE);
    reactor_sync_flush(h->nvh_target);
    return nvfuse_tx_leave(h,0);
}
extern s32 nvfuse_fsync_impl(struct nvfuse_handle *nvh, int fd);
s32 nvfuse_fsync(struct nvfuse_handle *nvh, int fd) {
    struct nvfuse_handle *h=nvh;
    if(h->nvh_target->tx_user) return -EBUSY;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    if(fd<START_OPEN_FILE || fd>=MAX_OPEN_FILE || !h->nvh_sb.sb_file_table[fd].used)
        return nvfuse_tx_leave(h,-EBADF);
    nvfuse_check_flush_dirty(&h->nvh_sb,DIRTY_FLUSH_FORCE);
    reactor_sync_flush(h->nvh_target);
    return nvfuse_tx_leave(h,0);
}
extern s32 nvfuse_sync_impl(struct nvfuse_handle *nvh);
s32 nvfuse_sync(struct nvfuse_handle *nvh) {
    struct nvfuse_handle *h=nvh;
    if(h->nvh_target->tx_user) return -EBUSY;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    nvfuse_check_flush_dirty(&h->nvh_sb,DIRTY_FLUSH_FORCE);
    reactor_sync_flush(h->nvh_target);
    return nvfuse_tx_leave(h,0);
}
extern s32 nvfuse_fallocate_impl(struct nvfuse_handle *nvh, const char *path, s64 start, s64 length);
s32 nvfuse_fallocate(struct nvfuse_handle *nvh, const char *path, s64 start, s64 length) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    if(h->nvh_target->journal) return nvfuse_tx_leave(h,nvfuse_allocate_range(h,path,start,length));
    int result=nvfuse_fallocate_impl(nvh, path, start, length);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_truncate_impl(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename,
		    nvfuse_off_t trunc_size);
s32 nvfuse_truncate(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename,
		    nvfuse_off_t trunc_size) {
    struct nvfuse_handle *h=sb->sb_nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_truncate_impl(sb, par_ino, filename, trunc_size);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_truncate_ino_impl(struct nvfuse_superblock *sb, inode_t ino, s64 trunc_size);
s32 nvfuse_truncate_ino(struct nvfuse_superblock *sb, inode_t ino, s64 trunc_size) {
    struct nvfuse_handle *h=sb->sb_nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_truncate_ino_impl(sb, ino, trunc_size);
    return nvfuse_tx_leave(h,result);
}
extern s32 nvfuse_chmod_impl(struct nvfuse_handle *nvh, inode_t par_ino, s8 *filename, mode_t mode);
s32 nvfuse_chmod(struct nvfuse_handle *nvh, inode_t par_ino, s8 *filename, mode_t mode) {
    struct nvfuse_handle *h=nvh;
    int rc=nvfuse_tx_enter(h); if(rc) return rc;
    int result=nvfuse_chmod_impl(nvh, par_ino, filename, mode);
    return nvfuse_tx_leave(h,result);
}
