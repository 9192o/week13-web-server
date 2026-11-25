#include "proxy_core.h"

#include "proxy.h"
#include "util.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static proxy_route_t g_routes[MAX_ROUTES];
static int g_route_count;
static const config_t *g_cfg;

int proxy_route_init(const config_t *cfg) {
    if (cfg == NULL)
        return -1;

    memset(g_routes, 0, sizeof g_routes);
    g_route_count = 0;
    g_cfg = cfg;
    proxy_pool_init(cfg->pool_max_idle, cfg->pool_idle_timeout_sec * 1000);

    for (int i = 0; i < cfg->route_count; i++) {
        const route_spec_t *spec = &cfg->routes[i];
        proxy_route_t *r = &g_routes[i];
        snprintf(r->prefix, sizeof r->prefix, "%s", spec->prefix);
        r->type = spec->type;
        r->strip_prefix = spec->strip_prefix;
        r->backend_count = spec->backend_count;

        for (int j = 0; j < spec->backend_count; j++) {
            proxy_backend_t *b = &r->backends[j];
            snprintf(b->host, sizeof b->host, "%s", spec->backends[j].host);
            b->port = spec->backends[j].port;

            char portbuf[16];
            snprintf(portbuf, sizeof portbuf, "%d", b->port);
            struct addrinfo hints;
            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_NUMERICSERV;
            struct addrinfo *res = NULL;
            int rc = getaddrinfo(b->host, portbuf, &hints, &res);
            if (rc != 0 || res == NULL) {
                fprintf(stderr, "proxy: resolve %s:%d: %s\n",
                        b->host, b->port, gai_strerror(rc));
                proxy_route_cleanup();
                return -1;
            }
            memcpy(&b->addr, res->ai_addr, res->ai_addrlen);
            b->addr_len = res->ai_addrlen;
            freeaddrinfo(res);
        }
        g_route_count++;
    }

    return 0;
}

void proxy_route_cleanup(void) {
    for (int i = 0; i < g_route_count; i++) {
        for (int j = 0; j < g_routes[i].backend_count; j++)
            proxy_pool_close(&g_routes[i].backends[j]);
    }
    g_route_count = 0;
    g_cfg = NULL;
}

const config_t *proxy_config(void) {
    return g_cfg;
}

int proxy_route_count(void) {
    return g_route_count;
}

proxy_route_t *proxy_route_at(int index) {
    if (index < 0 || index >= g_route_count)
        return NULL;
    return &g_routes[index];
}

proxy_backend_t *proxy_select_backend(proxy_route_t *r, uint32_t skip_mask,
                                       int *out_index) {
    uint64_t now = now_ms();
    for (int i = 0; i < r->backend_count; i++) {
        int idx = (r->rr_index + i) % r->backend_count;
        if (skip_mask & (1u << idx))
            continue;
        proxy_backend_t *b = &r->backends[idx];
        if (b->down_until_ms <= now) {
            r->rr_index = (idx + 1) % r->backend_count;
            if (out_index != NULL)
                *out_index = idx;
            return b;
        }
    }
    return NULL;
}
