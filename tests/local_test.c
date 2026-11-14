/* SPDX-License-Identifier: GPL-2.0-only */

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_file.h"
#include "nvfuse_dirhash.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_backend.h"

#define PAYLOAD_SIZE (3 * 1024 * 1024 + 137)
#define SMALL_FILES 40
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

static void pattern(char *buf, size_t size)
{
    for (size_t i = 0; i < size; i++) buf[i] = (char)((i * 37 + i / 4096) & 255);
    /* A later unaligned overwrite spans three filesystem blocks. */
    memset(buf + 4093, 0x5a, 8197);
}
static int check_payload(struct nvfuse_handle *h, const char *path)
{
    struct stat st;
    CHECK(nvfuse_getattr(h, path, &st) == 0);
    CHECK(st.st_size == PAYLOAD_SIZE);
    int fd = nvfuse_openfile_path(h, path, O_RDONLY, 0);
    CHECK(fd >= 0);
    char *expected = malloc(PAYLOAD_SIZE), *actual = malloc(PAYLOAD_SIZE);
    CHECK(expected && actual);
    pattern(expected, PAYLOAD_SIZE);
    for (int offset = 0; offset < PAYLOAD_SIZE;) {
        int count = PAYLOAD_SIZE - offset;
        if (count > 5003) count = 5003;
        CHECK(nvfuse_readfile(h, fd, actual + offset, count, offset) == count);
        offset += count;
    }
    CHECK(memcmp(actual, expected, PAYLOAD_SIZE) == 0);
    CHECK(nvfuse_readfile(h, fd, actual, 1, PAYLOAD_SIZE) == 0);
    CHECK(nvfuse_closefile(h, fd) == 0);
    free(expected); free(actual);
    return 0;
}
static int check_small(struct nvfuse_handle *h)
{
    for (int i = 0; i < SMALL_FILES; i++) {
        char path[64], expected[64], actual[64] = {0};
        snprintf(path, sizeof(path), "/work/item-%02d", i);
        snprintf(expected, sizeof(expected), "item %d", i);
        int fd = nvfuse_openfile_path(h, path, O_RDONLY, 0);
        CHECK(fd >= 0);
        int n = (int)strlen(expected) + 1;
        CHECK(nvfuse_readfile(h, fd, actual, n, 0) == n);
        CHECK(!memcmp(actual, expected, n));
        CHECK(nvfuse_closefile(h, fd) == 0);
    }
    return 0;
}
static int create_files(struct nvfuse_handle *h)
{
    CHECK(nvfuse_mkdir_path(h, "/work", 0755) == 0);
    int fd = nvfuse_openfile_path(h, "/work/payload", O_CREAT | O_RDWR, 0644);
    CHECK(fd >= 0);
    char *data = malloc(PAYLOAD_SIZE);
    CHECK(data);
    for (size_t i = 0; i < PAYLOAD_SIZE; i++) data[i] = (char)((i * 37 + i / 4096) & 255);
    for (int offset = 0; offset < PAYLOAD_SIZE;) {
        int count = PAYLOAD_SIZE - offset;
        if (count > 7001) count = 7001;
        CHECK(nvfuse_writefile(h, fd, data + offset, count, offset) == count);
        offset += count;
    }
    memset(data + 4093, 0x5a, 8197);
    CHECK(nvfuse_writefile(h, fd, data + 4093, 8197, 4093) == 8197);
    CHECK(nvfuse_fsync(h, fd) == 0);
    CHECK(nvfuse_closefile(h, fd) == 0);
    free(data);
    for (int i = 0; i < SMALL_FILES; i++) {
        char path[64], contents[64];
        snprintf(path, sizeof(path), "/work/item-%02d", i);
        snprintf(contents, sizeof(contents), "item %d", i);
        fd = nvfuse_openfile_path(h, path, O_CREAT | O_RDWR, 0644);
        CHECK(fd >= 0);
        int n = (int)strlen(contents) + 1;
        CHECK(nvfuse_writefile(h, fd, contents, n, 0) == n);
        CHECK(nvfuse_closefile(h, fd) == 0);
    }
    CHECK(check_payload(h, "/work/payload") == 0);
    CHECK(check_small(h) == 0);
    return 0;
}
static int mutate_files(struct nvfuse_handle *h)
{
    CHECK(nvfuse_rename_path(h, "/work/payload", "/work/renamed") == 0);
    struct stat st;
    CHECK(nvfuse_getattr(h, "/work/payload", &st) < 0);
    CHECK(check_payload(h, "/work/renamed") == 0);
    for (int i = 0; i < SMALL_FILES; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/work/item-%02d", i);
        CHECK(nvfuse_rmfile_path(h, path) == 0);
        CHECK(nvfuse_getattr(h, path, &st) < 0);
    }
    CHECK(nvfuse_mkdir_path(h, "/empty", 0755) == 0);
    CHECK(nvfuse_rmdir_path(h, "/empty") == 0);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "create") && strcmp(argv[1], "verify") &&
                     strcmp(argv[1], "mutate") && strcmp(argv[1], "final") &&
                     strcmp(argv[1], "reopen"))) {
        fprintf(stderr, "usage: %s create|verify|mutate|final|reopen IMAGE\n", argv[0]);
        return 2;
    }
    CHECK(crc32c_intel((const unsigned char *)"123456789", 9) == 0x1cf96d7cU);
    CHECK(crc32c_intel((const unsigned char *)"", 0) == 0xffffffffU);
    int create = !strcmp(argv[1], "create");
    struct nvfuse_handle *h = nvfuse_file_open(argv[2], create, 256ULL * 1024 * 1024);
    if (!h) { perror("nvfuse_file_open"); return 1; }
    if (create) CHECK(create_files(h) == 0);
    else if (!strcmp(argv[1], "final")) {
        struct stat st;
        CHECK(check_payload(h, "/work/renamed") == 0);
        CHECK(nvfuse_getattr(h, "/work/payload", &st) < 0);
        CHECK(nvfuse_getattr(h, "/empty", &st) < 0);
        for (int i = 0; i < SMALL_FILES; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/work/item-%02d", i);
            CHECK(nvfuse_getattr(h, path, &st) < 0);
        }
    } else {
        CHECK(check_payload(h, "/work/payload") == 0);
        CHECK(check_small(h) == 0);
        if (!strcmp(argv[1], "mutate")) CHECK(mutate_files(h) == 0);
    }
    CHECK(nvfuse_file_close(h) == 0);
    if (!strcmp(argv[1], "reopen")) {
        /* Pool and ring ownership must survive repeated mounts in one process. */
        for (int i = 0; i < 3; i++) {
            h = nvfuse_file_open(argv[2], 0, 0);
            CHECK(h);
            CHECK(check_payload(h, "/work/payload") == 0);
            CHECK(nvfuse_file_close(h) == 0);
        }
    }
    printf("PASS %s\n", argv[1]);
    return 0;
}
