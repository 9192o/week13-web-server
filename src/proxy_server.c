#include "proxy_server.h"

#include "conn_common.h"
#include "event_backend.h"
#include "http.h"
#include "logger.h"
#include "proxy.h"
#include "server_conn.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROXY_DRAIN_TIMEOUT_MS 5000

static int process_buffer(conn_t *c, ev_loop_t *loop, void *ctx,
                          const config_t *cfg) {
    (void)loop;
    (void)ctx;

    c->body_received = 0;
    http_request_init(&c->req);

    http_status_t st = http_request_parse_buffer(c->in, c->in_len, cfg, &c->req);
    if (st == HTTP_PARSE_INCOMPLETE)
        st = HTTP_BAD_REQUEST;

    int head = (c->req.method == HTTP_HEAD) ||
               conn_common_request_is_head(c->in, c->in_len);

    if (st != HTTP_OK) {
        conn_common_log_begin(
            c, c->req.method_str[0] != '\0' ? c->req.method_str : "-",
            c->req.path[0] != '\0' ? c->req.path : "-");
        if (conn_common_build_error(c, st, 1, head, 0,
                                    cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 1;
    }

    conn_common_log_begin(c, c->req.method_str, c->req.path);

    route_spec_t *route = config_find_route((config_t *)cfg, c->req.path);
    if (route == NULL || route->type != ROUTE_PROXY) {
        if (conn_common_build_error(c, HTTP_NOT_FOUND, 1, head, 0,
                                    cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 1;
    }

    int route_index = (int)(route - cfg->routes);
    int rc = proxy_start(c, route_index, &c->req);
    if (rc < 0) {
        http_request_free(&c->req);
        return -1;
    }

    if (rc == 1) {
        size_t buffered = c->in_len > c->req.header_len
                              ? c->in_len - c->req.header_len : 0;
        size_t body_len = 0;
        if (c->req.chunked) {
            body_len = buffered;
        } else if (c->req.content_length > 0) {
            body_len = buffered < c->req.content_length
                           ? buffered : c->req.content_length;
        }
        if (body_len > 0)
            memmove(c->in, c->in + c->req.header_len, body_len);
        if (buffered > body_len)
            proxy_mark_close(c);
        c->in_len = body_len;
    } else {
        c->in_len = 0;
    }

    http_request_free(&c->req);
    if (rc == 1)
        proxy_kick(c);
    return 1;
}

int proxy_server_run(const config_t *cfg) {
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

    if (proxy_init(cfg) < 0) {
        logger_stop();
        close(listenfd);
        ev_destroy(loop);
        conn_common_close_signals();
        return -1;
    }

    log_error("reverse-proxy listening on 0.0.0.0:%d (loop=%s, max_conns=%d, routes=%d)",
              cfg->port, ev_loop_backend_name(loop), cfg->max_conns,
              cfg->route_count);

    conn_t *head = NULL;
    int count = 0;
    int draining = 0;
    uint64_t drain_deadline = 0;

    while (!conn_common_stopping() || !draining || head != NULL) {
        if (conn_common_stopping() && !draining) {
            draining = 1;
            drain_deadline = now_ms() + PROXY_DRAIN_TIMEOUT_MS;
            conn_common_start_shutdown_watchdog(PROXY_DRAIN_TIMEOUT_MS);
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

            ev_source_t *src = events[i].ptr;
            if (src == NULL)
                continue;
            if (src->kind == EV_SRC_UPSTREAM) {
                proxy_handle_upstream_event((proxy_conn_t *)src,
                                            events[i].events);
                continue;
            }

            conn_t *c = (conn_t *)src;
            if (c->closed)
                continue;
            if (c->state == ST_PROXY) {
                proxy_handle_client_event(c, events[i].events);
                continue;
            }

            int err = (events[i].events & EV_ERR) != 0;
            if (err)
                c->close_after = 1;

            if ((events[i].events & EV_READ) && c->state == ST_READ) {
                if (conn_common_read_headers(c, loop, cfg, process_buffer,
                                             NULL) < 0) {
                    conn_common_close(loop, c);
                    continue;
                }
            }
            if (!c->closed && (events[i].events & EV_WRITE) &&
                c->state == ST_WRITE) {
                if (conn_common_on_writable(c, loop, cfg, process_buffer,
                                            NULL) < 0) {
                    conn_common_close(loop, c);
                    continue;
                }
            }

            if (!c->closed && err && c->state == ST_READ)
                conn_common_close(loop, c);
        }

        conn_common_sweep(&head, &count);
        conn_common_expire_timers(loop, head, cfg);
        proxy_sweep();
        conn_common_sweep(&head, &count);
        proxy_reap();
    }

    proxy_shutdown_all();

    while (head != NULL) {
        conn_common_close(loop, head);
        conn_common_sweep(&head, &count);
    }

    if (listenfd >= 0) {
        (void)ev_del(loop, listenfd);
        close(listenfd);
    }
    (void)ev_del(loop, sigfd);
    conn_common_close_signals();
    ev_destroy(loop);
    proxy_cleanup();
    log_error("reverse-proxy stopped");
    logger_stop();
    return 0;
}
