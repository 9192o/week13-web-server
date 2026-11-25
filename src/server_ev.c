#include "server_ev.h"

#include "access_log.h"
#include "cgi.h"
#include "conn_common.h"
#include "conn_loop.h"
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

typedef struct {
    const config_t *cfg;
    task_pool_t *pool;
} server_app_ctx;

static int server_process_buffer(conn_t *c, ev_loop_t *loop, void *ctx,
                                 const config_t *cfg) {
    server_app_ctx *app = ctx;
    return process_buffer(c, loop, app->pool, cfg);
}

static int server_readable(conn_t *c, ev_loop_t *loop, void *ctx,
                           const config_t *cfg) {
    server_app_ctx *app = ctx;
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
        if (submit_cgi(c, loop, app->pool, cfg, !keep) < 0)
            return -1;
        if (c->state == ST_WRITE) {
            if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
                return -1;
        }
        return 0;
    }

    return conn_common_read_headers(c, loop, cfg, server_process_buffer, ctx);
}

static int server_writable(conn_t *c, ev_loop_t *loop, void *ctx,
                           const config_t *cfg) {
    return conn_common_on_writable(c, loop, cfg, server_process_buffer, ctx);
}

static void server_extra_event(void *ctx, ev_loop_t *loop, conn_t *head,
                               uint32_t events) {
    (void)events;
    server_app_ctx *app = ctx;
    drain_task_completions(loop, app->pool, head, app->cfg);
}

static void server_tick(void *ctx) {
    (void)ctx;
    proxy_sweep();
    proxy_reap();
}

static void server_upstream_event(void *p, uint32_t events) {
    proxy_handle_upstream_event((proxy_conn_t *)p, events);
}

static int server_start(void *ctx, ev_loop_t *loop) {
    (void)loop;
    server_app_ctx *app = ctx;
    if (proxy_init(app->cfg) < 0) {
        proxy_cleanup();
        return -2;
    }
    app->pool = task_pool_create(app->cfg->task_workers,
                                 app->cfg->task_queue_size);
    if (app->pool == NULL) {
        proxy_cleanup();
        return -2;
    }
    return task_pool_completion_fd(app->pool);
}

static void server_stop(void *ctx) {
    server_app_ctx *app = ctx;
    if (app->pool != NULL) {
        task_pool_shutdown(app->pool);
        free_pending_results(app->pool);
        task_pool_destroy(app->pool);
        app->pool = NULL;
    }
    proxy_shutdown_all();
    proxy_cleanup();
}

int server_ev_run(const config_t *cfg) {
    static const conn_common_hooks_t hooks = {
        proxy_detach, proxy_next_timeout_ms
    };
    server_app_ctx app = {.cfg = cfg, .pool = NULL};
    const conn_loop_app_t loop_app = {
        .name = "server",
        .hooks = &hooks,
        .start = server_start,
        .stop = server_stop,
        .extra_event = server_extra_event,
        .tick = server_tick,
        .readable = server_readable,
        .writable = server_writable,
        .client_event = proxy_handle_client_event,
        .upstream_event = server_upstream_event,
    };
    return conn_loop_run(cfg, &loop_app, &app);
}
