/* Same public API oracle on image files, Linux block devices and SPDK NVMe. */
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_backend.h"
#include "nvfuse_tx.h"
#include "nvfuse_xattr.h"
#ifdef NVFUSE_SPDK_TRANSPORT
#include "nvfuse_nvme_transport.h"
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s (errno=%d)\n", __LINE__, #x, errno); exit(1); } } while (0)
#define LENGTH (2 * 1024 * 1024 + 137)
static unsigned char byte_at(size_t i) { return (unsigned char)(i * 37 + i / 4096); }
static void verify_api(struct nvfuse_handle *h)
{
    struct stat st;
    CHECK(!nvfuse_getattr(h, "/api-data", &st) && st.st_size == LENGTH);
    int fd = nvfuse_openfile_path(h, "/api-data", O_RDONLY, 0);
    CHECK(fd >= 0);
    unsigned char data[5003];
    for (int offset = 0; offset < LENGTH;) {
        int count = LENGTH - offset;
        if (count > (int)sizeof(data)) count = sizeof(data);
        CHECK(nvfuse_readfile(h, fd, (char *)data, count, offset) == count);
        for (int i = 0; i < count; i++) CHECK(data[i] == byte_at(offset + i));
        offset += count;
    }
    CHECK(!nvfuse_closefile(h, fd));
    unsigned char value[4];
    CHECK(nvfuse_getxattr(h, "/api-data", "user.binary", value, sizeof(value)) == 4);
    CHECK(!memcmp(value, "\0\xff\1\x80", 4));
    CHECK(nvfuse_getattr(h, "/aborted", &st) == -ENOENT);
}
static void create(struct nvfuse_handle *h)
{
#ifdef __linux__
    CHECK(!nvfuse_chmod_path(h, "/", 0755));
    CHECK(!nvfuse_chown(h, "/", getuid(), getgid()));
#endif
    int fd = nvfuse_openfile_path(h, "/api-data", O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK(fd >= 0);
    unsigned char data[7001];
    for (int offset = 0; offset < LENGTH;) {
        int count = LENGTH - offset;
        if (count > (int)sizeof(data)) count = sizeof(data);
        for (int i = 0; i < count; i++) data[i] = byte_at(offset + i);
        CHECK(nvfuse_writefile(h, fd, (char *)data, count, offset) == count);
        offset += count;
    }
    CHECK(!nvfuse_fsync(h, fd));
    CHECK(!nvfuse_closefile(h, fd));
    CHECK(!nvfuse_setxattr(h, "/api-data", "user.binary", "\0\xff\1\x80", 4, 0));
    CHECK(!nvfuse_tx_begin(h, NVFUSE_TX_METADATA, 0));
    fd = nvfuse_openfile_path(h, "/aborted", O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0 && !nvfuse_closefile(h, fd));
    CHECK(!nvfuse_tx_abort(h));
    verify_api(h);
}
static void verify_fuse(struct nvfuse_handle *h)
{
    const char tail[] = "unaligned-tail";
    int length = 256 * 8193 + (int)strlen(tail);
    struct stat st;
    CHECK(!nvfuse_getattr(h, "/POSIX tests/binary.dat", &st) && st.st_size == length);
    int fd = nvfuse_openfile_path(h, "/POSIX tests/binary.dat", O_RDONLY, 0);
    CHECK(fd >= 0);
    unsigned char data[4099];
    for (int offset = 0; offset < length;) {
        int count = length - offset;
        if (count > (int)sizeof(data)) count = sizeof(data);
        CHECK(nvfuse_readfile(h, fd, (char *)data, count, offset) == count);
        for (int i = 0; i < count; i++) {
            int at = offset + i;
            CHECK(data[i] == (at < 256 * 8193 ? (unsigned char)at : (unsigned char)tail[at - 256 * 8193]));
        }
        offset += count;
    }
    CHECK(!nvfuse_closefile(h, fd));
    CHECK(nvfuse_getxattr(h, "/POSIX tests/binary.dat", "user.binary", data, sizeof(data)) == 32);
    for (unsigned i = 0; i < 32; i++) CHECK(data[i] == i);
}
int main(int argc, char **argv)
{
    CHECK(argc == 4 || argc == 5);
    int legacy = !strcmp(argv[1], "legacy-create");
    int whole = !strcmp(argv[1], "whole-create");
    struct nvfuse_backend_options o = {.target = argv[3], .format = legacy || whole || !strcmp(argv[1], "create")};
    if (!strcmp(argv[2], "imagefile")) o.kind = NVFUSE_BACKEND_IMAGEFILE;
    else if (!strcmp(argv[2], "kernel")) o.kind = NVFUSE_BACKEND_KERNEL;
    else if (!strcmp(argv[2], "spdk")) o.kind = NVFUSE_BACKEND_SPDK;
    else CHECK(0);
    if (argc == 5) o.namespace_id = strtoul(argv[4], NULL, 10);
    if (o.format && !whole) o.size_bytes = 256ULL * 1024 * 1024;
    struct nvfuse_handle *h;
#ifdef NVFUSE_SPDK_TRANSPORT
    if (legacy) {
        CHECK(o.kind == NVFUSE_BACKEND_SPDK);
        h = nvfuse_nvme_fs_open(o.target, 1, o.size_bytes, 0);
    } else
#else
    CHECK(!legacy);
#endif
        h = nvfuse_backend_open(&o);
    if (!h) { perror("nvfuse_backend_open"); return 1; }
    if (whole) {
        struct statvfs geometry;
        CHECK(!nvfuse_statvfs(h, "/", &geometry));
        printf("WHOLE_DEVICE_BYTES=%llu\n",
               (unsigned long long)geometry.f_blocks * geometry.f_frsize);
    }
    if (o.format) create(h);
    else {
        verify_api(h);
        if (!strcmp(argv[1], "after-fuse")) verify_fuse(h);
        else CHECK(!strcmp(argv[1], "verify"));
    }
    CHECK(!nvfuse_backend_close(h));
    printf("PASS backend %s %s\n", argv[1], argv[2]);
    return 0;
}
