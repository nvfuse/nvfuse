/* SPDX-License-Identifier: GPL-2.0-only */

/* Exercise the real primary reactor SPSC entry points without device I/O. */
#include "spdk/stdinc.h"
#include "nvfuse_core.h"
#include "nvfuse_reactor.h"
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

#define TRANSFERS 2000000U
#define WINDOW 128U
static struct reactor_task task;
static struct io_job jobs[1024];

static void *consume(void *unused)
{
    (void)unused;
    for (unsigned sequence = 0; sequence < TRANSFERS; sequence++) {
        struct io_job *job;
        while (!(job = reactor_sq_get_req(&task))) {}
        CHECK(job->offset == sequence);
        CHECK((unsigned)job->bytes == (sequence ^ 0xabcdefU));
        CHECK(!reactor_cq_put_req(&task, job));
    }
    return NULL;
}

int main(void)
{
    struct io_target target = {0};
    struct io_job *batch[1024], *completed[WINDOW];
    task.target = &target; task.qdepth = 513; task.polling = 1;
    for (unsigned i = 0; i < 1024; i++) batch[i] = &jobs[i];
    CHECK(!reactor_sq_get_req(&task));
    CHECK(!reactor_submit_reqs(&target, &task, batch, 512));
    int saved_head = task.sq_head;
    CHECK(reactor_submit_reqs(&target, &task, batch, 2) < 0);
    CHECK(task.sq_head == saved_head); /* failed batch published nothing */
    for (unsigned i = 0; i < 512; i++) CHECK(reactor_sq_get_req(&task) == batch[i]);
    CHECK(!reactor_sq_get_req(&task));

    pthread_t consumer;
    CHECK(!pthread_create(&consumer, NULL, consume, NULL));
    unsigned submitted = 0, done = 0;
    while (done < TRANSFERS) {
        unsigned count = 1 + submitted % 64;
        if (count > TRANSFERS - submitted) count = TRANSFERS - submitted;
        if (count > WINDOW - (submitted - done)) count = WINDOW - (submitted - done);
        for (unsigned i = 0; i < count; i++) {
            struct io_job *job = &jobs[(submitted + i) % 1024];
            job->offset = submitted + i;
            job->bytes = (submitted + i) ^ 0xabcdefU;
            batch[i] = job;
        }
        if (count) {
            CHECK(!reactor_submit_reqs(&target, &task, batch, count));
            submitted += count;
        }
        int n = reactor_cq_get_reqs(&task, completed, 0, WINDOW);
        CHECK(n >= 0 && (unsigned)n <= submitted - done);
        for (int i = 0; i < n; i++) {
            CHECK(completed[i]->offset == done);
            CHECK((unsigned)completed[i]->bytes == (done ^ 0xabcdefU));
            done++;
        }
    }
    CHECK(!pthread_join(consumer, NULL));
    CHECK(task.sq_head == task.sq_tail && !reactor_cq_size(&task));
    puts("PASS primary reactor: 2M ordered SPSC round trips, wraparound and full-batch rollback");
    return 0;
}
