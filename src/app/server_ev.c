#include "server_ev.h"

#include "conn_common.h"
#include "conn_loop.h"
#ifndef BACKEND_ONLY
#include "proxy.h"
#endif
#include "server_internal.h"
#include "task_pool.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

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
#ifndef BACKEND_ONLY
    server_app_ctx *app = ctx;
    if (app->cfg->mode == SERVER_MODE_BACKEND)
        return;
    proxy_sweep();
    proxy_reap();
#else
    (void)ctx;
#endif
}

#ifndef BACKEND_ONLY
static void server_upstream_event(void *p, uint32_t events) {
    proxy_handle_upstream_event((proxy_conn_t *)p, events);
}
#endif

static int server_start(void *ctx, ev_loop_t *loop) {
    (void)loop;
    server_app_ctx *app = ctx;
#ifndef BACKEND_ONLY
    if (app->cfg->mode != SERVER_MODE_BACKEND && proxy_init(app->cfg) < 0) {
        proxy_cleanup();
        return -2;
    }
#endif
    app->pool = task_pool_create(app->cfg->task_workers,
                                 app->cfg->task_queue_size);
    if (app->pool == NULL) {
#ifndef BACKEND_ONLY
        if (app->cfg->mode != SERVER_MODE_BACKEND)
            proxy_cleanup();
#endif
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
#ifndef BACKEND_ONLY
    if (app->cfg->mode != SERVER_MODE_BACKEND) {
        proxy_shutdown_all();
        proxy_cleanup();
    }
#endif
}

int server_ev_run(const config_t *cfg) {
#ifndef BACKEND_ONLY
    static const conn_common_hooks_t hooks = {
        proxy_detach, proxy_next_timeout_ms, proxy_upstream_connections
    };
#define SERVER_HOOKS &hooks
#define SERVER_CLIENT_EVENT proxy_handle_client_event
#define SERVER_UPSTREAM_EVENT server_upstream_event
#else
#define SERVER_HOOKS NULL
#define SERVER_CLIENT_EVENT NULL
#define SERVER_UPSTREAM_EVENT NULL
#endif
    server_app_ctx app = {.cfg = cfg, .pool = NULL};
    const conn_loop_app_t loop_app = {
        .name = cfg->mode == SERVER_MODE_BACKEND ? "backend-server" : "server",
        .hooks = SERVER_HOOKS,
        .start = server_start,
        .stop = server_stop,
        .extra_event = server_extra_event,
        .tick = server_tick,
        .readable = server_readable,
        .writable = server_writable,
        .client_event = SERVER_CLIENT_EVENT,
        .upstream_event = SERVER_UPSTREAM_EVENT,
    };
    return conn_loop_run(cfg, &loop_app, &app);
#undef SERVER_HOOKS
#undef SERVER_CLIENT_EVENT
#undef SERVER_UPSTREAM_EVENT
}
