#include "proxy_core.h"
#include "test_harness.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static void set_backend(route_spec_t *r, int idx, const char *host, int port) {
    snprintf(r->backends[idx].host, sizeof r->backends[idx].host, "%s", host);
    r->backends[idx].port = port;
}

void test_proxy_route_table(void) {
    config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.pool_max_idle = 1;
    cfg.pool_idle_timeout_sec = 1;
    cfg.route_count = 2;

    route_spec_t *a = &cfg.routes[0];
    snprintf(a->prefix, sizeof a->prefix, "%s", "/api/");
    a->type = ROUTE_PROXY;
    a->strip_prefix = 1;
    a->backend_count = 2;
    set_backend(a, 0, "127.0.0.1", 18001);
    set_backend(a, 1, "127.0.0.1", 18002);

    route_spec_t *b = &cfg.routes[1];
    snprintf(b->prefix, sizeof b->prefix, "%s", "/static/");
    b->type = ROUTE_STATIC;
    b->backend_count = 0;

    TEST_ASSERT_EQ_INT(proxy_route_init(&cfg), 0);
    TEST_ASSERT(proxy_config() == &cfg);
    TEST_ASSERT_EQ_INT(proxy_route_count(), 2);
    TEST_ASSERT_EQ_STR(proxy_route_at(0)->prefix, "/api/");
    TEST_ASSERT_EQ_INT(proxy_route_at(0)->strip_prefix, 1);
    TEST_ASSERT_EQ_INT(proxy_route_at(0)->backend_count, 2);
    TEST_ASSERT_EQ_INT(proxy_route_at(1)->type, ROUTE_STATIC);
    TEST_ASSERT(proxy_route_at(-1) == NULL);
    TEST_ASSERT(proxy_route_at(2) == NULL);

    TEST_ASSERT(config_find_route(&cfg, "/api/users") == a);
    TEST_ASSERT(config_find_route(&cfg, "/apix") == NULL);
    TEST_ASSERT(config_find_route(&cfg, "/static/app.js") == b);

    proxy_route_cleanup();
    TEST_ASSERT_EQ_INT(proxy_route_count(), 0);
    TEST_ASSERT(proxy_config() == NULL);
}

void test_proxy_route_round_robin(void) {
    config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.pool_max_idle = 1;
    cfg.pool_idle_timeout_sec = 1;
    cfg.route_count = 1;

    route_spec_t *r = &cfg.routes[0];
    snprintf(r->prefix, sizeof r->prefix, "%s", "/api/");
    r->type = ROUTE_PROXY;
    r->backend_count = 2;
    set_backend(r, 0, "127.0.0.1", 18001);
    set_backend(r, 1, "127.0.0.1", 18002);

    TEST_ASSERT_EQ_INT(proxy_route_init(&cfg), 0);
    proxy_route_t *route = proxy_route_at(0);
    int idx = -1;

    proxy_backend_t *first = proxy_select_backend(route, 0, &idx);
    TEST_ASSERT(first != NULL);
    TEST_ASSERT_EQ_INT(idx, 0);
    TEST_ASSERT_EQ_INT(first->port, 18001);

    proxy_backend_t *second = proxy_select_backend(route, 0, &idx);
    TEST_ASSERT(second != NULL);
    TEST_ASSERT_EQ_INT(idx, 1);
    TEST_ASSERT_EQ_INT(second->port, 18002);

    proxy_backend_t *third = proxy_select_backend(route, 0, &idx);
    TEST_ASSERT(third == first);
    TEST_ASSERT_EQ_INT(idx, 0);

    /* Skip the first backend, and make the second unavailable. */
    TEST_ASSERT(proxy_select_backend(route, 1u, &idx) == second);
    second->down_until_ms = now_ms() + 10000;
    TEST_ASSERT(proxy_select_backend(route, 0, &idx) == first);

    proxy_route_cleanup();
}

void test_proxy_route_resolve_failure(void) {
    config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.pool_max_idle = 1;
    cfg.pool_idle_timeout_sec = 1;
    cfg.route_count = 1;

    route_spec_t *r = &cfg.routes[0];
    snprintf(r->prefix, sizeof r->prefix, "%s", "/api/");
    r->type = ROUTE_PROXY;
    r->backend_count = 1;
    set_backend(r, 0, "bad.invalid", 18001);

    /* Fail fast; route state must be reset after the failure. */
    TEST_ASSERT_EQ_INT(proxy_route_init(&cfg), -1);
    TEST_ASSERT_EQ_INT(proxy_route_count(), 0);
    TEST_ASSERT(proxy_config() == NULL);
}
