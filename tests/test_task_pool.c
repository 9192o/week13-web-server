#include "task_pool.h"
#include "test_harness.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started;
    int release;
} gate_t;

static void gate_init(gate_t *g) {
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
    g->started = 0;
    g->release = 0;
}

static void gate_destroy(gate_t *g) {
    pthread_cond_destroy(&g->cv);
    pthread_mutex_destroy(&g->mu);
}

static void *blocking_task(void *arg) {
    gate_t *g = arg;

    pthread_mutex_lock(&g->mu);
    g->started = 1;
    pthread_cond_broadcast(&g->cv);
    while (!g->release)
        pthread_cond_wait(&g->cv, &g->mu);
    pthread_mutex_unlock(&g->mu);
    return NULL;
}

static void gate_wait_started(gate_t *g) {
    pthread_mutex_lock(&g->mu);
    while (!g->started)
        pthread_cond_wait(&g->cv, &g->mu);
    pthread_mutex_unlock(&g->mu);
}

static void gate_release(gate_t *g) {
    pthread_mutex_lock(&g->mu);
    g->release = 1;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

static void *simple_task(void *arg) {
    intptr_t value = (intptr_t)arg;
    int *out = malloc(sizeof *out);
    if (out != NULL)
        *out = (int)(value * 2);
    return out;
}

static void drain_pipe(int fd) {
    char buf[64];
    while (read(fd, buf, sizeof buf) > 0)
        ;
}

static int collect_results(task_pool_t *pool, int expected,
                           int timeout_ms, int *ids, int *values) {
    int got = 0;

    for (int attempt = 0; attempt < 100 && got < expected; attempt++) {
        struct pollfd pfd;
        pfd.fd = task_pool_completion_fd(pool);
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, timeout_ms);
        if (pr > 0)
            drain_pipe(pfd.fd);

        uint64_t id = 0;
        void *data = NULL;
        while (task_pool_pop_result(pool, &id, &data) == 1) {
            if (got < expected) {
                ids[got] = (int)id;
                values[got] = data != NULL ? *(int *)data : -1;
                got++;
            }
            free(data);
            data = NULL;
        }
    }
    return got;
}

void test_task_pool_basic(void) {
    task_pool_t *pool = task_pool_create(2, 8);
    TEST_ASSERT(pool != NULL);
    if (pool == NULL)
        return;

    for (int i = 1; i <= 4; i++)
        TEST_ASSERT_EQ_INT(
            task_pool_submit(pool, (uint64_t)i, simple_task,
                             (void *)(intptr_t)i), 0);

    int ids[4] = {0};
    int values[4] = {0};
    int got = collect_results(pool, 4, 1000, ids, values);
    TEST_ASSERT_EQ_INT(got, 4);

    int seen[5] = {0};
    for (int i = 0; i < got; i++) {
        if (ids[i] >= 1 && ids[i] <= 4) {
            seen[ids[i]] = 1;
            TEST_ASSERT_EQ_INT(values[i], ids[i] * 2);
        }
    }
    for (int i = 1; i <= 4; i++)
        TEST_ASSERT_EQ_INT(seen[i], 1);

    TEST_ASSERT_EQ_INT(task_pool_pending(pool), 0);
    task_pool_destroy(pool);
}

void test_task_pool_queue_full(void) {
    task_pool_t *pool = task_pool_create(1, 1);
    TEST_ASSERT(pool != NULL);
    if (pool == NULL)
        return;

    gate_t gate;
    gate_init(&gate);

    TEST_ASSERT_EQ_INT(
        task_pool_submit(pool, 1, blocking_task, &gate), 0);
    gate_wait_started(&gate);

    /* One worker is busy, queue size is 1: this fills the queue. */
    TEST_ASSERT_EQ_INT(
        task_pool_submit(pool, 2, simple_task, (void *)(intptr_t)2), 0);

    errno = 0;
    TEST_ASSERT_EQ_INT(
        task_pool_submit(pool, 3, simple_task, (void *)(intptr_t)3), -1);
    TEST_ASSERT_EQ_INT(errno, EAGAIN);

    gate_release(&gate);

    int ids[3] = {0};
    int values[3] = {0};
    int got = collect_results(pool, 2, 1000, ids, values);
    TEST_ASSERT_EQ_INT(got, 2);

    int seen[3] = {0};
    for (int i = 0; i < got; i++) {
        if (ids[i] == 1) {
            seen[1] = 1;
            TEST_ASSERT_EQ_INT(values[i], -1);
        } else if (ids[i] == 2) {
            seen[2] = 1;
            TEST_ASSERT_EQ_INT(values[i], 4);
        }
    }
    TEST_ASSERT_EQ_INT(seen[1], 1);
    TEST_ASSERT_EQ_INT(seen[2], 1);

    task_pool_destroy(pool);
    gate_destroy(&gate);
}

void test_task_pool_timeout(void) {
    task_pool_t *pool = task_pool_create(1, 2);
    TEST_ASSERT(pool != NULL);
    if (pool == NULL)
        return;

    struct pollfd pfd;
    pfd.fd = task_pool_completion_fd(pool);
    pfd.events = POLLIN;
    pfd.revents = 0;
    TEST_ASSERT_EQ_INT(poll(&pfd, 1, 50), 0);

    task_pool_destroy(pool);
}

void test_task_pool_empty(void) {
    task_pool_t *pool = task_pool_create(1, 2);
    TEST_ASSERT(pool != NULL);
    if (pool == NULL)
        return;

    uint64_t id = 0;
    void *data = NULL;
    TEST_ASSERT_EQ_INT(task_pool_pop_result(pool, &id, &data), 0);

    task_pool_destroy(pool);
}

void test_task_pool_invalid(void) {
    errno = 0;
    TEST_ASSERT(task_pool_create(0, 1) == NULL);
    TEST_ASSERT_EQ_INT(errno, EINVAL);

    errno = 0;
    TEST_ASSERT(task_pool_create(1, 0) == NULL);
    TEST_ASSERT_EQ_INT(errno, EINVAL);

    errno = 0;
    TEST_ASSERT_EQ_INT(task_pool_submit(NULL, 1, simple_task, NULL), -1);
    TEST_ASSERT_EQ_INT(errno, EINVAL);

    errno = 0;
    TEST_ASSERT_EQ_INT(task_pool_pending(NULL), -1);
    TEST_ASSERT_EQ_INT(errno, EINVAL);
}
