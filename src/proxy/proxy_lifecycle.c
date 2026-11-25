#include "proxy_core.h"

#include "event_backend.h"
#include "http.h"
#include "server_conn.h"
#include "util.h"

#include <stdlib.h>
#include <unistd.h>

static proxy_conn_t *g_active;
static proxy_conn_t *g_dead;
static int g_active_upstreams;

int proxy_upstream_connections(void) {
    int total = g_active_upstreams;
    for (int i = 0; i < proxy_route_count(); i++) {
        proxy_route_t *r = proxy_route_at(i);
        for (int j = 0; j < r->backend_count; j++)
            total += r->backends[j].idle_count;
    }
    return total;
}

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

void proxy_list_add(proxy_conn_t *p) {
    p->next = g_active;
    g_active = p;
    g_active_upstreams++;
}

proxy_conn_t *proxy_active_head(void) {
    return g_active;
}

int proxy_active_count(void) {
    return g_active_upstreams;
}

static void proxy_list_remove(proxy_conn_t *p) {
    proxy_conn_t **pp = &g_active;
    while (*pp != NULL) {
        if (*pp == p) {
            *pp = p->next;
            if (g_active_upstreams > 0)
                g_active_upstreams--;
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
    if (p->client != NULL && p->client->app_state == p)
        p->client->app_state = NULL;
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

void proxy_detach(conn_t *c) {
    if (c == NULL || c->app_state == NULL)
        return;
    proxy_conn_t *p = c->app_state;
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
