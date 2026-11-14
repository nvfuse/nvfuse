/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal standalone image API example. Creation never replaces an image. */
#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_backend.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int ok, const char *operation)
{
    if (!ok) {
        fprintf(stderr, "Failed: %s; retain the image for diagnosis\n", operation);
        /* Legacy core errors do not promise that a dirty handle can unwind. */
        fflush(NULL);
        _Exit(EXIT_FAILURE);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "create") && strcmp(argv[1], "read"))) {
        fprintf(stderr, "Usage: %s create|read IMAGE\n", argv[0]);
        return 2;
    }
    int create = !strcmp(argv[1], "create");
    const char contents[] = "Hello from NVFUSE!\n";
    struct nvfuse_backend_options options = {
        .kind = NVFUSE_BACKEND_IMAGEFILE,
        .target = argv[2],
        .format = create,
        .size_bytes = create ? 256ull * 1024 * 1024 : 0,
    };
    struct nvfuse_handle *fs = nvfuse_backend_open(&options);
    if (!fs) { perror("nvfuse_backend_open"); return 1; }
    int fd = nvfuse_openfile_path(fs, "/hello.txt",
                                create ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY,
                                0644);
    require(fd >= 0, "open /hello.txt");
    if (create) {
        require(nvfuse_writefile(fs, fd, contents, sizeof(contents) - 1, 0) ==
                sizeof(contents) - 1, "write /hello.txt");
        require(nvfuse_fsync(fs, fd) == 0, "sync /hello.txt");
    } else {
        struct stat st;
        char actual[sizeof(contents)] = {0};
        require(!nvfuse_getattr(fs, "/hello.txt", &st) &&
                st.st_size == sizeof(contents) - 1, "check file size");
        require(nvfuse_readfile(fs, fd, actual, sizeof(contents) - 1, 0) ==
                sizeof(contents) - 1, "read /hello.txt");
        require(!memcmp(actual, contents, sizeof(contents) - 1), "verify contents");
        fputs(actual, stdout);
    }
    require(nvfuse_closefile(fs, fd) == 0, "close /hello.txt");
    require(nvfuse_backend_close(fs) == 0, "close filesystem");
    puts(create ? "PASS created and synced" : "PASS reopened and verified");
    return 0;
}
