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

int main(void)
{
    RUN_TEST(test_defaults);
    RUN_TEST(test_port_and_root);
    RUN_TEST(test_port_bounds);
    RUN_TEST(test_port_not_number);
    RUN_TEST(test_missing_value_and_unknown);
    RUN_TEST(test_bad_root);
    return TEST_SUMMARY();
}
