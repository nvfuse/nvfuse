#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include "../examples/fio_plugin/modern/completion.h"
#include "../examples/fio_plugin/modern/api_queue.h"
struct request { struct nvfio_call_link link; struct nvfio_completion completion; unsigned input, result, producer; };
static struct nvfio_call_queue pending;
static atomic_int stopping;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static void *owner(void *unused)
{
    (void)unused;
    unsigned expected[4] = {0};
    while (!atomic_load(&stopping)) {
        struct nvfio_call_link *list = nvfio_call_take(&pending);
        if (!list) { sched_yield(); continue; }
        while (list) {
            struct request *r = (struct request *)list;
            list = list->next;
            assert(r->producer < 4 && r->input == expected[r->producer]++);
            r->result = r->input ^ 0xabc123;
            if (nvfio_completion_finish(&r->completion)) {
                pthread_mutex_lock(&mutex);
                pthread_cond_broadcast(&cond);
                pthread_mutex_unlock(&mutex);
            }
        }
    }
    return NULL;
}
static void *producer(void *argument)
{
    unsigned id = *(unsigned *)argument;
    for (unsigned i = 0; i < 25000; i++) {
        struct request r = {.input = i, .producer = id};
        nvfio_call_push(&pending, &r.link);
        for (unsigned spin = 0; spin < i % 256 && !nvfio_completion_ready(&r.completion); spin++)
            atomic_signal_fence(memory_order_seq_cst);
        if (nvfio_completion_park(&r.completion)) {
            pthread_mutex_lock(&mutex);
            while (!nvfio_completion_ready(&r.completion)) pthread_cond_wait(&cond, &mutex);
            pthread_mutex_unlock(&mutex);
        }
        assert(r.result == (i ^ 0xabc123));
    }
    return NULL;
}
int main(void)
{
    struct nvfio_completion early = {0}, parked = {0};
    assert(!nvfio_completion_finish(&early));
    assert(!nvfio_completion_park(&early) && nvfio_completion_ready(&early));
    assert(nvfio_completion_park(&parked));
    assert(nvfio_completion_finish(&parked) && nvfio_completion_ready(&parked));
    pthread_t thread, clients[4]; unsigned ids[4] = {0, 1, 2, 3};
    assert(!pthread_create(&thread, NULL, owner, NULL));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&clients[i], NULL, producer, &ids[i]));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(clients[i], NULL));
    atomic_store(&stopping, 1);
    assert(!pthread_join(thread, NULL));
    puts("PASS MPSC FIFO, request lifetime and park/wake races (4 producers, 100000 requests)");
    return 0;
}
