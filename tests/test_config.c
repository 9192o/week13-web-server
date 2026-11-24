#include "config.h"
#include "test.h"

#define ARGC(a) ((int)(sizeof(a) / sizeof((a)[0])) - 1)

static struct server_config cfg;
static char err[256];

static void test_defaults(void)
{
    char *argv[] = {"server", NULL};
    CHECK_EQ_INT(config_parse(ARGC(argv), argv, &cfg, err, sizeof err), 0);
    CHECK_EQ_INT(cfg.port, 8080);
    CHECK(cfg.root[0] == '/');                      /* 절대경로로 정규화 */
    CHECK(strstr(cfg.root, "/www") != NULL);
    CHECK_EQ_INT(cfg.workers, 8);
    CHECK_EQ_INT(cfg.queue_cap, 64);
}

static void test_workers_and_queue(void)
{
    char *ok[]    = {"server", "-w", "2", "-q", "2", NULL};
    char *maxv[]  = {"server", "-w", "256", "-q", "4096", NULL};
    char *w0[]    = {"server", "-w", "0", NULL};
    char *wbig[]  = {"server", "-w", "257", NULL};
    char *q0[]    = {"server", "-q", "0", NULL};
    char *qbig[]  = {"server", "-q", "4097", NULL};
    char *wtext[] = {"server", "-w", "4x", NULL};
    CHECK_EQ_INT(config_parse(ARGC(ok), ok, &cfg, err, sizeof err), 0);
    CHECK_EQ_INT(cfg.workers, 2);
    CHECK_EQ_INT(cfg.queue_cap, 2);
    CHECK_EQ_INT(config_parse(ARGC(maxv), maxv, &cfg, err, sizeof err), 0);
    CHECK_EQ_INT(cfg.workers, 256);
    CHECK_EQ_INT(cfg.queue_cap, 4096);
    CHECK_EQ_INT(config_parse(ARGC(w0), w0, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(wbig), wbig, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(q0), q0, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(qbig), qbig, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(wtext), wtext, &cfg, err, sizeof err), -1);
}

static void test_port_and_root(void)
{
    char *argv[] = {"server", "-p", "9090", "-r", "./www", NULL};
    CHECK_EQ_INT(config_parse(ARGC(argv), argv, &cfg, err, sizeof err), 0);
    CHECK_EQ_INT(cfg.port, 9090);
}

static void test_port_bounds(void)
{
    char *lo[]  = {"server", "-p", "1", NULL};
    char *hi[]  = {"server", "-p", "65535", NULL};
    char *zero[] = {"server", "-p", "0", NULL};
    char *over[] = {"server", "-p", "65536", NULL};
    CHECK_EQ_INT(config_parse(ARGC(lo), lo, &cfg, err, sizeof err), 0);
    CHECK_EQ_INT(config_parse(ARGC(hi), hi, &cfg, err, sizeof err), 0);
    CHECK_EQ_INT(config_parse(ARGC(zero), zero, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(over), over, &cfg, err, sizeof err), -1);
}

static void test_port_not_number(void)
{
    char *a[] = {"server", "-p", "abc", NULL};
    char *b[] = {"server", "-p", "80x", NULL};
    char *c[] = {"server", "-p", "", NULL};
    CHECK_EQ_INT(config_parse(ARGC(a), a, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(b), b, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(c), c, &cfg, err, sizeof err), -1);
}

static void test_missing_value_and_unknown(void)
{
    char *a[] = {"server", "-p", NULL};
    char *b[] = {"server", "-x", NULL};
    char *c[] = {"server", "extra", NULL};
    CHECK_EQ_INT(config_parse(ARGC(a), a, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(b), b, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(c), c, &cfg, err, sizeof err), -1);
}

static void test_bad_root(void)
{
    char *missing[] = {"server", "-r", "./no-such-dir", NULL};
    char *file[]    = {"server", "-r", "./Makefile", NULL};
    CHECK_EQ_INT(config_parse(ARGC(missing), missing, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(file), file, &cfg, err, sizeof err), -1);
}

static void test_root_slash_rejected(void)
{
    char *slash[]  = {"server", "-r", "/", NULL};
    char *dotdot[] = {"server", "-r", "/..", NULL};   /* realpath 결과가 "/" */
    CHECK_EQ_INT(config_parse(ARGC(slash), slash, &cfg, err, sizeof err), -1);
    CHECK_EQ_STR(err, "root must not be \"/\"");
    CHECK_EQ_INT(config_parse(ARGC(dotdot), dotdot, &cfg, err, sizeof err), -1);
    CHECK_EQ_STR(err, "root must not be \"/\"");
}

static void test_proxy_rules(void)
{
    char *none[]  = {"server", NULL};
    char *two[]   = {"server", "-R", "/api/=127.0.0.1:8001", "-R", "/s/=localhost:8002", NULL};
    char *bad[]   = {"server", "-R", "api=127.0.0.1:8001", NULL};
    char *dup[]   = {"server", "-R", "/a/=127.0.0.1:8001", "-R", "/a/=127.0.0.1:8002", NULL};
    char *self[]  = {"server", "-p", "9090", "-R", "/a/=127.0.0.1:9090", NULL};
    char *selfd[] = {"server", "-R", "/a/=localhost:8080", NULL};    /* 기본 포트 8080 */
    char *selfl[] = {"server", "-R", "/a/=127.0.0.1:9090", "-p", "9090", NULL}; /* -p 가 뒤 */
    char *other[] = {"server", "-p", "9090", "-R", "/a/=127.0.0.1:9091", NULL};

    CHECK_EQ_INT(config_parse(ARGC(none), none, &cfg, err, sizeof err), 0);
    CHECK_EQ_INT(cfg.routes.count, 0);
    CHECK_EQ_INT(config_parse(ARGC(two), two, &cfg, err, sizeof err), 0);
    CHECK_EQ_INT(cfg.routes.count, 2);
    CHECK_EQ_STR(cfg.routes.routes[0].prefix, "/api/");
    CHECK_EQ_STR(cfg.routes.routes[1].backends[0].name, "localhost:8002");
    CHECK_EQ_INT(config_parse(ARGC(bad), bad, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(dup), dup, &cfg, err, sizeof err), -1);
    CHECK_EQ_STR(err, "duplicate proxy prefix /a/");
    CHECK_EQ_INT(config_parse(ARGC(self), self, &cfg, err, sizeof err), -1);
    CHECK(strstr(err, "points to this server") != NULL);
    CHECK_EQ_INT(config_parse(ARGC(selfd), selfd, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(selfl), selfl, &cfg, err, sizeof err), -1);
    CHECK_EQ_INT(config_parse(ARGC(other), other, &cfg, err, sizeof err), 0);
}

int main(void)
{
    RUN_TEST(test_defaults);
    RUN_TEST(test_proxy_rules);
    RUN_TEST(test_port_and_root);
    RUN_TEST(test_workers_and_queue);
    RUN_TEST(test_port_bounds);
    RUN_TEST(test_port_not_number);
    RUN_TEST(test_missing_value_and_unknown);
    RUN_TEST(test_bad_root);
    RUN_TEST(test_root_slash_rejected);
    return TEST_SUMMARY();
}
