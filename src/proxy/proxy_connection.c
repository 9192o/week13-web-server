#include "proxy_core.h"

#include "event_backend.h"
#include "http.h"
#include "proxy_wire.h"
#include "server_conn.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static proxy_conn_t *g_active;
static proxy_conn_t *g_dead;
static int g_active_upstreams;

int proxy_init(const config_t *cfg) {
    g_active = NULL;
    g_dead = NULL;
    g_active_upstreams = 0;
    return proxy_route_init(cfg);
}

void proxy_cleanup(void) {
    proxy_route_cleanup();
    g_active = NULL;
    g_active_upstreams = 0;
    proxy_reap();
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int proxy_set_events(conn_t *c, int fd, uint32_t events, void *ptr,
                            int *in_loop) {
    if (events == 0) {
        if (*in_loop) {
            (void)ev_del(c->loop, fd);
            *in_loop = 0;
        }
        return 0;
    }
    if (*in_loop)
        return ev_mod(c->loop, fd, events, ptr);
    int rc = ev_add(c->loop, fd, events, ptr);
    if (rc == 0)
        *in_loop = 1;
    return rc;
}

static void proxy_list_add(proxy_conn_t *p) {
    p->next = g_active;
    g_active = p;
}

static void proxy_list_remove(proxy_conn_t *p) {
    proxy_conn_t **pp = &g_active;
    while (*pp != NULL) {
        if (*pp == p) {
            *pp = p->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

void proxy_close_upstream(proxy_conn_t *p) {
    if (p->upstream_fd >= 0) {
        if (p->upstream_in_loop && p->client != NULL) {
            (void)ev_del(p->client->loop, p->upstream_fd);
            p->upstream_in_loop = 0;
        }
        close(p->upstream_fd);
        p->upstream_fd = -1;
    }
}

static void proxy_free_struct(proxy_conn_t *p) {
    if (p == NULL || p->dead)
        return;
    if (p->client != NULL && p->client->proxy == p)
        p->client->proxy = NULL;
    proxy_list_remove(p);
    if (g_active_upstreams > 0)
        g_active_upstreams--;
    p->dead = 1;
    p->next_dead = g_dead;
    g_dead = p;
}

void proxy_reap(void) {
    while (g_dead != NULL) {
        proxy_conn_t *p = g_dead;
        g_dead = p->next_dead;
        free(p->req_out);
        free(p->resp_out);
        free(p->resp_hdr);
        free(p);
    }
}

void proxy_abort(proxy_conn_t *p) {
    if (p == NULL || p->dead)
        return;
    conn_t *c = p->client;
    if (p->backend != NULL)
        log_error("proxy: %s:%d aborted (state=%d)",
                  p->backend->host, p->backend->port, p->state);
    proxy_close_upstream(p);
    proxy_free_struct(p);
    if (c != NULL)
        server_conn_close(c);
}

void proxy_fail(proxy_conn_t *p, int status) {
    if (p == NULL || p->dead)
        return;
    conn_t *c = p->client;
    int started = p->response_started;
    if (p->backend != NULL)
        log_error("proxy: %s:%d failed with %d (state=%d)",
                  p->backend->host, p->backend->port, status, p->state);
    proxy_close_upstream(p);

    if (!started && c != NULL) {
        int rc = server_conn_build_error(c, status, 1, p->head_only, 0);
        proxy_free_struct(p);
        if (rc == 0) {
            if (proxy_set_events(c, c->fd, EV_WRITE, c, &c->in_loop) < 0)
                server_conn_close(c);
            return;
        }
    }

    proxy_free_struct(p);
    if (c != NULL)
        server_conn_close(c);
}

void proxy_finish(proxy_conn_t *p) {
    conn_t *c = p->client;
    int keep = p->client_keep_alive && !p->close_client &&
               !server_is_stopping();

    server_conn_log_response(c, p->status, p->body_bytes);
    server_conn_log_end(c);

    if (p->upstream_keep_alive && p->upstream_fd >= 0) {
        if (p->upstream_in_loop) {
            (void)ev_del(c->loop, p->upstream_fd);
            p->upstream_in_loop = 0;
        }
        int fd = p->upstream_fd;
        p->upstream_fd = -1;
        proxy_pool_release(p->backend, fd);
    } else {
        proxy_close_upstream(p);
    }

    proxy_free_struct(p);

    if (keep) {
        if (c->in_len > 0) {
            server_conn_close(c);
            return;
        }
        c->state = ST_READ;
        c->close_after = 0;
        c->deadline_ms = now_ms() +
            (uint64_t)(c->in_len == 0 ? c->cfg->keepalive_idle_sec
                                      : c->cfg->request_timeout_sec) * 1000;
        if (proxy_set_events(c, c->fd, EV_READ, c, &c->in_loop) < 0)
            server_conn_close(c);
    } else {
        server_conn_close(c);
    }
}

void proxy_update_events(proxy_conn_t *p) {
    if (p == NULL || p->dead || p->client == NULL || p->client->closed) {
        proxy_abort(p);
        return;
    }

    if (p->state == PROXY_SENDING &&
        p->req_out_sent == p->req_out_len &&
        p->c2u_len == 0 && p->req_body_done) {
        p->state = PROXY_READING_HEADERS;
        p->deadline_ms = now_ms() + (uint64_t)(proxy_config()->timeout_response_sec * 1000);
    }

    conn_t *c = p->client;
    uint32_t cev = 0;
    if (p->continue_pending)
        cev |= EV_WRITE;
    if (p->state == PROXY_SENDING && !p->req_body_done &&
        p->c2u_len < sizeof p->c2u)
        cev |= EV_READ;
    if (p->resp_out_sent < p->resp_out_len ||
        p->u2c_len > p->u2c_off)
        cev |= EV_WRITE;

    if (proxy_set_events(c, c->fd, cev, c, &c->in_loop) < 0) {
        proxy_abort(p);
        return;
    }

    uint32_t uev = 0;
    if (p->upstream_fd >= 0) {
        if (p->state == PROXY_CONNECTING) {
            uev = EV_WRITE;
        } else if (p->state == PROXY_SENDING) {
            if (p->req_out_sent < p->req_out_len ||
                p->c2u_len > p->c2u_off)
                uev = EV_WRITE;
        } else if (p->state == PROXY_READING_HEADERS) {
            uev = EV_READ;
        } else if (p->state == PROXY_RELAY_BODY) {
            if (!p->resp_body_done && p->u2c_len < sizeof p->u2c)
                uev |= EV_READ;
            if (p->c2u_len > p->c2u_off)
                uev |= EV_WRITE;
        }
        if (proxy_set_events(c, p->upstream_fd, uev, p,
                             &p->upstream_in_loop) < 0) {
            proxy_abort(p);
            return;
        }
    }
}

void proxy_kick(conn_t *c) {
    if (c == NULL || c->proxy == NULL)
        return;
    proxy_conn_t *p = c->proxy;
    if (p->state == PROXY_SENDING && !p->req_body_done &&
        c->in_len > 0) {
        proxy_read_client_body(p);
    } else {
        proxy_update_events(p);
    }
}

void proxy_mark_close(conn_t *c) {
    if (c != NULL && c->proxy != NULL)
        c->proxy->close_client = 1;
}

void proxy_sweep(void) {
    uint64_t now = now_ms();

    for (int i = 0; i < proxy_route_count(); i++) {
        for (int j = 0; j < proxy_route_at(i)->backend_count; j++) {
            proxy_pool_sweep(&proxy_route_at(i)->backends[j], now);
        }
    }

    proxy_conn_t *p = g_active;
    while (p != NULL) {
        proxy_conn_t *next = p->next;
        if (p->deadline_ms != 0 && now >= p->deadline_ms) {
            if (p->state == PROXY_CONNECTING && p->backend != NULL) {
                p->backend->down_until_ms =
                    now + PROXY_HEALTH_COOLDOWN_MS;
                proxy_pool_close(p->backend);
            }
            int status = (p->state == PROXY_SENDING && !p->req_body_done)
                             ? HTTP_REQUEST_TIMEOUT
                             : HTTP_GATEWAY_TIMEOUT;
            proxy_fail(p, status);
        }
        p = next;
    }
}

int proxy_next_timeout_ms(void) {
    uint64_t now = now_ms();
    uint64_t min = UINT64_MAX;

    for (proxy_conn_t *p = g_active; p != NULL; p = p->next) {
        if (p->deadline_ms != 0 && p->deadline_ms < min)
            min = p->deadline_ms;
    }

    for (int i = 0; i < proxy_route_count(); i++) {
        for (int j = 0; j < proxy_route_at(i)->backend_count; j++) {
            proxy_backend_t *b = &proxy_route_at(i)->backends[j];
            int pool_ms = proxy_pool_next_timeout_ms(b);
            if (pool_ms >= 0 && (uint64_t)pool_ms < min)
                min = (uint64_t)pool_ms;
        }
    }

    if (min == UINT64_MAX)
        return -1;
    if (min <= now)
        return 0;
    uint64_t d = min - now;
    return d > (uint64_t)0x7fffffff ? 0x7fffffff : (int)d;
}

static uint64_t proxy_request_deadline(const proxy_conn_t *p) {
    if (!p->req_body_done)
        return now_ms() + (uint64_t)proxy_config()->request_timeout_sec * 1000;
    return now_ms() + (uint64_t)(proxy_config()->timeout_response_sec * 1000);
}

static int proxy_connect_backend(proxy_conn_t *p) {
    proxy_route_t *route = p->route;

    for (;;) {
        int idx = -1;
        proxy_backend_t *backend =
            proxy_select_backend(route, p->tried_mask, &idx);
        if (backend == NULL)
            return -1;

        p->backend = backend;
        p->tried_mask |= 1u << idx;

        int fd = proxy_pool_acquire(backend);
        if (fd >= 0) {
            p->upstream_fd = fd;
            p->state = PROXY_SENDING;
            p->deadline_ms = proxy_request_deadline(p);
            if (ev_add(p->client->loop, fd, EV_WRITE, p) < 0) {
                close(fd);
                p->upstream_fd = -1;
                return -1;
            }
            p->upstream_in_loop = 1;
            return 0;
        }

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        if (set_nonblocking(fd) < 0 || set_cloexec(fd) < 0) {
            close(fd);
            return -1;
        }

        int rc = connect(fd, (struct sockaddr *)&backend->addr,
                         backend->addr_len);
        if (rc == 0) {
            p->upstream_fd = fd;
            p->state = PROXY_SENDING;
            p->deadline_ms = proxy_request_deadline(p);
            if (ev_add(p->client->loop, fd, EV_WRITE, p) < 0) {
                close(fd);
                p->upstream_fd = -1;
                return -1;
            }
            p->upstream_in_loop = 1;
            return 0;
        }
        if (errno == EINPROGRESS) {
            p->upstream_fd = fd;
            p->state = PROXY_CONNECTING;
            p->deadline_ms = now_ms() + (uint64_t)(proxy_config()->timeout_connect_sec * 1000);
            if (ev_add(p->client->loop, fd, EV_WRITE, p) < 0) {
                close(fd);
                p->upstream_fd = -1;
                return -1;
            }
            p->upstream_in_loop = 1;
            return 0;
        }

        close(fd);
        backend->down_until_ms = now_ms() + PROXY_HEALTH_COOLDOWN_MS;
        proxy_pool_close(backend);
    }
}

int proxy_start(conn_t *c, int route_index, const http_request_t *req) {
    if (proxy_config() == NULL || c == NULL || req == NULL ||
        route_index < 0 || route_index >= proxy_route_count())
        return -1;

    proxy_route_t *route = proxy_route_at(route_index);
    if (route->backend_count <= 0) {
        if (server_conn_build_error(c, HTTP_BAD_GATEWAY, 1,
                                    req->method == HTTP_HEAD, 0) < 0)
            return -1;
        if (ev_mod(c->loop, c->fd, EV_WRITE, c) < 0)
            return -1;
        return 0;
    }

    if (g_active_upstreams >= proxy_config()->max_upstreams) {
        if (server_conn_build_error(c, HTTP_SERVICE_UNAVAILABLE, 1,
                                    req->method == HTTP_HEAD, 1) < 0)
            return -1;
        if (ev_mod(c->loop, c->fd, EV_WRITE, c) < 0)
            return -1;
        return 0;
    }

    proxy_conn_t *p = calloc(1, sizeof *p);
    if (p == NULL)
        return -1;

    p->source.kind = EV_SRC_UPSTREAM;
    p->client = c;
    p->upstream_fd = -1;
    p->route = route;
    p->backend = &route->backends[0];
    p->head_only = req->method == HTTP_HEAD;
    p->client_keep_alive = req->keep_alive &&
        (c->requests + 1 < proxy_config()->keepalive_max_requests) &&
        !server_is_stopping();
    p->state = PROXY_CONNECTING;

    if (req->chunked) {
        p->req_body = BODY_CHUNKED;
        chunk_parser_init(&p->req_chunk);
    } else if (req->content_length > 0) {
        p->req_body = BODY_CONTENT_LENGTH;
        p->req_remaining = req->content_length;
    } else {
        p->req_body = BODY_NONE;
        p->req_body_done = 1;
    }

    proxy_build_request(c, req, p);
    if (p->req_out == NULL) {
        free(p);
        if (server_conn_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1,
                                    req->method == HTTP_HEAD, 0) < 0)
            return -1;
        if (ev_mod(c->loop, c->fd, EV_WRITE, c) < 0)
            return -1;
        return 0;
    }

    if (req->expect_continue) {
        p->continue_pending = 1;
        p->continue_sent = 0;
    }

    c->proxy = p;
    c->state = ST_PROXY;
    c->requests++;
    proxy_list_add(p);
    g_active_upstreams++;

    if (proxy_connect_backend(p) < 0) {
        proxy_fail(p, 502);
        return 0;
    }

    proxy_update_events(p);
    return 1;
}

void proxy_handle_upstream_event(proxy_conn_t *p, uint32_t events) {
    if (p == NULL || p->dead || p->upstream_fd < 0)
        return;

    if (p->state == PROXY_CONNECTING) {
        if (events & (EV_WRITE | EV_ERR)) {
            int err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(p->upstream_fd, SOL_SOCKET, SO_ERROR,
                           &err, &len) < 0 || err != 0) {
                p->backend->down_until_ms =
                    now_ms() + PROXY_HEALTH_COOLDOWN_MS;
                proxy_pool_close(p->backend);
                proxy_close_upstream(p);
                if (proxy_connect_backend(p) < 0) {
                    proxy_fail(p, 502);
                    return;
                }
                proxy_update_events(p);
                return;
            }
            p->state = PROXY_SENDING;
            p->deadline_ms = proxy_request_deadline(p);
            if (p->client->in_len > 0 && !p->req_body_done)
                proxy_read_client_body(p);
            else
                proxy_flush_upstream(p);
        }
        return;
    }

    if (events & EV_WRITE) {
        if (p->state == PROXY_SENDING || p->state == PROXY_RELAY_BODY)
            proxy_flush_upstream(p);
        if (p->dead)
            return;
    }

    int got_err = (events & EV_ERR) != 0;

    if ((events & EV_READ) || got_err) {
        if (p->state == PROXY_READING_HEADERS) {
            proxy_read_upstream_headers(p);
            if (p->dead)
                return;
        } else if (p->state == PROXY_RELAY_BODY) {
            if (got_err)
                proxy_drain_eof(p);
            else
                proxy_read_upstream_body(p);
            if (p->dead)
                return;
        } else if (p->state == PROXY_SENDING && got_err) {
            proxy_fail(p, 502);
            return;
        }
    }

    if (got_err && !p->dead) {
        p->upstream_eof = 1;
        if (p->state == PROXY_READING_HEADERS ||
            p->state == PROXY_SENDING) {
            proxy_fail(p, 502);
        } else if (p->state == PROXY_RELAY_BODY &&
                   !p->resp_body_done) {
            if (p->resp_body == BODY_CLOSE)
                proxy_drain_eof(p);
            else
                proxy_fail(p, 502);
        }
    }
}

void proxy_handle_client_event(conn_t *c, uint32_t events) {
    proxy_conn_t *p = c->proxy;
    if (p == NULL || p->dead)
        return;

    if (events & EV_ERR) {
        proxy_abort(p);
        return;
    }

    if (events & EV_READ) {
        if (p->state == PROXY_SENDING && !p->req_body_done) {
            proxy_read_client_body(p);
        } else {
            proxy_abort(p);
            return;
        }
    }

    if (c->proxy != NULL && (events & EV_WRITE)) {
        if (p->continue_pending)
            proxy_flush_continue(p);
        if (!p->dead && !p->continue_pending)
            proxy_flush_client(p);
    }
}

void proxy_detach(conn_t *c) {
    if (c == NULL || c->proxy == NULL)
        return;
    proxy_conn_t *p = c->proxy;
    proxy_close_upstream(p);
    proxy_free_struct(p);
}

void proxy_shutdown_all(void) {
    proxy_conn_t *p = g_active;
    while (p != NULL) {
        proxy_conn_t *next = p->next;
        conn_t *c = p->client;
        proxy_close_upstream(p);
        proxy_free_struct(p);
        if (c != NULL)
            server_conn_close(c);
        p = next;
    }

    for (int i = 0; i < proxy_route_count(); i++) {
        for (int j = 0; j < proxy_route_at(i)->backend_count; j++)
            proxy_pool_close(&proxy_route_at(i)->backends[j]);
    }
    proxy_reap();
}
