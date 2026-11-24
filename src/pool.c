#include "pool.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* pool_stop 이 워커 종료와 종료 신호를 확인하는 간격 */
#define POOL_STOP_CHECK_MS 200

struct worker {
    pthread_t thread;
    struct worker_pool *p;
    int fd;      /* 처리 중인 연결, 없으면 -1 (p->lock) */
    int done;    /* 스레드 함수가 끝남 (p->lock) */
};

static long long mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    struct worker_pool *p = w->p;
    struct conn_item item;

    while (queue_pop(p->q, &item) == QUEUE_OK) {
        pthread_mutex_lock(&p->lock);
        w->fd = item.fd;
        if (p->forced)               /* 강제 종료가 이미 시작됨: 바로 끊기게 */
            shutdown(item.fd, SHUT_RDWR);
        pthread_mutex_unlock(&p->lock);

        p->handler(&item, p->arg);

        /*
         * 기록을 먼저 지우고(잠금 안에서) close 한다. 반대 순서면 close 와 기록 사이에
         * 같은 번호가 다른 워커의 CGI 파이프·파일로 재사용됐을 때 pool_stop 이
         * 엉뚱한 fd 를 shutdown 할 수 있다.
         */
        pthread_mutex_lock(&p->lock);
        w->fd = -1;
        pthread_mutex_unlock(&p->lock);
        close(item.fd);
    }

    pthread_mutex_lock(&p->lock);
    w->done = 1;
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

static void join_all(struct worker_pool *p, int started)
{
    int i;

    queue_close(p->q);
    for (i = 0; i < started; i++)
        pthread_join(p->workers[i].thread, NULL);
    pthread_mutex_destroy(&p->lock);
    free(p->workers);
    p->workers = NULL;
    p->n = 0;
}

int pool_start(struct worker_pool *p, int n, struct conn_queue *q,
               conn_handler handler, void *arg)
{
    sigset_t block, old;
    int i, e = 0;

    p->workers = calloc((size_t)n, sizeof *p->workers);
    if (p->workers == NULL) {
        perror("calloc(workers)");
        return -1;
    }
    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        fprintf(stderr, "pthread_mutex_init(pool)\n");
        free(p->workers);
        p->workers = NULL;
        return -1;
    }
    p->n = n;
    p->q = q;
    p->handler = handler;
    p->arg = arg;
    p->forced = 0;
    for (i = 0; i < n; i++) {
        p->workers[i].p = p;
        p->workers[i].fd = -1;
        p->workers[i].done = 0;
    }

    /* 새 스레드는 만든 스레드의 신호 마스크를 물려받는다 */
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    for (i = 0; i < n; i++) {
        e = pthread_create(&p->workers[i].thread, NULL, worker_main, &p->workers[i]);
        if (e != 0)
            break;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    if (e != 0) {
        fprintf(stderr, "pthread_create (worker %d of %d): %s\n", i + 1, n,
                strerror(e));
        join_all(p, i);
        return -1;
    }
    return 0;
}

int pool_active(struct worker_pool *p)
{
    int i, n = 0;

    pthread_mutex_lock(&p->lock);
    for (i = 0; i < p->n; i++)
        if (p->workers[i].fd >= 0)
            n++;
    pthread_mutex_unlock(&p->lock);
    return n;
}

/* 모든 워커 스레드 함수가 끝났으면 1 */
static int all_done(struct worker_pool *p)
{
    int i, done = 1;

    pthread_mutex_lock(&p->lock);
    for (i = 0; i < p->n; i++)
        if (!p->workers[i].done)
            done = 0;
    pthread_mutex_unlock(&p->lock);
    return done;
}

/* 처리 중인 연결을 모두 shutdown. 잠금 안에서 해서 워커의 close 와 겹치지 않게. */
static int force_close(struct worker_pool *p)
{
    int i, n = 0;

    pthread_mutex_lock(&p->lock);
    p->forced = 1;
    for (i = 0; i < p->n; i++) {
        if (p->workers[i].fd >= 0) {
            shutdown(p->workers[i].fd, SHUT_RDWR);
            n++;
        }
    }
    pthread_mutex_unlock(&p->lock);
    return n;
}

int pool_stop(struct worker_pool *p, int timeout_ms,
              const volatile sig_atomic_t *force_level)
{
    long long deadline = mono_ms() + timeout_ms;
    int forced = 0;

    queue_close(p->q);
    /* 워커는 신호를 막아 두었고, 이 스레드도 잠들어 있으면 신호를 못 보므로 나눠서 확인 */
    while (!all_done(p)) {
        long long left = deadline - mono_ms();
        struct timespec ts;

        if (left <= 0 || *force_level >= 2) {
            forced = force_close(p);
            break;
        }
        if (left > POOL_STOP_CHECK_MS)
            left = POOL_STOP_CHECK_MS;
        ts.tv_sec = 0;
        ts.tv_nsec = (long)left * 1000000L;
        nanosleep(&ts, NULL);        /* 신호에 깨면 EINTR: 그대로 다시 확인 */
    }
    join_all(p, p->n);
    return forced;
}
