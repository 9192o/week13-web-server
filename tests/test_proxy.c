#include "proxy.h"
#include "http_parser.h"
#include "test.h"

#include <arpa/inet.h>

static char err[256];

static void test_parse_rule_ok(void)
{
    struct proxy_route r;

    CHECK_EQ_INT(proxy_parse_rule("/api/=127.0.0.1:8001", &r, err, sizeof err), 0);
    CHECK_EQ_STR(r.prefix, "/api/");
    CHECK_EQ_STR(r.name, "127.0.0.1:8001");
    CHECK_EQ_INT(ntohs(r.addr.sin_port), 8001);
    CHECK_EQ_INT(ntohl(r.addr.sin_addr.s_addr), 0x7f000001);

    /* 호스트명은 시작할 때 한 번 해석 */
    CHECK_EQ_INT(proxy_parse_rule("/=localhost:65535", &r, err, sizeof err), 0);
    CHECK_EQ_STR(r.prefix, "/");
    CHECK_EQ_INT(ntohs(r.addr.sin_port), 65535);
    CHECK_EQ_INT(ntohl(r.addr.sin_addr.s_addr) >> 24, 127);
}

static void test_parse_rule_bad(void)
{
    struct proxy_route r;
    static const char *const bad[] = {
        "/api/",                    /* '=' 없음 */
        "=127.0.0.1:8001",          /* prefix 비어 있음 */
        "api/=127.0.0.1:8001",      /* '/' 로 시작 안 함 */
        "/api/=127.0.0.1",          /* 포트 없음 */
        "/api/=:8001",              /* 호스트 없음 */
        "/api/=127.0.0.1:0",
        "/api/=127.0.0.1:65536",
        "/api/=127.0.0.1:80x",
        "/api/=127.0.0.1:",
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = proxy_parse_rule(bad[i], &r, err, sizeof err);
        CHECK_EQ_INT(rc, -1);
        if (rc != -1)
            fprintf(stderr, "    accepted: %s\n", bad[i]);
    }
}

static void test_match(void)
{
    struct proxy_table t;

    t.count = 0;
    CHECK(proxy_match(&t, "/api/x") == NULL);

    CHECK_EQ_INT(proxy_parse_rule("/api/=127.0.0.1:8001", &t.routes[0], err, sizeof err), 0);
    CHECK_EQ_INT(proxy_parse_rule("/api/v2/=127.0.0.1:8002", &t.routes[1], err, sizeof err), 0);
    t.count = 2;

    CHECK(proxy_match(&t, "/api/hello") == &t.routes[0]);
    CHECK(proxy_match(&t, "/api/") == &t.routes[0]);
    CHECK(proxy_match(&t, "/api/v2/users") == &t.routes[1]);   /* 가장 긴 prefix */
    CHECK(proxy_match(&t, "/api") == NULL);                    /* 문자열 그대로 매칭 */
    CHECK(proxy_match(&t, "/apix") == NULL);
    CHECK(proxy_match(&t, "/index.html") == NULL);
}

static void test_is_self(void)
{
    struct proxy_route r;

    proxy_parse_rule("/a/=127.0.0.1:8080", &r, err, sizeof err);
    CHECK_EQ_INT(proxy_is_self(&r, 8080), 1);
    CHECK_EQ_INT(proxy_is_self(&r, 8081), 0);
    proxy_parse_rule("/a/=localhost:8080", &r, err, sizeof err);
    CHECK_EQ_INT(proxy_is_self(&r, 8080), 1);
    proxy_parse_rule("/a/=0.0.0.0:8080", &r, err, sizeof err);
    CHECK_EQ_INT(proxy_is_self(&r, 8080), 1);
    proxy_parse_rule("/a/=10.0.0.1:8080", &r, err, sizeof err);
    CHECK_EQ_INT(proxy_is_self(&r, 8080), 0);
}

/* raw 요청을 파싱해서 재작성한 결과를 out 에 */
static int build(const char *raw, char *out, size_t cap)
{
    static char buf[4096];
    struct http_request req;

    snprintf(buf, sizeof buf, "%s", raw);
    if (http_parse_request(buf, strlen(buf), &req) != 0)
        return -2;
    return proxy_build_request(&req, out, cap);
}

static void test_build_request(void)
{
    char out[1024];
    int n;

    n = build("GET /api/hello?x=1&y=%20 HTTP/1.1\r\n"
              "Host: localhost:8080\r\n"
              "User-Agent: curl\r\n"
              "Connection: keep-alive, X-Secret\r\n"
              "Keep-Alive: timeout=5\r\n"
              "X-Secret: s\r\n"
              "Proxy-Connection: keep-alive\r\n"
              "Upgrade: h2c\r\n"
              "Accept: */*\r\n"
              "\r\n", out, sizeof out);
    CHECK(n > 0);
    CHECK_EQ_STR(out,
                 "GET /api/hello?x=1&y=%20 HTTP/1.0\r\n"
                 "Host: localhost:8080\r\n"
                 "User-Agent: curl\r\n"
                 "Accept: */*\r\n"
                 "Connection: close\r\n"
                 "\r\n");
    CHECK_EQ_INT(n, (long long)strlen(out));

    /* HTTP/1.0, Host 없음, 쿼리 없음, 본문 길이는 전달하지 않음 (P1) */
    n = build("GET /api/ HTTP/1.0\r\nContent-Length: 3\r\n\r\n", out, sizeof out);
    CHECK_EQ_STR(out, "GET /api/ HTTP/1.0\r\nConnection: close\r\n\r\n");

    /* 빈 쿼리("?" 만)도 그대로 */
    n = build("GET /api/x? HTTP/1.1\r\nHost: h\r\n\r\n", out, sizeof out);
    CHECK_EQ_STR(out, "GET /api/x? HTTP/1.0\r\nHost: h\r\nConnection: close\r\n\r\n");

    /* 버퍼 부족 */
    n = build("GET /api/x HTTP/1.1\r\nHost: h\r\n\r\n", out, 20);
    CHECK_EQ_INT(n, -1);
}

/* raw 응답 헤더를 파싱. 실패면 이유 문자열 */
static const char *parse(const char *raw, struct proxy_response *r)
{
    static char buf[4096];

    snprintf(buf, sizeof buf, "%s", raw);
    return proxy_parse_response(buf, strlen(buf), r);
}

static void test_parse_response_ok(void)
{
    struct proxy_response r;

    CHECK(parse("HTTP/1.0 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length:  5 \r\n"
                "Connection: close\r\n"
                "\r\nhello", &r) == NULL);
    CHECK_EQ_INT(r.status, 200);
    CHECK_EQ_STR(r.reason, "OK");
    CHECK_EQ_INT(r.header_count, 3);
    CHECK_EQ_STR(r.headers[1].value, "5");
    CHECK_EQ_INT(r.body_len, 5);
    CHECK_EQ_INT(r.head_len, strlen("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                                    "Content-Length:  5 \r\nConnection: close\r\n\r\n"));

    /* Content-Length 없음 → EOF 까지 */
    CHECK(parse("HTTP/1.1 404 Not Found\r\nX: y\r\n\r\n", &r) == NULL);
    CHECK_EQ_INT(r.status, 404);
    CHECK_EQ_INT(r.body_len, -1);

    /* 204/304 는 본문 없음 */
    CHECK(parse("HTTP/1.1 204 No Content\r\n\r\n", &r) == NULL);
    CHECK_EQ_INT(r.body_len, 0);
    CHECK(parse("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n", &r) == NULL);
    CHECK_EQ_INT(r.body_len, 0);

    /* 문구 없음, 같은 Content-Length 여러 개 */
    CHECK(parse("HTTP/1.0 201\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\n", &r) == NULL);
    CHECK_EQ_INT(r.status, 201);
    CHECK_EQ_STR(r.reason, "");
    CHECK_EQ_INT(r.body_len, 2);
}

static void test_parse_response_bad(void)
{
    struct proxy_response r;
    static const char *const bad[] = {
        "HTTP/1.0 200 OK\r\nContent-Length: 5",                 /* 헤더 끝 없음 */
        "HTTP/2 200 OK\r\n\r\n",
        "HTTP/1.0 20 OK\r\n\r\n",
        "HTTP/1.0 2000 OK\r\n\r\n",
        "HTTP/1.0 600 X\r\n\r\n",
        "HTTP/1.0 200OK\r\n\r\n",
        "http/1.0 200 OK\r\n\r\n",
        "HTTP/1.1 100 Continue\r\n\r\n",                         /* 1xx */
        "HTTP/1.0 200 OK\r\nContent-Length: abc\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: \r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 1, 1\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
        "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.0 200 OK\r\nX: a\r\n  folded\r\n\r\n",           /* obs-fold */
        "HTTP/1.0 200 OK\r\nNoColon\r\n\r\n",
        "HTTP/1.0 200 OK\r\n: empty\r\n\r\n",
        "HTTP/1.0 200 OK\r\nBad Name: x\r\n\r\n",
        "HTTP/1.0 200 OK\r\nX: a\rb\r\n\r\n",                    /* 줄 안의 CR */
        "HTTP/1.0 200 O\nK\r\n\r\n",
        "\r\n\r\n",
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char *why = parse(bad[i], &r);
        CHECK(why != NULL);
        if (why == NULL)
            fprintf(stderr, "    accepted: %s\n", bad[i]);
    }
}

static void test_parse_response_nul(void)
{
    char buf[] = "HTTP/1.0 200 OK\r\nX: a\0b\r\n\r\n";
    struct proxy_response r;

    CHECK(proxy_parse_response(buf, sizeof buf - 1, &r) != NULL);
}

static void test_format_response_head(void)
{
    struct proxy_response r;
    char out[1024];
    int n;

    CHECK(parse("HTTP/1.0 200 OK\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: 5\r\n"
                "Connection: close, X-Hop\r\n"
                "Keep-Alive: timeout=5\r\n"
                "X-Hop: 1\r\n"
                "Server: backend\r\n"
                "\r\n", &r) == NULL);

    n = proxy_format_response_head(&r, 1, out, sizeof out);
    CHECK_EQ_STR(out,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Content-Length: 5\r\n"
                 "Server: backend\r\n"
                 "\r\n");
    CHECK_EQ_INT(n, (long long)strlen(out));

    proxy_format_response_head(&r, 0, out, sizeof out);
    CHECK_EQ_STR(out,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Content-Length: 5\r\n"
                 "Server: backend\r\n"
                 "Connection: close\r\n"
                 "\r\n");

    CHECK_EQ_INT(proxy_format_response_head(&r, 0, out, 30), -1);
}

int main(void)
{
    RUN_TEST(test_parse_rule_ok);
    RUN_TEST(test_parse_rule_bad);
    RUN_TEST(test_match);
    RUN_TEST(test_is_self);
    RUN_TEST(test_build_request);
    RUN_TEST(test_parse_response_ok);
    RUN_TEST(test_parse_response_bad);
    RUN_TEST(test_parse_response_nul);
    RUN_TEST(test_format_response_head);
    return TEST_SUMMARY();
}
