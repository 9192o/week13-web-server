#include "server_ev.h"

#include "access_log.h"
#include "cgi.h"
#include "conn_common.h"
#include "event_backend.h"
#include "http.h"
#include "logger.h"
#include "proxy.h"
#include "server_conn.h"
#include "static_file.h"
#include "task_pool.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define EV_DRAIN_TIMEOUT_MS 5000

static uint64_t g_task_seq = 0;

static int conn_build_options(conn_t *c, int close_connection,
                              int timeout_sec) {
    http_response_t res;
    http_response_init(&res);
    res.status = 200;
    res.close_connection = close_connection;

    if (http_response_add_header(&res, "Allow",
                                 "GET, POST, HEAD, OPTIONS") < 0 ||
        http_response_add_header(&res, "Content-Length", "0") < 0 ||
        http_response_add_header(&res, "Connection",
                                 close_connection ? "close" : "keep-alive") < 0)
        return -1;

    size_t cap = HTTP_MAX_RESPONSE_HEADERS + 128;
    char *out = malloc(cap);
    if (out == NULL)
        return -1;

    size_t out_len = 0;
    if (http_response_format(&res, out, cap, &out_len) < 0) {
        free(out);
        return -1;
    }

    conn_common_reset_response(c);
    c->out = out;
    c->out_len = out_len;
    c->out_sent = 0;
    c->head_only = 0;
    c->close_after = close_connection;
    c->state = ST_WRITE;
    c->deadline_ms = now_ms() + (uint64_t)timeout_sec * 1000;
    conn_common_log_response(c, 200, 0);
    return 0;
}

static int conn_build_static(conn_t *c, static_result_t *r,
                             int timeout_sec) {
    size_t cap = HTTP_MAX_RESPONSE_HEADERS + 128 + r->body_len;
    char *out = malloc(cap);
    if (out == NULL) {
        if (r->file_fd >= 0)
            close(r->file_fd);
        return -1;
    }

    size_t hdr_len = 0;
    if (http_response_format(&r->res, out, cap, &hdr_len) < 0) {
        free(out);
        if (r->file_fd >= 0)
            close(r->file_fd);
        return -1;
    }

    size_t out_len = hdr_len;
    if (!r->head_only && r->body_len > 0) {
        memcpy(out + out_len, r->body, r->body_len);
        out_len += r->body_len;
    }

    conn_common_reset_response(c);
    c->out = out;
    c->out_len = out_len;
    c->out_sent = 0;
    c->head_only = r->head_only;
    c->close_after = r->res.close_connection;
    c->state = ST_WRITE;
    c->deadline_ms = now_ms() + (uint64_t)timeout_sec * 1000;

    if (r->file_fd >= 0) {
        if (r->head_only) {
            close(r->file_fd);
        } else {
            if (lseek(r->file_fd, r->file_offset, SEEK_SET) < 0) {
                close(r->file_fd);
                free(c->out);
                c->out = NULL;
                c->out_len = 0;
                return -1;
            }
            c->file_fd = r->file_fd;
            c->file_offset = r->file_offset;
            c->file_remaining = r->content_length;
        }
    }
    conn_common_log_response(c, r->res.status,
                      r->head_only ? 0 : r->body_len + r->content_length);
    return 0;
}

static void *cgi_task_fn(void *arg) {
    cgi_job_t *job = arg;
    cgi_result_t *res = cgi_job_execute(job);
    cgi_job_free(job);
    return res;
}

static conn_t *find_by_task(conn_t *head, uint64_t id) {
    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->task_id == id)
            return c;
    }
    return NULL;
}

static int submit_cgi(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
                      const config_t *cfg, int close_after) {
    char remote[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &c->peer.sin_addr, remote, sizeof remote) == NULL)
        snprintf(remote, sizeof remote, "?");

    cgi_job_t *job = cgi_job_create(&c->req, cfg, remote, close_after);
    if (job == NULL) {
        if (conn_common_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1,
                             c->head_only, 0,
                             cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 0;
    }

    uint64_t id = ++g_task_seq;
    if (task_pool_submit(pool, id, cgi_task_fn, job) < 0) {
        int err = errno;
        cgi_job_free(job);

        int status = err == EAGAIN ? HTTP_SERVICE_UNAVAILABLE
                                   : HTTP_INTERNAL_SERVER_ERROR;
        int retry_after = err == EAGAIN;
        if (conn_common_build_error(c, status, 1, c->head_only, retry_after,
                             cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 0;
    }

    c->task_id = id;
    c->state = ST_WAIT_TASK;
    c->head_only = c->req.method == HTTP_HEAD;
    c->requests++;
    c->deadline_ms = now_ms() +
        (uint64_t)(cfg->request_timeout_sec + 5) * 1000;
    http_request_free(&c->req);

    if (ev_del(loop, c->fd) < 0)
        return -1;
    return 0;
}

static void drain_task_completions(ev_loop_t *loop, task_pool_t *pool,
                                   conn_t *head, const config_t *cfg) {
    int fd = task_pool_completion_fd(pool);
    char buf[256];
    while (read(fd, buf, sizeof buf) > 0)
        ;

    uint64_t id = 0;
    void *data = NULL;
    while (task_pool_pop_result(pool, &id, &data) == 1) {
        cgi_result_t *res = data;
        conn_t *c = find_by_task(head, id);

        if (c != NULL && !c->closed && c->state == ST_WAIT_TASK) {
            if (res == NULL || res->error || res->response == NULL) {
                if (conn_common_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1,
                                     c->head_only, 0,
                                     cfg->request_timeout_sec) < 0) {
                    conn_common_close(loop, c);
                } else if (ev_add(loop, c->fd, EV_WRITE, c) < 0) {
                    conn_common_close(loop, c);
                }
            } else {
                free(c->out);
                c->out = res->response;
                res->response = NULL;
                c->out_len = res->response_len;
                c->out_sent = 0;
                c->close_after = res->close_connection || conn_common_stopping();
                c->file_fd = -1;
                c->file_remaining = 0;
                c->state = ST_WRITE;
                c->deadline_ms = now_ms() +
                    (uint64_t)cfg->request_timeout_sec * 1000;
                conn_common_log_response(c, res->status, res->body_bytes);
                if (ev_add(loop, c->fd, EV_WRITE, c) < 0)
                    conn_common_close(loop, c);
            }
        }

        if (res != NULL)
            cgi_result_free(res);
    }
}

static void free_pending_results(task_pool_t *pool) {
    uint64_t id = 0;
    void *data = NULL;
    while (task_pool_pop_result(pool, &id, &data) == 1)
        cgi_result_free(data);
}

static int process_buffer(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
                          const config_t *cfg) {
    c->body_received = 0;
    http_request_init(&c->req);

    http_status_t st = http_request_parse_buffer(c->in, c->in_len, cfg, &c->req);
    if (st == HTTP_PARSE_INCOMPLETE)
        st = HTTP_BAD_REQUEST;

    int head = (c->req.method == HTTP_HEAD) ||
               conn_common_request_is_head(c->in, c->in_len);

    if (st != HTTP_OK) {
        conn_common_log_begin(c,
                       c->req.method_str[0] != '\0' ? c->req.method_str : "-",
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

    size_t buffered = c->in_len > c->req.header_len
                          ? c->in_len - c->req.header_len : 0;
    int is_cgi = cfg->cgi_dir[0] != '\0' &&
                 strncmp(c->req.path, "/cgi-bin/", 9) == 0;

    if (!is_cgi) {
        route_spec_t *route = config_find_route((config_t *)cfg, c->req.path);
        if (route != NULL && route->type == ROUTE_PROXY) {
            int route_index = (int)(route - cfg->routes);
            int rc = proxy_start(c, route_index, &c->req);
            if (rc < 0) {
                http_request_free(&c->req);
                return -1;
            }

            if (rc == 1) {
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
    }

    if (c->req.chunked) {
        if (conn_common_build_error(c, HTTP_NOT_IMPLEMENTED, 1, head, 0,
                             cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 1;
    }

    int keep = c->req.keep_alive &&
               (c->requests + 1 < cfg->keepalive_max_requests) &&
               !conn_common_stopping();
    int close_after = !keep;
    c->head_only = head;

    if (is_cgi && c->req.content_length > 0 &&
        buffered < c->req.content_length) {
        c->req.body = malloc(c->req.content_length);
        if (c->req.body == NULL) {
            if (conn_common_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1, head, 0,
                                 cfg->request_timeout_sec) < 0) {
                http_request_free(&c->req);
                return -1;
            }
            c->requests++;
            http_request_free(&c->req);
            return 1;
        }

        if (buffered > 0)
            memcpy(c->req.body, c->in + c->req.header_len, buffered);
        c->body_received = buffered;

        size_t consumed = c->req.header_len + buffered;
        if (consumed > c->in_len)
            consumed = c->in_len;
        size_t leftover = c->in_len - consumed;
        if (leftover > 0)
            memmove(c->in, c->in + consumed, leftover);
        c->in_len = leftover;

        c->state = ST_READ_BODY;
        c->deadline_ms = now_ms() +
            (uint64_t)cfg->request_timeout_sec * 1000;

        if (c->req.expect_continue) {
            if (writen(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25) < 0) {
                http_request_free(&c->req);
                return -1;
            }
        }
        return 1;
    }

    if (is_cgi && c->req.content_length > 0) {
        c->req.body = malloc(c->req.content_length);
        if (c->req.body == NULL) {
            if (conn_common_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1, head, 0,
                                 cfg->request_timeout_sec) < 0) {
                http_request_free(&c->req);
                return -1;
            }
            c->requests++;
            http_request_free(&c->req);
            return 1;
        }
        memcpy(c->req.body, c->in + c->req.header_len,
               c->req.content_length);
        c->req.body_len = c->req.content_length;
    }

    if (is_cgi) {
        size_t body_in_buf = buffered < c->req.content_length
                                 ? buffered : c->req.content_length;
        size_t consumed = c->req.header_len + body_in_buf;
        if (consumed > c->in_len)
            consumed = c->in_len;
        size_t leftover = c->in_len - consumed;
        if (leftover > 0)
            memmove(c->in, c->in + consumed, leftover);
        c->in_len = leftover;

        return submit_cgi(c, loop, pool, cfg, close_after);
    }

    if (c->req.content_length > 0 &&
        buffered < c->req.content_length)
        close_after = 1;

    if (c->req.method == HTTP_OPTIONS) {
        if (conn_build_options(c, close_after,
                               cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
    } else {
        static_result_t r;
        handler_result_t rc = static_file_prepare(&c->req, cfg,
                                                  close_after, &r);
        if (rc == HANDLER_ERROR) {
            http_request_free(&c->req);
            return -1;
        }
        if (conn_build_static(c, &r, cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
    }

    c->requests++;

    size_t body_in_buf = 0;
    if (c->req.content_length > 0 && buffered > 0) {
        body_in_buf = buffered < c->req.content_length
                          ? buffered : c->req.content_length;
    }
    size_t consumed = c->req.header_len + body_in_buf;
    if (consumed > c->in_len)
        consumed = c->in_len;

    size_t leftover = c->in_len - consumed;
    if (leftover > 0)
        memmove(c->in, c->in + consumed, leftover);
    c->in_len = leftover;

    http_request_free(&c->req);
    return 1;
}

static int server_process_buffer(conn_t *c, ev_loop_t *loop, void *ctx,
                                 const config_t *cfg) {
    return process_buffer(c, loop, (task_pool_t *)ctx, cfg);
}

static int on_readable(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
                       const config_t *cfg) {
    if (c->state == ST_READ_BODY) {
        while (c->body_received < c->req.content_length) {
            size_t want = c->req.content_length - c->body_received;
            ssize_t n = read(c->fd, c->req.body + c->body_received, want);
            if (n > 0) {
                c->body_received += (size_t)n;
                continue;
            }
            if (n == 0)
                return -1;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }

        c->req.body_len = c->req.content_length;
        int keep = c->req.keep_alive &&
                   (c->requests + 1 < cfg->keepalive_max_requests) &&
                   !conn_common_stopping();
        if (submit_cgi(c, loop, pool, cfg, !keep) < 0)
            return -1;
        if (c->state == ST_WRITE) {
            if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
                return -1;
        }
        return 0;
    }

    return conn_common_read_headers(c, loop, cfg, server_process_buffer, pool);
}

static int on_writable(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
                       const config_t *cfg) {
    return conn_common_on_writable(c, loop, cfg, server_process_buffer, pool);
}

int server_ev_run(const config_t *cfg) {
    g_task_seq = 0;

    /*
     * macOS keeps one signal mask for the whole process, and fork() blocks
     * signals around the fork. A worker's fork can therefore leave SIGINT
     * blocked in the main thread; unblock the shutdown signals before every
     * wait so they are always deliverable.
     */
    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);

    if (conn_common_setup_signals() < 0) {
        log_error("signal setup failed: %s", strerror(errno));
        return -1;
    }

    int listenfd = conn_common_create_listen_socket(cfg->port);
    if (listenfd < 0)
        return -1;

    ev_loop_t *loop = ev_create_backend(getenv("EV_BACKEND"));
    if (loop == NULL) {
        close(listenfd);
        return -1;
    }
    if (ev_add(loop, listenfd, EV_READ, NULL) < 0) {
        close(listenfd);
        ev_destroy(loop);
        return -1;
    }

    task_pool_t *pool = task_pool_create(cfg->task_workers, cfg->task_queue_size);
    if (pool == NULL) {
        close(listenfd);
        ev_destroy(loop);
        return -1;
    }
    int completion_fd = task_pool_completion_fd(pool);
    if (ev_add(loop, completion_fd, EV_READ, NULL) < 0) {
        task_pool_destroy(pool);
        close(listenfd);
        ev_destroy(loop);
        return -1;
    }
    if (ev_add(loop, conn_common_signal_fd(), EV_READ, NULL) < 0) {
        task_pool_destroy(pool);
        close(listenfd);
        ev_destroy(loop);
        return -1;
    }

    if (logger_start(STDERR_FILENO, LOGGER_DEFAULT_QUEUE) < 0) {
        task_pool_destroy(pool);
        close(listenfd);
        ev_destroy(loop);
        conn_common_close_signals();
        return -1;
    }

    if (proxy_init(cfg) < 0) {
        logger_stop();
        task_pool_destroy(pool);
        close(listenfd);
        ev_destroy(loop);
        conn_common_close_signals();
        return -1;
    }

    log_error("listening on 0.0.0.0:%d (loop=%s, task_workers=%d, max_conns=%d, docroot: %s)",
              cfg->port, ev_loop_backend_name(loop), cfg->task_workers,
              cfg->max_conns, cfg->docroot);

    conn_t *head = NULL;
    int count = 0;
    int draining = 0;
    uint64_t drain_deadline = 0;

    while (!conn_common_stopping() || !draining || head != NULL) {
        if (conn_common_stopping() && !draining) {
            draining = 1;
            drain_deadline = now_ms() + EV_DRAIN_TIMEOUT_MS;
            conn_common_start_shutdown_watchdog(EV_DRAIN_TIMEOUT_MS);
            log_error("shutdown: draining %d connections", count);
            conn_common_begin_drain(loop, &listenfd, head);
            conn_common_sweep(&head, &count);
        }

        if (draining && now_ms() >= drain_deadline)
            break;
        if (draining && head == NULL)
            break;

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
            if (events[i].fd == completion_fd) {
                drain_task_completions(loop, pool, head, cfg);
                continue;
            }
            if (events[i].fd == conn_common_signal_fd()) {
                char buf[64];
                while (read(conn_common_signal_fd(), buf, sizeof buf) > 0)
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

            if ((events[i].events & EV_READ) &&
                (c->state == ST_READ || c->state == ST_READ_BODY)) {
                if (on_readable(c, loop, pool, cfg) < 0) {
                    conn_common_close(loop, c);
                    continue;
                }
            }
            if (!c->closed && (events[i].events & EV_WRITE) &&
                c->state == ST_WRITE) {
                if (on_writable(c, loop, pool, cfg) < 0) {
                    conn_common_close(loop, c);
                    continue;
                }
            }

            /*
             * A hangup can arrive with readable data (half-close). Read and
             * queue the response first; only close immediately when there is
             * nothing left to write.
             */
            if (!c->closed && err &&
                (c->state == ST_READ || c->state == ST_READ_BODY))
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

    log_error("server_ev: shutting down pool");
    (void)ev_del(loop, completion_fd);
    (void)ev_del(loop, conn_common_signal_fd());
    task_pool_shutdown(pool);
    free_pending_results(pool);
    task_pool_destroy(pool);

    if (listenfd >= 0) {
        (void)ev_del(loop, listenfd);
        close(listenfd);
    }
    conn_common_close_signals();
    ev_destroy(loop);
    proxy_cleanup();
    log_error("server_ev stopped");
    logger_stop();
    return 0;
}
