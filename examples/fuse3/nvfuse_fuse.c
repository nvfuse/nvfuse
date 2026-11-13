/* Optional Linux FUSE 3 frontend. The filesystem and device logic stay in NVFUSE. */
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_backend.h"
#include "nvfuse_xattr.h"

/* One foreground process, one request thread, one exclusively owned backend. */
static struct nvfuse_handle *fs;

static int nf_path_getattr(const char *path, struct stat *st)
{
    return nvfuse_getattr(fs, path, st);
}

static int nf_fgetattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    return nvfuse_fgetattr(fs, path, st, (int)fi->fh);
}

static int nf_owner(const char *path)
{
    return nvfuse_chown(fs, path, getuid(), getgid());
}

static int nf_descriptor_available(void)
{
    for (int fd = START_OPEN_FILE; fd < MAX_OPEN_FILE; fd++)
        if (!fs->nvh_sb.sb_file_table[fd].used) return 0;
    return -EMFILE;
}

/* FUSE only guarantees open flags in selected callbacks. Use our stored flags
 * for descriptor operations (not the temporary request's fi->flags). */
static int nf_file_flags(const struct fuse_file_info *fi)
{
    if (fi->fh < START_OPEN_FILE || fi->fh >= MAX_OPEN_FILE ||
        !fs->nvh_sb.sb_file_table[fi->fh].used) return -EBADF;
    return fs->nvh_sb.sb_file_table[fi->fh].flags;
}

static int nf_open(const char *path, struct fuse_file_info *fi)
{
    struct stat st;
    int rc = nf_path_getattr(path, &st);
    if (rc) return rc;
    if (S_ISDIR(st.st_mode)) return -EISDIR;
    if (S_ISLNK(st.st_mode)) return -ELOOP;
    if (!S_ISREG(st.st_mode)) return -EOPNOTSUPP;
    if ((fi->flags & O_TRUNC) && (fi->flags & O_ACCMODE) == O_RDONLY) return -EACCES;
    if (nf_descriptor_available()) return -EMFILE;
    int fd = nvfuse_openfile_path(fs, path, fi->flags & ~(O_CREAT | O_EXCL | O_TRUNC), 0);
    if (fd < 0) return fd;
    if (fi->flags & O_TRUNC) {
        rc = nvfuse_ftruncate(fs, fd, 0);
        if (rc) { nvfuse_closefile(fs, fd); return rc; }
    }
    fi->fh = (uint64_t)fd;
    fi->keep_cache = 0;
    return 0;
}

static int nf_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct stat st;
    int rc = nf_path_getattr(path, &st);
    if (!rc) return fi->flags & O_EXCL ? -EEXIST : nf_open(path, fi);
    if (rc != -ENOENT) return rc;
    if (nf_descriptor_available()) return -EMFILE;
    int fd = nvfuse_openfile_path(fs, path, O_CREAT | O_EXCL | O_RDWR, mode | S_IFREG);
    if (fd < 0) return fd;
    rc = nf_owner(path);
    if (rc) { nvfuse_closefile(fs, fd); nvfuse_unlink(fs, path); return rc; }
    /* The creation descriptor must carry the caller's access/append flags. */
    fs->nvh_sb.sb_file_table[fd].flags = fi->flags;
    fi->fh = (uint64_t)fd;
    fi->keep_cache = 0;
    return 0;
}

static int nf_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    return nvfuse_closefile(fs, (int)fi->fh);
}

static int nf_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    (void)path;
    if (off < 0 || size > INT_MAX) return -EINVAL;
    int flags = nf_file_flags(fi);
    if (flags < 0) return flags;
    if ((flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    return nvfuse_readfile(fs, (u32)fi->fh, buf, (s32)size, off);
}

static int nf_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    (void)path;
    if (off < 0 || size > INT_MAX) return -EINVAL;
    int flags = nf_file_flags(fi);
    if (flags < 0) return flags;
    if ((flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (flags & O_APPEND) {
        struct stat st;
        int rc = nf_fgetattr(path, &st, fi);
        if (rc) return rc;
        off = st.st_size;
    }
    return nvfuse_writefile(fs, (u32)fi->fh, buf, (u32)size, off);
}

static int nf_fsync(const char *path, int data, struct fuse_file_info *fi)
{
    (void)path;
    return data ? nvfuse_fdatasync(fs, (int)fi->fh) : nvfuse_fsync(fs, (int)fi->fh);
}

static int nf_flush(const char *path, struct fuse_file_info *fi)
{
    /* flush can occur multiple times; it must never close the descriptor. */
    return nf_fsync(path, 0, fi);
}

static int nf_opendir(const char *path, struct fuse_file_info *fi)
{
    struct stat st;
    int rc = nf_path_getattr(path, &st);
    if (rc) return rc;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    int ino = nvfuse_opendir(fs, path);
    if (ino < 0) return ino;
    fi->fh = (uint64_t)ino | (UINT64_C(1) << 63);
    return 0;
}

static int nf_readdir(const char *path, void *buf, fuse_fill_dir_t fill,
                      off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    (void)flags;
    (void)path;
    if (off < 0) return -EINVAL;
    struct nvfuse_inode_ctx *ctx = nvfuse_read_inode(&fs->nvh_sb, NULL, (inode_t)(fi->fh & ~(UINT64_C(1) << 63)));
    if (!ctx) return -EIO;
    off_t slots = ctx->ictx_inode->i_size / DIR_ENTRY_SIZE;
    nvfuse_release_inode(&fs->nvh_sb, ctx, NVF_CLEAN);
    if (slots < 2) slots = 2;
    for (; off < slots; off++) {
        struct dirent ent;
        /* A deleted directory slot is a hole, not end-of-directory. */
        if (!nvfuse_readdir(fs, (inode_t)(fi->fh & ~(UINT64_C(1) << 63)), &ent, off)) continue;
        struct stat st = {0};
        st.st_ino = ent.d_ino;
        /* A zero type lets the kernel fetch attributes, including symlinks. */
        if (fill(buf, ent.d_name, &st, off + 1, 0)) break;
    }
    return 0;
}

static int nf_releasedir(const char *path, struct fuse_file_info *fi)
{
    (void)path; (void)fi;
    return 0;
}

static int nf_fsyncdir(const char *path, int data, struct fuse_file_info *fi)
{
    (void)path; (void)data; (void)fi;
    return nvfuse_sync(fs);
}

static int nf_mkdir(const char *path, mode_t mode)
{
    struct stat st;
    int rc = nf_path_getattr(path, &st);
    if (!rc) return -EEXIST;
    if (rc != -ENOENT) return rc;
    rc = nvfuse_mkdir_path(fs, path, mode);
    if (!rc) {
        rc = nf_owner(path);
        if (rc) nvfuse_rmdir_path(fs, path);
    }
    return rc;
}

static int nf_mknod(const char *path, mode_t mode, dev_t dev)
{
    (void)dev;
    if (!S_ISREG(mode)) return -EOPNOTSUPP;
    struct fuse_file_info fi = {.flags = O_CREAT | O_EXCL | O_WRONLY};
    int rc = nf_create(path, mode, &fi);
    return rc ? rc : nf_release(path, &fi);
}

static int nf_unlink(const char *path)
{
    struct stat st;
    int rc = nf_path_getattr(path, &st);
    if (rc) return rc;
    return S_ISDIR(st.st_mode) ? -EISDIR : nvfuse_unlink(fs, path);
}
static int nf_rmdir(const char *path)
{
    struct stat st;
    int rc = nf_path_getattr(path, &st);
    if (rc) return rc;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    if (st.st_ino == nvfuse_get_root_ino(fs)) return -EBUSY;
    /* The core's directory link count includes all occupied entries. */
    if (st.st_nlink > 2) return -ENOTEMPTY;
    return nvfuse_rmdir_path(fs, path);
}
static int nf_rename(const char *from, const char *to, unsigned flags)
{
    if (flags) return -EOPNOTSUPP;
    return nvfuse_rename_path(fs, from, to);
}
static int nf_link(const char *from, const char *to)
{
    struct stat st;
    int rc = nf_path_getattr(to, &st);
    if (!rc) return -EEXIST;
    if (rc != -ENOENT) return rc;
    rc = nf_path_getattr(from, &st);
    if (rc) return rc;
    return S_ISDIR(st.st_mode) ? -EPERM : nvfuse_hardlink_path(fs, from, to);
}
static int nf_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) { (void)fi; return path ? nvfuse_chmod_path(fs, path, mode) : -EOPNOTSUPP; }
static int nf_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) { (void)fi; return path ? nvfuse_chown(fs, path, uid, gid) : -EOPNOTSUPP; }
static int nf_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    if (fi) {
        int flags = nf_file_flags(fi);
        if (flags < 0) return flags;
        if ((flags & O_ACCMODE) == O_RDONLY) return -EBADF;
        return nvfuse_ftruncate(fs, (int)fi->fh, size);
    }
    return nvfuse_truncate_path(fs, path, size);
}
static int nf_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) { (void)fi; return path ? nvfuse_utimens(fs, path, tv) : -EOPNOTSUPP; }
static int nf_statfs(const char *path, struct statvfs *st) { return nvfuse_statvfs(fs, path, st); }

static int nf_symlink(const char *target, const char *path)
{
    struct stat st;
    int rc = nf_path_getattr(path, &st);
    if (!rc) return -EEXIST;
    if (rc != -ENOENT) return rc;
    rc = nvfuse_symlink_path(fs, target, path);
    if (!rc) {
        rc = nf_owner(path);
        if (rc) nvfuse_unlink(fs, path);
    }
    return rc;
}

static int nf_readlink(const char *path, char *buf, size_t size)
{
    if (!size) return -EINVAL;
    int rc = nvfuse_readlink(fs, path, buf, size - 1);
    if (rc < 0) return rc;
    buf[rc] = 0;
    return 0;
}



static int nf_setxattr(const char *path, const char *name, const char *buf,
                       size_t size, int flags)
{
    if (flags & ~(XATTR_CREATE | XATTR_REPLACE)) return -EOPNOTSUPP;
    unsigned options = (flags & XATTR_CREATE ? NVFUSE_XATTR_CREATE : 0) |
                       (flags & XATTR_REPLACE ? NVFUSE_XATTR_REPLACE : 0);
    return nvfuse_setxattr(fs, path, name, buf, size, options);
}

static int nf_getxattr(const char *path, const char *name, char *buf, size_t size)
{
    return nvfuse_getxattr(fs, path, name, buf, size);
}

static int nf_listxattr(const char *path, char *buf, size_t size)
{
    return nvfuse_listxattr(fs, path, buf, size);
}

static int nf_removexattr(const char *path, const char *name)
{
    return nvfuse_removexattr(fs, path, name);
}

static int nf_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    if (fi && !(fi->fh & (UINT64_C(1) << 63))) return nf_fgetattr(path, st, fi);
    return path ? nf_path_getattr(path, st) : -EOPNOTSUPP;
}

static void *nf_init(struct fuse_conn_info *conn, struct fuse_config *config)
{
    (void)conn;
    config->entry_timeout = 0;
    config->attr_timeout = 0;
    config->negative_timeout = 0;
    config->use_ino = 1;
    config->nullpath_ok = 1;
    /* Keep libfuse's standard hidden-name handling: high-level getattr can be
     * issued without a file handle, even for an unlinked open file. */
    config->hard_remove = 0;
    return NULL;
}

static const struct fuse_operations nf_operations = {
    .init = nf_init, .getattr = nf_getattr,
    .open = nf_open, .create = nf_create, .release = nf_release,
    .read = nf_read, .write = nf_write, .flush = nf_flush, .fsync = nf_fsync,
    .opendir = nf_opendir, .readdir = nf_readdir, .releasedir = nf_releasedir,
    .fsyncdir = nf_fsyncdir, .mkdir = nf_mkdir, .mknod = nf_mknod,
    .unlink = nf_unlink, .rmdir = nf_rmdir, .rename = nf_rename, .link = nf_link,
    .symlink = nf_symlink, .readlink = nf_readlink,
    .chmod = nf_chmod, .chown = nf_chown, .utimens = nf_utimens,
    .truncate = nf_truncate, .statfs = nf_statfs,
    .setxattr = nf_setxattr, .getxattr = nf_getxattr,
    .listxattr = nf_listxattr, .removexattr = nf_removexattr,
};

static void nf_usage(const char *name)
{
    fprintf(stderr,
        "Usage: %s --image IMAGE MOUNTPOINT [--debug]\n"
        "       %s --mkfs IMAGE --size-mib MULTIPLE_OF_128\n"
        "       %s --backend imagefile|kernel|spdk --target PATH_OR_BDF [--nsid N]\n"
        "          MOUNTPOINT [--debug]\n"
        "       %s --backend imagefile|kernel|spdk --target PATH_OR_BDF [--nsid N]\n"
        "          --mkfs [--size-mib MULTIPLE_OF_128] [--force]\n"
        "Device formatting requires --force and defaults to the whole device.\n"
        "Images require --size-mib; mounting never formats.\n",
        name, name, name, name);
}

static int nf_number(const char *text, uint64_t *value)
{
    char *end;
    if (!text[0] || text[0] == '-' || text[0] == '+') return -1;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    if (errno || *end) return -1;
    *value = n;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) { nf_usage(argv[0]); return 0; }
    struct nvfuse_backend_options options = {.kind = NVFUSE_BACKEND_IMAGEFILE};
    const char *mount_arg = NULL;
    int debug = 0, force = 0, kind_seen = 0, start = 1;
    /* Retain the original image-format CLI. The new --mkfs is a flag. */
    if (argc >= 3 && !strcmp(argv[1], "--mkfs") && argv[2][0] != '-') {
        options.format = 1; options.target = argv[2]; start = 3;
    }
    for (int i = start; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--debug")) debug = 1;
        else if (!strcmp(arg, "--force")) force = 1;
        else if (!strcmp(arg, "--mkfs")) options.format = 1;
        else if (!strcmp(arg, "--image") || !strcmp(arg, "--target")) {
            if (++i == argc || options.target) goto usage;
            if (!strcmp(arg, "--image") && kind_seen) goto usage;
            options.target = argv[i];
            if (!strcmp(arg, "--image")) kind_seen = 1;
        } else if (!strcmp(arg, "--backend")) {
            if (++i == argc || kind_seen) goto usage;
            kind_seen = 1;
            if (!strcmp(argv[i], "imagefile")) options.kind = NVFUSE_BACKEND_IMAGEFILE;
            else if (!strcmp(argv[i], "kernel")) options.kind = NVFUSE_BACKEND_KERNEL;
            else if (!strcmp(argv[i], "spdk")) options.kind = NVFUSE_BACKEND_SPDK;
            else goto usage;
        } else if (!strcmp(arg, "--size-mib")) {
            uint64_t mib;
            if (++i == argc || options.size_bytes || nf_number(argv[i], &mib) ||
                mib < 128 || mib % 128 || mib > UINT64_MAX / (1024 * 1024)) goto usage;
            options.size_bytes = mib * 1024 * 1024;
        } else if (!strcmp(arg, "--nsid")) {
            uint64_t nsid;
            if (++i == argc || options.namespace_id || nf_number(argv[i], &nsid) ||
                !nsid || nsid > UINT32_MAX) goto usage;
            options.namespace_id = (uint32_t)nsid;
        } else if (arg[0] == '-' || mount_arg) goto usage;
        else mount_arg = arg;
    }
    if (!options.target || (options.namespace_id && options.kind != NVFUSE_BACKEND_SPDK)) goto usage;
    if (getuid() > UINT16_MAX || getgid() > UINT16_MAX) {
        fprintf(stderr, "NVFUSE currently supports only 16-bit UID/GID values.\n"); return 1;
    }
    if (!nvfuse_backend_supported(options.kind)) {
        fprintf(stderr, "Backend unavailable in this build/platform; SPDK needs make fuse-spdk.\n"); return 1;
    }
    if (options.format) {
        if ((options.kind == NVFUSE_BACKEND_IMAGEFILE && !options.size_bytes) || mount_arg) goto usage;
        if (options.kind != NVFUSE_BACKEND_IMAGEFILE && !force) {
            fprintf(stderr, "Device format overwrites data; specify --force with the selected target.\n"); return 2;
        }
        fs = nvfuse_backend_open(&options);
        if (!fs) { perror("format backend"); return 1; }
        int rc = nf_owner("/");
        if (!rc) rc = nvfuse_chmod_path(fs, "/", 0755);
        int close_rc = nvfuse_backend_close(fs);
        if (rc || close_rc) { fprintf(stderr, "filesystem initialization failed: %d / %d\n", rc, close_rc); return 1; }
        printf("Created NVFUSE filesystem: %s\n", options.target);
        return 0;
    }
    if (!mount_arg || options.size_bytes || force) goto usage;
    char target[PATH_MAX], mountpoint[PATH_MAX];
    if (!realpath(mount_arg, mountpoint)) { perror("mountpoint"); return 1; }
    if (options.kind != NVFUSE_BACKEND_SPDK) {
        if (!realpath(options.target, target)) { perror("target"); return 1; }
        size_t len = strlen(mountpoint);
        if (!strncmp(target, mountpoint, len) && (target[len] == '/' || !target[len])) {
            fprintf(stderr, "The backing target must be outside the mount point.\n"); return 1;
        }
        options.target = target;
    }
    struct stat st;
    if (stat(mountpoint, &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Mount point must be an existing directory.\n"); return 1;
    }
    fs = nvfuse_backend_open(&options);
    if (!fs) { perror("open backend"); return 1; }
    /* -f prevents a post-open fork; -s keeps the image's owning thread. */
    char *args[] = {argv[0], "-f", "-s", "-o", "default_permissions,fsname=nvfuse",
                    mountpoint, "-d", NULL};
    int rc = fuse_main(debug ? 7 : 6, args, &nf_operations, NULL);
    /* Unmount may leave open handles if clients disappeared. Close on owner. */
    for (int fd = START_OPEN_FILE; fd < MAX_OPEN_FILE; fd++)
        if (fs->nvh_sb.sb_file_table[fd].used) {
            int err = nvfuse_closefile(fs, fd);
            if (err) { fprintf(stderr, "descriptor close failed: %d\n", err); rc = 1; }
        }
    int close_rc = nvfuse_backend_close(fs);
    if (close_rc) { fprintf(stderr, "filesystem close failed: %d\n", close_rc); rc = 1; }
    return rc ? 1 : 0;
usage:
    nf_usage(argv[0]);
    return 2;
}
