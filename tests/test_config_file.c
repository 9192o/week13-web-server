#include "config.h"
#include "test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_temp_config(const char *text, char *path, size_t cap) {
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || *tmp == '\0')
        tmp = "/tmp";
    int n = snprintf(path, cap, "%s/web_server_cfg_XXXXXX", tmp);
    if (n < 0 || (size_t)n >= cap)
        return -1;

    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    FILE *f = fdopen(fd, "w");
    if (f == NULL) {
        close(fd);
        unlink(path);
        return -1;
    }
    fputs(text, f);
    fclose(f);
    return 0;
}

void test_config_file_valid(void) {
    const char *text =
        "listen 18099\n"
        "docroot ./www\n"
        "max_conns 7\n"
        "max_upstreams 5\n"
        "timeout request 3\n"
        "[route /api/]\n"
        "type proxy\n"
        "strip_prefix 1\n"
        "backend 127.0.0.1:8001\n"
        "backend 127.0.0.1:8002\n"
        "[route /static/]\n"
        "type static\n"
        "[global]\n"
        "timeout connect 2\n";

    char path[512];
    TEST_ASSERT_EQ_INT(write_temp_config(text, path, sizeof path), 0);

    config_t cfg;
    config_init(&cfg);
    TEST_ASSERT_EQ_INT(config_parse_file(&cfg, path), 0);
    TEST_ASSERT_EQ_INT(cfg.port, 18099);
    TEST_ASSERT_EQ_INT(cfg.max_conns, 7);
    TEST_ASSERT_EQ_INT(cfg.max_upstreams, 5);
    TEST_ASSERT_EQ_INT(cfg.route_count, 2);
    TEST_ASSERT_EQ_INT(cfg.routes[0].type, ROUTE_PROXY);
    TEST_ASSERT_EQ_INT(cfg.routes[0].strip_prefix, 1);
    TEST_ASSERT_EQ_INT(cfg.routes[0].backend_count, 2);
    TEST_ASSERT_EQ_STR(cfg.routes[0].prefix, "/api/");
    TEST_ASSERT_EQ_STR(cfg.routes[0].backends[0].host, "127.0.0.1");
    TEST_ASSERT_EQ_INT(cfg.routes[0].backends[0].port, 8001);
    TEST_ASSERT_EQ_INT(cfg.routes[1].type, ROUTE_STATIC);
    TEST_ASSERT_EQ_INT(cfg.timeout_connect_sec, 2);
    TEST_ASSERT_EQ_INT(cfg.request_timeout_sec, 3);
    TEST_ASSERT_EQ_INT(config_validate(&cfg), 0);

    route_spec_t *r = config_find_route(&cfg, "/api/users");
    TEST_ASSERT(r == &cfg.routes[0]);
    TEST_ASSERT(config_find_route(&cfg, "/apix") == NULL);
    TEST_ASSERT(config_find_route(&cfg, "/static/app.js") == &cfg.routes[1]);

    unlink(path);
}

void test_config_file_duplicate_route(void) {
    const char *text =
        "[route /api/]\n"
        "type static\n"
        "[route /api/]\n"
        "type static\n";
    char path[512];
    TEST_ASSERT_EQ_INT(write_temp_config(text, path, sizeof path), 0);
    config_t cfg;
    config_init(&cfg);
    TEST_ASSERT_EQ_INT(config_parse_file(&cfg, path), -1);
    unlink(path);
}

void test_config_file_bad_number(void) {
    const char *text =
        "listen 8080\n"
        "timeout connect nope\n";
    char path[512];
    TEST_ASSERT_EQ_INT(write_temp_config(text, path, sizeof path), 0);
    config_t cfg;
    config_init(&cfg);
    TEST_ASSERT_EQ_INT(config_parse_file(&cfg, path), -1);
    unlink(path);
}

void test_config_file_proxy_without_backend(void) {
    const char *text =
        "[route /api/]\n"
        "type proxy\n";
    char path[512];
    TEST_ASSERT_EQ_INT(write_temp_config(text, path, sizeof path), 0);
    config_t cfg;
    config_init(&cfg);
    TEST_ASSERT_EQ_INT(config_parse_file(&cfg, path), 0);
    TEST_ASSERT_EQ_INT(config_validate(&cfg), -1);
    unlink(path);
}

void test_config_file_ipv6_backend(void) {
    const char *text =
        "[route /api/]\n"
        "type proxy\n"
        "backend [::1]:8001\n";
    char path[512];
    TEST_ASSERT_EQ_INT(write_temp_config(text, path, sizeof path), 0);
    config_t cfg;
    config_init(&cfg);
    TEST_ASSERT_EQ_INT(config_parse_file(&cfg, path), -1);
    unlink(path);
}
