#include "pool.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *worker_main(void *arg)
{
    struct worker_pool *p = arg;
    struct conn_item item;

    while (queue_pop(p->q, &item) == QUEUE_OK)
        p->handler(&item, p->arg);
    return NULL;
}

static void join_all(struct worker_pool *p, int started)
{
    int i;

    queue_close(p->q);
    for (i = 0; i < started; i++)
        pthread_join(p->threads[i], NULL);
    free(p->threads);
    p->threads = NULL;
    p->n = 0;
}

int pool_start(struct worker_pool *p, int n, struct conn_queue *q,
               conn_handler handler, void *arg)
{
    sigset_t block, old;
    int i, e = 0;

    p->threads = malloc((size_t)n * sizeof *p->threads);
    if (p->threads == NULL) {
        perror("malloc(worker threads)");
        return -1;
    }
    p->n = n;
    p->q = q;
    p->handler = handler;
    p->arg = arg;

    /* 새 스레드는 만든 스레드의 신호 마스크를 물려받는다 */
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    for (i = 0; i < n; i++) {
        e = pthread_create(&p->threads[i], NULL, worker_main, p);
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

void pool_stop(struct worker_pool *p)
{
    join_all(p, p->n);
}
