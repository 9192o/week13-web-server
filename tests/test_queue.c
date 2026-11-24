#include "queue.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static struct conn_item item(int fd)
{
    struct conn_item it;

    it.fd = fd;
    snprintf(it.peer, sizeof it.peer, "1.2.3.4:%d", fd);
    return it;
}

static long long mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static void test_init_rejects_zero(void)
{
    struct conn_queue q;
    CHECK_EQ_INT(queue_init(&q, 0), -1);
}

static void test_fifo_and_wraparound(void)
{
    struct conn_queue q;
    struct conn_item it, out;
    int i;

    CHECK_EQ_INT(queue_init(&q, 3), 0);
    /* 여러 바퀴 돌려서 head 가 끝에서 0 으로 넘어가도 순서가 맞는지 */
    for (i = 0; i < 10; i++) {
        it = item(100 + i);
        CHECK_EQ_INT(queue_push(&q, &it, 0), QUEUE_OK);
        if (i % 2 == 1) {
            CHECK_EQ_INT(queue_pop(&q, &out), QUEUE_OK);
            CHECK_EQ_INT(out.fd, 100 + i - 1);
            CHECK_EQ_INT(queue_pop(&q, &out), QUEUE_OK);
            CHECK_EQ_INT(out.fd, 100 + i);
        }
    }
    it = item(7);
    CHECK_EQ_INT(queue_push(&q, &it, 0), QUEUE_OK);
    CHECK_EQ_INT(queue_pop(&q, &out), QUEUE_OK);
    CHECK_EQ_STR(out.peer, "1.2.3.4:7");
    queue_destroy(&q);
}

static void test_push_full_times_out(void)
{
    struct conn_queue q;
    struct conn_item it = item(1);
    long long t0, el;

    CHECK_EQ_INT(queue_init(&q, 2), 0);
    CHECK_EQ_INT(queue_push(&q, &it, 0), QUEUE_OK);
    CHECK_EQ_INT(queue_push(&q, &it, 0), QUEUE_OK);
    t0 = mono_ms();
    CHECK_EQ_INT(queue_push(&q, &it, 200), QUEUE_TIMEOUT);
    el = mono_ms() - t0;
    CHECK(el >= 150 && el < 1000);
    CHECK_EQ_INT(q.len, 2);
    queue_destroy(&q);
}

static void test_closed_pop_ignores_remaining(void)
{
    struct conn_queue q;
    struct conn_item it = item(5), out;

    CHECK_EQ_INT(queue_init(&q, 2), 0);
    CHECK_EQ_INT(queue_push(&q, &it, 0), QUEUE_OK);
    queue_close(&q);
    CHECK_EQ_INT(queue_pop(&q, &out), QUEUE_CLOSED);
    CHECK_EQ_INT(queue_push(&q, &it, 0), QUEUE_CLOSED);
    CHECK_EQ_INT(q.len, 1);       /* 남은 항목은 drain 이 처리 */
    q.len = 0;                    /* fd 5 는 가짜라 drain 하지 않음 */
    queue_destroy(&q);
}

static void *pop_thread(void *arg)
{
    static enum queue_result r;
    struct conn_item out;

    r = queue_pop(arg, &out);
    return &r;
}

static void test_close_wakes_blocked_pop(void)
{
    struct conn_queue q;
    pthread_t th;
    void *res;

    CHECK_EQ_INT(queue_init(&q, 2), 0);
    CHECK_EQ_INT(pthread_create(&th, NULL, pop_thread, &q), 0);
    sleep_ms(50);                 /* 스레드가 빈 큐에서 잠들 시간 */
    queue_close(&q);
    pthread_join(th, &res);
    CHECK_EQ_INT(*(enum queue_result *)res, QUEUE_CLOSED);
    queue_destroy(&q);
}

static void *push_thread(void *arg)
{
    static enum queue_result r;
    struct conn_item it = item(9);

    r = queue_push(arg, &it, 5000);
    return &r;
}

static void test_pop_wakes_blocked_push(void)
{
    struct conn_queue q;
    struct conn_item it = item(8), out;
    pthread_t th;
    void *res;

    CHECK_EQ_INT(queue_init(&q, 1), 0);
    CHECK_EQ_INT(queue_push(&q, &it, 0), QUEUE_OK);
    CHECK_EQ_INT(pthread_create(&th, NULL, push_thread, &q), 0);
    sleep_ms(50);                 /* 가득 찬 큐에서 기다리게 */
    CHECK_EQ_INT(queue_pop(&q, &out), QUEUE_OK);
    CHECK_EQ_INT(out.fd, 8);
    pthread_join(th, &res);
    CHECK_EQ_INT(*(enum queue_result *)res, QUEUE_OK);
    CHECK_EQ_INT(queue_pop(&q, &out), QUEUE_OK);
    CHECK_EQ_INT(out.fd, 9);
    queue_destroy(&q);
}

static void test_drain_closes_fds(void)
{
    struct conn_queue q;
    struct conn_item it;
    int p[2];

    CHECK_EQ_INT(pipe(p), 0);
    CHECK_EQ_INT(queue_init(&q, 4), 0);
    it = item(p[0]);
    CHECK_EQ_INT(queue_push(&q, &it, 0), QUEUE_OK);
    it = item(p[1]);
    CHECK_EQ_INT(queue_push(&q, &it, 0), QUEUE_OK);
    queue_close(&q);
    CHECK_EQ_INT(queue_drain(&q), 2);
    CHECK_EQ_INT(q.len, 0);
    CHECK(fcntl(p[0], F_GETFD) == -1 && errno == EBADF);
    CHECK(fcntl(p[1], F_GETFD) == -1 && errno == EBADF);
    queue_destroy(&q);
}

int main(void)
{
    RUN_TEST(test_init_rejects_zero);
    RUN_TEST(test_fifo_and_wraparound);
    RUN_TEST(test_push_full_times_out);
    RUN_TEST(test_closed_pop_ignores_remaining);
    RUN_TEST(test_close_wakes_blocked_pop);
    RUN_TEST(test_pop_wakes_blocked_push);
    RUN_TEST(test_drain_closes_fds);
    return TEST_SUMMARY();
}
