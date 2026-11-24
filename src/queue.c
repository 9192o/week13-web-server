#include "queue.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int queue_init(struct conn_queue *q, int cap)
{
    if (cap < 1)
        return -1;
    q->items = malloc((size_t)cap * sizeof *q->items);
    if (q->items == NULL)
        return -1;
    q->cap = cap;
    q->head = 0;
    q->len = 0;
    q->closed = 0;
    if (pthread_mutex_init(&q->lock, NULL) != 0)
        goto fail_mutex;
    if (pthread_cond_init(&q->not_empty, NULL) != 0)
        goto fail_empty;
    if (pthread_cond_init(&q->not_full, NULL) != 0)
        goto fail_full;
    return 0;

fail_full:
    pthread_cond_destroy(&q->not_empty);
fail_empty:
    pthread_mutex_destroy(&q->lock);
fail_mutex:
    free(q->items);
    q->items = NULL;
    return -1;
}

void queue_destroy(struct conn_queue *q)
{
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->items);
    q->items = NULL;
}

/*
 * 지금부터 ms 뒤의 절대 시각 (pthread_cond_timedwait 용).
 * macOS 에는 pthread_condattr_setclock 이 없어 CLOCK_REALTIME 기준으로 쓴다.
 */
static struct timespec abs_after(int ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

enum queue_result queue_push(struct conn_queue *q, const struct conn_item *item,
                             int timeout_ms)
{
    struct timespec until = abs_after(timeout_ms);
    enum queue_result r = QUEUE_OK;

    pthread_mutex_lock(&q->lock);
    while (!q->closed && q->len == q->cap) {
        int e = pthread_cond_timedwait(&q->not_full, &q->lock, &until);
        if (e == ETIMEDOUT && !q->closed && q->len == q->cap) {
            r = QUEUE_TIMEOUT;
            break;
        }
    }
    if (r == QUEUE_OK) {
        if (q->closed) {
            r = QUEUE_CLOSED;
        } else {
            q->items[(q->head + q->len) % q->cap] = *item;
            q->len++;
            pthread_cond_signal(&q->not_empty);
        }
    }
    pthread_mutex_unlock(&q->lock);
    return r;
}

enum queue_result queue_pop(struct conn_queue *q, struct conn_item *out)
{
    enum queue_result r = QUEUE_OK;

    pthread_mutex_lock(&q->lock);
    while (!q->closed && q->len == 0)
        pthread_cond_wait(&q->not_empty, &q->lock);
    if (q->closed) {
        r = QUEUE_CLOSED;
    } else {
        *out = q->items[q->head];
        q->head = (q->head + 1) % q->cap;
        q->len--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->lock);
    return r;
}

void queue_close(struct conn_queue *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

int queue_drain(struct conn_queue *q)
{
    int n = 0;

    pthread_mutex_lock(&q->lock);
    while (q->len > 0) {
        close(q->items[q->head].fd);
        q->head = (q->head + 1) % q->cap;
        q->len--;
        n++;
    }
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return n;
}
