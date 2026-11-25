#include "conn_internal.h"

#include "http.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int conn_common_next_timeout(conn_t *head) {
    uint64_t now = now_ms();
    uint64_t min = UINT64_MAX;
    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->state == ST_STREAM)
            continue;
        if (c->deadline_ms < min)
            min = c->deadline_ms;
    }

    int extra_ms = conn_common_hook_next_timeout_ms();
    if (extra_ms >= 0 && (uint64_t)extra_ms < min)
        min = (uint64_t)extra_ms;

    if (min == UINT64_MAX)
        return 1000;
    if (min <= now)
        return 0;

    uint64_t d = min - now;
    return d > (uint64_t)INT_MAX ? INT_MAX : (int)d;
}

void conn_common_expire_timers(ev_loop_t *loop, conn_t *head,
                               const config_t *cfg) {
    uint64_t now = now_ms();
    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->closed || now < c->deadline_ms)
            continue;
        if (c->state == ST_STREAM)
            continue;

        if (c->state == ST_READ && c->in_len == 0 && c->requests > 0) {
            conn_common_close(loop, c);
            continue;
        }

        if (c->state == ST_READ || c->state == ST_READ_BODY) {
            if (!c->log_has_request)
                conn_common_log_begin(c, "-", "-");
            if (conn_common_build_error(c, HTTP_REQUEST_TIMEOUT, 1,
                                        conn_common_request_is_head(
                                            c->in, c->in_len),
                                        0, cfg->request_timeout_sec) < 0) {
                conn_common_close(loop, c);
                continue;
            }
            c->requests++;
            if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
                conn_common_close(loop, c);
            continue;
        }

        conn_common_close(loop, c);
    }
}

static void *shutdown_watchdog(void *arg) {
    int timeout_ms = *(int *)arg;
    free(arg);
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
    _exit(0);
    return NULL;
}

void conn_common_start_shutdown_watchdog(int timeout_ms) {
    pthread_t thread;
    int *arg = malloc(sizeof *arg);
    if (arg == NULL)
        return;
    *arg = timeout_ms;
    if (pthread_create(&thread, NULL, shutdown_watchdog, arg) == 0)
        pthread_detach(thread);
    else
        free(arg);
}

void conn_common_begin_drain(ev_loop_t *loop, int *listenfd, conn_t *head) {
    if (*listenfd >= 0) {
        (void)ev_del(loop, *listenfd);
        close(*listenfd);
        *listenfd = -1;
    }

    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->closed)
            continue;
        if (c->state == ST_READ || c->state == ST_READ_BODY)
            conn_common_close(loop, c);
        else
            c->close_after = 1;
    }
}
