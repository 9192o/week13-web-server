#include "proxy_server.h"

#include "conn_common.h"
#include "conn_loop.h"
#include "http.h"
#include "proxy.h"
#include "server_conn.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const config_t *cfg;
} proxy_app_ctx;

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

static int proxy_app_readable(conn_t *c, ev_loop_t *loop, void *ctx,
                              const config_t *cfg) {
    (void)ctx;
    return conn_common_read_headers(c, loop, cfg, process_buffer, NULL);
}

static int proxy_app_writable(conn_t *c, ev_loop_t *loop, void *ctx,
                              const config_t *cfg) {
    (void)ctx;
    return conn_common_on_writable(c, loop, cfg, process_buffer, NULL);
}

static void proxy_app_tick(void *ctx) {
    (void)ctx;
    proxy_sweep();
    proxy_reap();
}

static void proxy_app_upstream_event(void *p, uint32_t events) {
    proxy_handle_upstream_event((proxy_conn_t *)p, events);
}

static int proxy_app_start(void *ctx, ev_loop_t *loop) {
    (void)loop;
    proxy_app_ctx *app = ctx;
    if (proxy_init(app->cfg) < 0) {
        proxy_cleanup();
        return -2;
    }
    return -1;
}

static void proxy_app_stop(void *ctx) {
    (void)ctx;
    proxy_shutdown_all();
    proxy_cleanup();
}

int proxy_server_run(const config_t *cfg) {
    static const conn_common_hooks_t hooks = {
        proxy_detach, proxy_next_timeout_ms
    };
    proxy_app_ctx app = {.cfg = cfg};
    const conn_loop_app_t loop_app = {
        .name = "reverse-proxy",
        .hooks = &hooks,
        .start = proxy_app_start,
        .stop = proxy_app_stop,
        .tick = proxy_app_tick,
        .readable = proxy_app_readable,
        .writable = proxy_app_writable,
        .client_event = proxy_handle_client_event,
        .upstream_event = proxy_app_upstream_event,
    };
    return conn_loop_run(cfg, &loop_app, &app);
}
