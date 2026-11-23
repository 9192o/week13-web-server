#include "cgi.h"
#include "server.h"
#include "test.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * 임시 디렉터리:
 *   base/outside.sh               root 밖 스크립트
 *   base/www/cgi-bin/<스크립트들>  #!/bin/sh 테스트 CGI
 *   base/www/cgi-bin/sub/         디렉터리
 *   base/www/cgi-bin/out -> ../../outside.sh
 */
static char base[64];
static char root[PATH_MAX];
static char made[32][PATH_MAX];
static int made_n;

#define HDR "printf 'Content-Type: text/plain\\n\\n'\n"

static void path_of(char *out, const char *rel)
{
    snprintf(out, PATH_MAX, "%s/%s", base, rel);
}

static void make_file(const char *rel, const char *content, mode_t mode)
{
    char p[PATH_MAX];
    FILE *f;

    path_of(p, rel);
    f = fopen(p, "w");
    if (f == NULL)
        abort();
    fputs(content, f);
    fclose(f);
    if (chmod(p, mode) < 0)
        abort();
    snprintf(made[made_n++], PATH_MAX, "%s", p);
}

static void make_script(const char *name, const char *body)
{
    char rel[128], content[1024];
    snprintf(rel, sizeof rel, "www/cgi-bin/%s", name);
    snprintf(content, sizeof content, "#!/bin/sh\n%s", body);
    make_file(rel, content, 0755);
}

static void setup(void)
{
    char p[PATH_MAX];

    snprintf(base, sizeof base, "/tmp/ws-cgi-XXXXXX");
    if (mkdtemp(base) == NULL)
        abort();
    path_of(p, "www");
    mkdir(p, 0755);
    path_of(p, "www/cgi-bin");
    mkdir(p, 0755);
    path_of(p, "www/cgi-bin/sub");
    mkdir(p, 0755);

    make_file("outside.sh", "#!/bin/sh\n" HDR, 0755);
    path_of(p, "www/cgi-bin/out");
    if (symlink("../../outside.sh", p) < 0)
        abort();
    snprintf(made[made_n++], PATH_MAX, "%s", p);

    make_script("ok", "printf 'Content-Type: text/plain\\n\\nhello'\n");
    make_script("env", HDR "printf '%s|%s|%s|%s|%s|%s' \"$REQUEST_METHOD\" "
                "\"$QUERY_STRING\" \"${CONTENT_LENGTH-none}\" \"$SCRIPT_NAME\" "
                "\"${HOME-unset}\" \"$GATEWAY_INTERFACE\"\n");
    make_script("echo", HDR "cat\n");
    make_script("bigfirst", HDR "head -c 200000 /dev/zero\nwc -c\n");
    make_script("crash", HDR "kill -SEGV $$\n");
    make_script("exit1", HDR "printf partial\nexit 1\n");
    make_script("slow", "sleep 10\n");
    make_script("lingers", HDR "exec >&-\nsleep 10\n");
    make_script("huge", HDR "head -c 2000000 /dev/zero\n");
    make_script("noct", "printf 'X-A: b\\n\\nbody'\n");
    make_script("status", "printf 'Status: 404 Whatever\\r\\n"
                "Content-Type: text/plain\\r\\n\\r\\nnf'\n");
    make_file("www/cgi-bin/badshebang", "#!/nonexistent/interp\n", 0755);
    make_file("www/cgi-bin/noexec", "#!/bin/sh\n" HDR, 0644);

    path_of(p, "www");
    if (realpath(p, root) == NULL)
        abort();
}

static void teardown(void)
{
    char p[PATH_MAX];
    int i;

    for (i = 0; i < made_n; i++)
        unlink(made[i]);
    path_of(p, "www/cgi-bin/sub");
    rmdir(p);
    path_of(p, "www/cgi-bin");
    rmdir(p);
    path_of(p, "www");
    rmdir(p);
    rmdir(base);
}

/* ---- cgi_resolve ---- */

static void test_resolve(void)
{
    char out[PATH_MAX], want[PATH_MAX + 32];

    CHECK_EQ_INT(cgi_resolve(root, "/cgi-bin/ok", out), 0);
    snprintf(want, sizeof want, "%s/cgi-bin/ok", root);
    CHECK_EQ_STR(out, want);

    CHECK_EQ_INT(cgi_resolve(root, "/cgi-bin", out), 404);
    CHECK_EQ_INT(cgi_resolve(root, "/cgi-bin/", out), 404);
    CHECK_EQ_INT(cgi_resolve(root, "/cgi-bin/ok/extra", out), 404);   /* PATH_INFO 없음 */
    CHECK_EQ_INT(cgi_resolve(root, "/cgi-bin/ok/", out), 404);
    CHECK_EQ_INT(cgi_resolve(root, "/cgi-bin/missing", out), 404);
    CHECK_EQ_INT(cgi_resolve(root, "/cgi-bin/sub", out), 403);        /* 디렉터리 */
    CHECK_EQ_INT(cgi_resolve(root, "/cgi-bin/noexec", out), 403);     /* 실행 권한 없음 */
    CHECK_EQ_INT(cgi_resolve(root, "/cgi-bin/out", out), 403);        /* cgi-bin 밖 링크 */
}

/* ---- cgi_parse_output ---- */

static int parse(const char *s, int *status, char *ct, size_t *off)
{
    return cgi_parse_output(s, strlen(s), status, ct, CGI_CONTENT_TYPE_MAX + 1, off);
}

static void test_parse_ok(void)
{
    char ct[CGI_CONTENT_TYPE_MAX + 1];
    int status;
    size_t off;
    const char *s = "Content-Type: text/plain\n\nhello";

    CHECK_EQ_INT(parse(s, &status, ct, &off), 0);
    CHECK_EQ_INT(status, 200);
    CHECK_EQ_STR(ct, "text/plain");
    CHECK_EQ_STR(s + off, "hello");

    s = "content-type:  text/html ; x=1 \r\nX-Other: dropped\r\n\r\n";
    CHECK_EQ_INT(parse(s, &status, ct, &off), 0);
    CHECK_EQ_STR(ct, "text/html ; x=1");                  /* 앞뒤 공백 제거 */
    CHECK_EQ_INT(off, strlen(s));                         /* 빈 본문 */
}

static void test_parse_status(void)
{
    char ct[CGI_CONTENT_TYPE_MAX + 1];
    int status;
    size_t off;

    CHECK_EQ_INT(parse("Status: 404 Whatever\nContent-Type: a\n\n", &status, ct, &off), 0);
    CHECK_EQ_INT(status, 404);                            /* CGI 문구는 무시 */
    CHECK_EQ_INT(parse("Status: 201\nContent-Type: a\n\n", &status, ct, &off), 0);
    CHECK_EQ_INT(status, 201);
    CHECK_EQ_INT(parse("Status: 100\nContent-Type: a\n\n", &status, ct, &off), 0);
    CHECK_EQ_INT(parse("Status: 599\nContent-Type: a\n\n", &status, ct, &off), 0);

    CHECK_EQ_INT(parse("Status: 099\nContent-Type: a\n\n", &status, ct, &off), -1);
    CHECK_EQ_INT(parse("Status: 600\nContent-Type: a\n\n", &status, ct, &off), -1);
    CHECK_EQ_INT(parse("Status: 99\nContent-Type: a\n\n", &status, ct, &off), -1);
    CHECK_EQ_INT(parse("Status: 2000\nContent-Type: a\n\n", &status, ct, &off), -1);
    CHECK_EQ_INT(parse("Status: abc\nContent-Type: a\n\n", &status, ct, &off), -1);
    CHECK_EQ_INT(parse("Status: 200\nStatus: 200\nContent-Type: a\n\n",
                       &status, ct, &off), -1);
}

static void test_parse_bad(void)
{
    char ct[CGI_CONTENT_TYPE_MAX + 1], longct[400];
    int status;
    size_t off;

    CHECK_EQ_INT(parse("X-A: b\n\nbody", &status, ct, &off), -1);          /* CT 없음 */
    CHECK_EQ_INT(parse("Content-Type: a\nContent-Type: b\n\n", &status, ct, &off), -1);
    CHECK_EQ_INT(parse("Content-Type: a\n", &status, ct, &off), -1);       /* 빈 줄 없음 */
    CHECK_EQ_INT(parse("", &status, ct, &off), -1);
    CHECK_EQ_INT(parse("no colon\nContent-Type: a\n\n", &status, ct, &off), -1);
    CHECK_EQ_INT(parse("Content-Type:\n\n", &status, ct, &off), -1);       /* 빈 값 */
    CHECK_EQ_INT(parse("Content-Type: a\rX-Injected: y\n\n", &status, ct, &off), -1);
    CHECK_EQ_INT(parse("Bad Name: a\nContent-Type: a\n\n", &status, ct, &off), -1);

    snprintf(longct, sizeof longct, "Content-Type: %0257d\n\n", 0);         /* 257 바이트 */
    CHECK_EQ_INT(parse(longct, &status, ct, &off), -1);
    snprintf(longct, sizeof longct, "Content-Type: %0256d\n\n", 0);         /* 256 바이트 */
    CHECK_EQ_INT(parse(longct, &status, ct, &off), 0);
}

/* ---- cgi_run ---- */

static void run(const char *name, const char *method, const char *query,
                const char *body, size_t body_len, int timeout_ms,
                struct cgi_result *res)
{
    char script[PATH_MAX], sname[64];
    struct cgi_request req;

    snprintf(sname, sizeof sname, "/cgi-bin/%s", name);
    if (cgi_resolve(root, sname, script) != 0)
        abort();
    memset(&req, 0, sizeof req);
    req.script_path = script;
    req.script_name = sname;
    req.method = method;
    req.query = query;
    req.protocol = "HTTP/1.1";
    req.content_length = body ? (long long)body_len : -1;
    req.body = body;
    req.body_len = body_len;
    req.remote_addr = "127.0.0.1";
    req.remote_port = 5555;
    req.server_name = "localhost";
    req.server_port = 8080;
    cgi_run(&req, timeout_ms, res);
}

static const char *body_of(const struct cgi_result *res)
{
    return res->output + res->body_off;
}

static void test_run_ok(void)
{
    struct cgi_result res;

    run("ok", "GET", NULL, NULL, 0, 2000, &res);
    CHECK_EQ_INT(res.fail, CGI_OK);
    CHECK_EQ_INT(res.status, 200);
    CHECK_EQ_STR(res.content_type, "text/plain");
    CHECK_EQ_INT(res.output_len - res.body_off, 5);
    CHECK(memcmp(body_of(&res), "hello", 5) == 0);
    cgi_result_free(&res);
}

static void test_run_env(void)
{
    struct cgi_result res;

    run("env", "GET", "a=1&b=2", NULL, 0, 2000, &res);
    CHECK_EQ_INT(res.fail, CGI_OK);
    res.output[res.output_len] = '\0';
    CHECK_EQ_STR(body_of(&res), "GET|a=1&b=2|none|/cgi-bin/env|unset|CGI/1.1");
    cgi_result_free(&res);

    run("env", "POST", NULL, "x", 1, 2000, &res);
    CHECK_EQ_INT(res.fail, CGI_OK);
    res.output[res.output_len] = '\0';
    CHECK_EQ_STR(body_of(&res), "POST||1|/cgi-bin/env|unset|CGI/1.1");
    cgi_result_free(&res);
}

static void test_run_post_stdin(void)
{
    struct cgi_result res;

    run("echo", "POST", NULL, "a=1&b=2", 7, 2000, &res);
    CHECK_EQ_INT(res.fail, CGI_OK);
    CHECK_EQ_INT(res.output_len - res.body_off, 7);
    CHECK(memcmp(body_of(&res), "a=1&b=2", 7) == 0);
    cgi_result_free(&res);
}

static void test_run_no_deadlock(void)
{
    /* CGI 가 stdin 을 읽기 전에 200KB 를 먼저 쓰고, 서버는 1MB 를 써야 하는 상황 */
    size_t n = 1024 * 1024;
    char *big = malloc(n);
    struct cgi_result res;
    const char *tail;

    memset(big, 'q', n);
    run("bigfirst", "POST", NULL, big, n, 3000, &res);
    CHECK_EQ_INT(res.fail, CGI_OK);
    res.output[res.output_len] = '\0';
    tail = res.output + res.output_len - 8;
    CHECK_EQ_STR(tail, "1048576\n");
    cgi_result_free(&res);

    /* stdin 을 안 읽는 CGI 에 큰 본문: EPIPE 는 오류가 아님 */
    run("ok", "POST", NULL, big, n, 2000, &res);
    CHECK_EQ_INT(res.fail, CGI_OK);
    cgi_result_free(&res);
    free(big);
}

static void test_run_failures(void)
{
    struct cgi_result res;
    long long t0;

    run("crash", "GET", NULL, NULL, 0, 2000, &res);           /* S11 */
    CHECK_EQ_INT(res.fail, CGI_FAIL_SIGNAL);
    CHECK_EQ_INT(res.detail, 11);
    CHECK(res.output == NULL);                                /* 출력은 버림 */

    run("exit1", "GET", NULL, NULL, 0, 2000, &res);
    CHECK_EQ_INT(res.fail, CGI_FAIL_EXIT);
    CHECK_EQ_INT(res.detail, 1);
    CHECK(res.output == NULL);

    run("badshebang", "GET", NULL, NULL, 0, 2000, &res);      /* exec 실패 */
    CHECK_EQ_INT(res.fail, CGI_FAIL_EXIT);
    CHECK_EQ_INT(res.detail, 127);

    run("huge", "GET", NULL, NULL, 0, 2000, &res);
    CHECK_EQ_INT(res.fail, CGI_FAIL_TOO_LARGE);

    run("noct", "GET", NULL, NULL, 0, 2000, &res);
    CHECK_EQ_INT(res.fail, CGI_FAIL_BAD_HEADER);

    t0 = now_ms();
    run("slow", "GET", NULL, NULL, 0, 300, &res);
    CHECK_EQ_INT(res.fail, CGI_FAIL_TIMEOUT);
    CHECK(now_ms() - t0 < 1500);                              /* kill 후 바로 돌아옴 */

    t0 = now_ms();
    run("lingers", "GET", NULL, NULL, 0, 300, &res);          /* stdout 닫고 계속 실행 */
    CHECK_EQ_INT(res.fail, CGI_FAIL_TIMEOUT);
    CHECK(now_ms() - t0 < 1500);
}

static void test_run_status(void)
{
    struct cgi_result res;

    run("status", "GET", NULL, NULL, 0, 2000, &res);
    CHECK_EQ_INT(res.fail, CGI_OK);
    CHECK_EQ_INT(res.status, 404);
    CHECK_EQ_INT(res.output_len - res.body_off, 2);
    cgi_result_free(&res);
}

int main(void)
{
    struct sigaction sa;

    /* 서버(main.c)와 같은 조건: stdin 을 안 읽는 CGI 에 쓰면 SIGPIPE 대신 EPIPE */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);

    setup();
    RUN_TEST(test_resolve);
    RUN_TEST(test_parse_ok);
    RUN_TEST(test_parse_status);
    RUN_TEST(test_parse_bad);
    RUN_TEST(test_run_ok);
    RUN_TEST(test_run_env);
    RUN_TEST(test_run_post_stdin);
    RUN_TEST(test_run_no_deadlock);
    RUN_TEST(test_run_failures);
    RUN_TEST(test_run_status);
    teardown();
    return TEST_SUMMARY();
}
