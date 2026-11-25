#include "conn_loop.h"

#include "logger.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONN_DRAIN_TIMEOUT_MS 5000

int conn_loop_run(const config_t *cfg, const conn_loop_app_t *app, void *ctx) {
    if (cfg == NULL || app == NULL || app->readable == NULL ||
        app->writable == NULL)
        return -1;

    conn_common_set_hooks(app->hooks);

    if (conn_common_setup_signals() < 0) {
        log_error("signal setup failed: %s", strerror(errno));
        return -1;
    }

    int listenfd = conn_common_create_listen_socket(cfg->port);
    if (listenfd < 0) {
        conn_common_close_signals();
        return -1;
    }

    ev_loop_t *loop = ev_create_backend(getenv("EV_BACKEND"));
    if (loop == NULL) {
        close(listenfd);
        conn_common_close_signals();
        return -1;
    }
    if (ev_add(loop, listenfd, EV_READ, NULL) < 0) {
        close(listenfd);
        ev_destroy(loop);
        conn_common_close_signals();
        return -1;
    }

    int sigfd = conn_common_signal_fd();
    if (ev_add(loop, sigfd, EV_READ, NULL) < 0) {
        close(listenfd);
        ev_destroy(loop);
        conn_common_close_signals();
        return -1;
    }

    if (logger_start(STDERR_FILENO, LOGGER_DEFAULT_QUEUE) < 0) {
        close(listenfd);
        ev_destroy(loop);
        conn_common_close_signals();
        return -1;
    }

    int extra_fd = -1;
    int started = 0;
    if (app->start != NULL) {
        extra_fd = app->start(ctx, loop);
        if (extra_fd == -2) {
            logger_stop();
            close(listenfd);
            ev_destroy(loop);
            conn_common_close_signals();
            return -1;
        }
        started = 1;
        if (extra_fd >= 0 && ev_add(loop, extra_fd, EV_READ, NULL) < 0) {
            if (app->stop != NULL)
                app->stop(ctx);
            logger_stop();
            close(listenfd);
            ev_destroy(loop);
            conn_common_close_signals();
            return -1;
        }
    }

    const char *name = app->name != NULL ? app->name : "server";
    log_error("%s listening on 0.0.0.0:%d (loop=%s, max_conns=%d, routes=%d)",
              name, cfg->port, ev_loop_backend_name(loop), cfg->max_conns,
              cfg->route_count);

    conn_t *head = NULL;
    int count = 0;
    int draining = 0;
    uint64_t drain_deadline = 0;

    while (!conn_common_stopping() || !draining || head != NULL) {
        if (conn_common_stopping() && !draining) {
            draining = 1;
            drain_deadline = now_ms() + CONN_DRAIN_TIMEOUT_MS;
            conn_common_start_shutdown_watchdog(CONN_DRAIN_TIMEOUT_MS);
            log_error("shutdown: draining %d connections", count);
            conn_common_begin_drain(loop, &listenfd, head);
            conn_common_sweep(&head, &count);
        }

        if (draining && now_ms() >= drain_deadline)
            break;
        if (draining && head == NULL)
            break;

        sigset_t shutdown_signals;
        sigemptyset(&shutdown_signals);
        sigaddset(&shutdown_signals, SIGINT);
        sigaddset(&shutdown_signals, SIGTERM);
        pthread_sigmask(SIG_UNBLOCK, &shutdown_signals, NULL);

        int timeout = conn_common_next_timeout(head);
        if (draining) {
            uint64_t now = now_ms();
            uint64_t remaining = drain_deadline > now ? drain_deadline - now : 0;
            if (remaining < (uint64_t)timeout)
                timeout = (int)remaining;
        }
        ev_event_t events[64];
        int n = ev_wait(loop, events, 64, timeout);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_error("ev_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            if (listenfd >= 0 && events[i].fd == listenfd) {
                if (events[i].events & EV_READ)
                    conn_common_accept_new(listenfd, loop, &head, &count, cfg);
                continue;
            }
            if (events[i].fd == sigfd) {
                char buf[64];
                while (read(sigfd, buf, sizeof buf) > 0)
                    ;
                continue;
            }
            if (extra_fd >= 0 && events[i].fd == extra_fd) {
                if (app->extra_event != NULL)
                    app->extra_event(ctx, loop, head, events[i].events);
                continue;
            }

            ev_source_t *src = events[i].ptr;
            if (src == NULL)
                continue;
            if (src->kind == EV_SRC_UPSTREAM) {
                if (app->upstream_event != NULL)
                    app->upstream_event(src, events[i].events);
                continue;
            }

            conn_t *c = (conn_t *)src;
            if (c->closed)
                continue;
            if (c->state == ST_PROXY) {
                if (app->client_event != NULL)
                    app->client_event(c, events[i].events);
                else
                    conn_common_close(loop, c);
                continue;
            }

            int err = (events[i].events & EV_ERR) != 0;
            if (err)
                c->close_after = 1;

            if ((events[i].events & EV_READ) &&
                (c->state == ST_READ || c->state == ST_READ_BODY)) {
                if (app->readable(c, loop, ctx, cfg) < 0) {
                    conn_common_close(loop, c);
                    continue;
                }
            }
            if (!c->closed && (events[i].events & EV_WRITE) &&
                c->state == ST_WRITE) {
                if (app->writable(c, loop, ctx, cfg) < 0) {
                    conn_common_close(loop, c);
                    continue;
                }
            }

            if (!c->closed && err &&
                (c->state == ST_READ || c->state == ST_READ_BODY))
                conn_common_close(loop, c);
        }

        conn_common_sweep(&head, &count);
        conn_common_expire_timers(loop, head, cfg);
        if (app->tick != NULL)
            app->tick(ctx);
        conn_common_sweep(&head, &count);
    }

    while (head != NULL) {
        conn_common_close(loop, head);
        conn_common_sweep(&head, &count);
    }

    if (started && app->stop != NULL)
        app->stop(ctx);

    if (extra_fd >= 0)
        (void)ev_del(loop, extra_fd);
    if (listenfd >= 0) {
        (void)ev_del(loop, listenfd);
        close(listenfd);
    }
    (void)ev_del(loop, sigfd);
    ev_destroy(loop);
    conn_common_close_signals();
    log_error("%s stopped", name);
    logger_stop();
    return 0;
}
