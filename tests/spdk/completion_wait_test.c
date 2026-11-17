/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only qualification against an existing file and its full POSIX copy.
 * Usage: completion_wait_test NATIVE_PATH POSIX_COPY
 * Runtime variables must describe an exclusively owned volume. */
#include "nvfuse_spdk_runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEPTH 32
#define BATCHES 1024
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line=%d condition=%s errno=%d\n", \
                __LINE__, #condition, errno); \
        abort(); \
    } \
} while (0)

static atomic_int finished;
static volatile sig_atomic_t signals;
static pthread_t caller;

static void caught(int sig)
{
    (void)sig;
    signals++;
}

static void *interrupt_waits(void *unused)
{
    (void)unused;
    while (!atomic_load(&finished)) {
        usleep(137);
        pthread_kill(caller, SIGUSR1);
    }
    return NULL;
}

static double now(void)
{
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s NATIVE_PATH POSIX_COPY\n", argv[0]);
        return 2;
    }
    int posix = open(argv[2], O_RDONLY);
    struct stat st;
    CHECK(posix >= 0 && !fstat(posix, &st));
    CHECK(st.st_size >= 4096 && st.st_size % 4096 == 0 &&
          (uint64_t)st.st_size <= INT64_MAX - 8192);

    struct sigaction sa = {.sa_handler = caught};
    sigemptyset(&sa.sa_mask);
    CHECK(!sigaction(SIGUSR1, &sa, NULL));
    CHECK(!nvfio_start(getenv("NVFUSE_SPDK_JSON"),
                       getenv("NVFUSE_SPDK_BDF"), 0));
    struct nvfio_client *client = nvfio_client_alloc(4);
    CHECK(client);
    int fd = nvfio_open(argv[1], 0);
    CHECK(fd >= 0);

    struct nvfio_request *req[DEPTH], *done[DEPTH];
    void *dma[DEPTH];
    unsigned char expected[DEPTH][4096];
    for (unsigned i = 0; i < DEPTH; i++) {
        req[i] = nvfio_request_alloc((void *)(uintptr_t)i);
        dma[i] = nvfio_dma_alloc(4096);
        CHECK(req[i] && dma[i]);
    }
    CHECK(nvfio_wait(client, DEPTH, done, 1000) == 0);
    CHECK(nvfio_wait(client, 0, done, 1000) == -EINVAL);
    CHECK(nvfio_wait(client, DEPTH, done, 1000001) == -EINVAL);

    caller = pthread_self();
    pthread_t interrupter;
    CHECK(!pthread_create(&interrupter, NULL, interrupt_waits, NULL));
    unsigned long completed = 0, empty = 0;
    for (unsigned batch = 0; batch < BATCHES; batch++) {
        unsigned count = batch % 3 == 0 ? 1 : DEPTH;
        uint64_t seen = 0;
        for (unsigned i = 0; i < count; i++) {
            uint64_t offset = ((uint64_t)(batch * 131 + i * 17) %
                               (st.st_size / 4096)) * 4096;
            ssize_t n;
            do {
                n = pread(posix, expected[i], 4096, offset);
            } while (n < 0 && errno == EINTR);
            CHECK(n == 4096);
            CHECK(!nvfio_submit(client, req[i], fd, dma[i], 4096, offset, 0));
        }
        CHECK(nvfio_client_free(client) == -EBUSY);
        unsigned retired = 0;
        double deadline = now() + 10;
        while (retired < count) {
            unsigned timeout = batch % 5 ? (batch % 2 ? 1 : 1000) : 0;
            int n = nvfio_wait(client, batch % 4 ? DEPTH : 1, done, timeout);
            CHECK(n >= 0);
            if (!n) empty++;
            for (int j = 0; j < n; j++) {
                unsigned i = (uintptr_t)nvfio_request_user(done[j]);
                CHECK(i < count && !(seen & (UINT64_C(1) << i)));
                seen |= UINT64_C(1) << i;
                CHECK(!nvfio_request_error(done[j]));
                CHECK(!memcmp(dma[i], expected[i], 4096));
                retired++;
                completed++;
            }
            CHECK(now() < deadline);
        }
        CHECK(nvfio_wait(client, DEPTH, done, 1000) == 0);
    }

    /* Accepted errors use the same notification path and retire exactly once. */
    CHECK(!nvfio_submit(client, req[0], fd, dma[0], 4096,
                        (uint64_t)st.st_size + 4096, 0));
    double deadline = now() + 10;
    int errors = 0;
    while (!errors) {
        int n = nvfio_wait(client, DEPTH, done, 1000);
        CHECK(n >= 0 && n <= 1);
        if (n) {
            CHECK(done[0] == req[0] && nvfio_request_error(done[0]));
            errors++;
        }
        CHECK(now() < deadline);
    }
    CHECK(nvfio_wait(client, DEPTH, done, 1000) == 0);
    atomic_store(&finished, 1);
    CHECK(!pthread_join(interrupter, NULL));
    CHECK(signals > 0);
    CHECK(!nvfio_client_free(client));
    for (unsigned i = 0; i < DEPTH; i++) {
        nvfio_request_free(req[i]);
        nvfio_dma_free(dma[i]);
    }
    CHECK(!nvfio_close(fd));
    CHECK(!nvfio_stop());
    CHECK(!close(posix));
    printf("API_WAIT_PASSED completions=%lu empty=%lu signals=%d "
           "error_completion=ok full_payload=ok exactly_once=ok\n",
           completed, empty, signals);
    return 0;
}
