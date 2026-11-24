#include "http_parser.h"
#include "test.h"

#include <stdlib.h>

/* 파서는 버퍼를 제자리에서 고치므로, 리터럴을 수정 가능한 버퍼로 복사해서 넘긴다. */
static char buf[32768];
static struct http_request req;

static int parse(const char *s)
{
    size_t n = strlen(s);
    memcpy(buf, s, n);
    return http_parse_request(buf, n, &req);
}

/* "GET /<n개의 a> HTTP/1.1" 요청 */
static int parse_long_uri(size_t n)
{
    size_t pos = 0;
    pos += (size_t)sprintf(buf, "GET /");
    memset(buf + pos, 'a', n - 1);     /* '/' 포함해서 URI 길이 n */
    pos += n - 1;
    pos += (size_t)sprintf(buf + pos, " HTTP/1.1\r\nHost: x\r\n\r\n");
    return http_parse_request(buf, pos, &req);
}

/* ---------- 정상 요청 ---------- */

static void test_s1_get(void)
{
    CHECK_EQ_INT(parse("GET /index.html HTTP/1.1\r\nHost: localhost:8080\r\n\r\n"), 0);
    CHECK_EQ_INT(req.method, HTTP_GET);
    CHECK_EQ_STR(req.method_str, "GET");
    CHECK_EQ_STR(req.path, "/index.html");
    CHECK(req.query == NULL);
    CHECK_EQ_INT(req.version_minor, 1);
    CHECK_EQ_STR(req.host, "localhost:8080");
    CHECK_EQ_INT(req.content_length, -1);
    CHECK(req.connection == NULL);
}

static void test_s5_query(void)
{
    CHECK_EQ_INT(parse("GET /cgi-bin/adder?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n"), 0);
    CHECK_EQ_STR(req.path, "/cgi-bin/adder");
    CHECK_EQ_STR(req.query, "a=1&b=2");

    CHECK_EQ_INT(parse("GET /a? HTTP/1.1\r\nHost: x\r\n\r\n"), 0);
    CHECK_EQ_STR(req.path, "/a");
    CHECK_EQ_STR(req.query, "");

    /* 두 번째 '?' 는 query 의 일부 */
    CHECK_EQ_INT(parse("GET /a?b?c HTTP/1.1\r\nHost: x\r\n\r\n"), 0);
    CHECK_EQ_STR(req.query, "b?c");
}

static void test_s6_post_with_body(void)
{
    const char *s = "POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                    "Content-Length: 7\r\n\r\na=1&b=2";
    CHECK_EQ_INT(parse(s), 0);
    CHECK_EQ_INT(req.method, HTTP_POST);
    CHECK_EQ_INT(req.content_length, 7);
    CHECK_EQ_INT(req.head_len, strlen(s) - 7);
    CHECK(memcmp(buf + req.head_len, "a=1&b=2", 7) == 0);  /* 본문은 그대로 */
}

static void test_headers_lookup_and_trim(void)
{
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\n"
                       "host:   example.com  \r\n"
                       "X-Empty:\r\n"
                       "CONNECTION: keep-alive\r\n"
                       "X-Tab:\tv\t\r\n\r\n"), 0);
    CHECK_EQ_INT(req.header_count, 4);
    CHECK_EQ_STR(req.host, "example.com");
    CHECK_EQ_STR(http_get_header(&req, "HOST"), "example.com");
    CHECK_EQ_STR(http_get_header(&req, "x-empty"), "");
    CHECK_EQ_STR(req.connection, "keep-alive");
    CHECK_EQ_STR(http_get_header(&req, "x-tab"), "v");
    CHECK(http_get_header(&req, "missing") == NULL);
    CHECK_EQ_STR(req.headers[0].name, "host");             /* 원문 대소문자 유지 */
}

static void test_http10_without_host(void)
{
    CHECK_EQ_INT(parse("GET / HTTP/1.0\r\n\r\n"), 0);
    CHECK_EQ_INT(req.version_minor, 0);
    CHECK(req.host == NULL);
}

static void test_obs_text_in_value_allowed(void)
{
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nX-Name: caf\xc3\xa9\r\n\r\n"), 0);
}

/* ---------- 414 ---------- */

static void test_414_uri_length(void)
{
    CHECK_EQ_INT(parse_long_uri(HTTP_URI_MAX), 0);
    CHECK_EQ_INT(parse_long_uri(HTTP_URI_MAX + 1), 414);
    CHECK_EQ_INT(parse_long_uri(9000), 414);                /* S8 */
}

static void test_414_comes_before_other_errors(void)
{
    /* 메서드도 틀리고 버전도 틀렸지만 URI 길이가 먼저 */
    size_t pos = (size_t)sprintf(buf, "DELETE /");
    memset(buf + pos, 'a', 9000);
    pos += 9000;
    pos += (size_t)sprintf(buf + pos, " HTTP/9.9\r\n\r\n");
    CHECK_EQ_INT(http_parse_request(buf, pos, &req), 414);
}

/* ---------- 400: 요청 라인 형식 ---------- */

static void test_400_request_line(void)
{
    CHECK_EQ_INT(parse("\r\n\r\n"), 400);                              /* 빈 요청 라인 */
    CHECK_EQ_INT(parse("GET\r\nHost: x\r\n\r\n"), 400);                /* 필드 부족 */
    CHECK_EQ_INT(parse("GET /\r\nHost: x\r\n\r\n"), 400);              /* 버전 없음 */
    CHECK_EQ_INT(parse("GET  / HTTP/1.1\r\nHost: x\r\n\r\n"), 400);    /* SP 두 개 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1 \r\nHost: x\r\n\r\n"), 400);    /* 끝 공백 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1 x\r\nHost: x\r\n\r\n"), 400);   /* 필드 초과 */
    CHECK_EQ_INT(parse("G(T / HTTP/1.1\r\nHost: x\r\n\r\n"), 400);     /* 토큰 아님 */
    CHECK_EQ_INT(parse("GET\t/ HTTP/1.1\r\nHost: x\r\n\r\n"), 400);    /* SP 대신 탭 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\nHost: x\r\n\r\n"), 400);       /* 단독 LF */
    CHECK_EQ_INT(parse("GET /\r HTTP/1.1\r\nHost: x\r\n\r\n"), 400);   /* 단독 CR */
}

static void test_400_version_format(void)
{
    CHECK_EQ_INT(parse("GET / HTTP/abc\r\nHost: x\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("GET / HTTP/1\r\nHost: x\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("GET / HTTP/1.10\r\nHost: x\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("GET / http/1.1\r\nHost: x\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("GET / HTTP/1,1\r\nHost: x\r\n\r\n"), 400);
}

static void test_400_uri_form(void)
{
    CHECK_EQ_INT(parse("GET http://a/ HTTP/1.1\r\nHost: x\r\n\r\n"), 400); /* absolute-form */
    CHECK_EQ_INT(parse("GET * HTTP/1.1\r\nHost: x\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("GET /a\x01 HTTP/1.1\r\nHost: x\r\n\r\n"), 400);    /* 제어문자 */
    CHECK_EQ_INT(parse("GET /\x7f HTTP/1.1\r\nHost: x\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("GET /caf\xc3\xa9 HTTP/1.1\r\nHost: x\r\n\r\n"), 400); /* 비 ASCII */
}

static void test_400_incomplete_head(void)
{
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\n"), 400);
    CHECK_EQ_INT(parse(""), 400);
}

/* ---------- 501 / 505 ---------- */

static void test_501_method(void)
{
    CHECK_EQ_INT(parse("DELETE /index.html HTTP/1.1\r\nHost: x\r\n\r\n"), 501); /* S7 */
    CHECK_EQ_INT(parse("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n"), 501);
    CHECK_EQ_INT(parse("get / HTTP/1.1\r\nHost: x\r\n\r\n"), 501);   /* 대소문자 구분 */
    CHECK_EQ_INT(parse("GETX / HTTP/1.1\r\nHost: x\r\n\r\n"), 501);
}

static void test_505_version(void)
{
    CHECK_EQ_INT(parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n"), 505);
    CHECK_EQ_INT(parse("GET / HTTP/1.2\r\nHost: x\r\n\r\n"), 505);
    CHECK_EQ_INT(parse("GET / HTTP/0.9\r\nHost: x\r\n\r\n"), 505);
}

static void test_check_order_line_before_headers(void)
{
    /* 형식 400 > 메서드 501 > 버전 505 > 헤더 오류 */
    CHECK_EQ_INT(parse("DELETE / HTTP/abc\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("DELETE / HTTP/2.0\r\n\r\n"), 501);
    CHECK_EQ_INT(parse("DELETE / HTTP/1.1\r\n\r\n"), 501);    /* Host 없어도 501 */
    CHECK_EQ_INT(parse("GET / HTTP/2.0\r\n\r\n"), 505);
}

/* ---------- 헤더 형식 ---------- */

static void test_400_header_format(void)
{
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost : x\r\n\r\n"), 400);    /* 이름 뒤 공백 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\n: x\r\nHost: x\r\n\r\n"), 400); /* 빈 이름 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nNoColon\r\nHost: x\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nX(y): 1\r\nHost: x\r\n\r\n"), 400); /* 토큰 아님 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n"), 400); /* obs-fold */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\n\tfolded\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\nX: y\r\n\r\n"), 400);    /* 단독 LF */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: a\rb\r\n\r\n"), 400);       /* 단독 CR */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: a\x01\r\n\r\n"), 400);      /* 값에 제어문자 */
}

static void test_431_too_many_headers(void)
{
    size_t pos, i;

    pos = (size_t)sprintf(buf, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (i = 1; i < HTTP_MAX_HEADERS; i++)          /* Host 포함 정확히 64개 */
        pos += (size_t)sprintf(buf + pos, "X-%zu: v\r\n", i);
    pos += (size_t)sprintf(buf + pos, "\r\n");
    CHECK_EQ_INT(http_parse_request(buf, pos, &req), 0);
    CHECK_EQ_INT(req.header_count, HTTP_MAX_HEADERS);

    pos = (size_t)sprintf(buf, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (i = 1; i <= HTTP_MAX_HEADERS; i++)         /* 65개 */
        pos += (size_t)sprintf(buf + pos, "X-%zu: v\r\n", i);
    pos += (size_t)sprintf(buf + pos, "\r\n");
    CHECK_EQ_INT(http_parse_request(buf, pos, &req), 431);
}

/* ---------- Content-Length / Transfer-Encoding ---------- */

static void test_content_length_values(void)
{
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"), 0);
    CHECK_EQ_INT(req.content_length, 0);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 007\r\n\r\n"), 0);
    CHECK_EQ_INT(req.content_length, 7);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\ncontent-length:  42 \r\n\r\n"), 0);
    CHECK_EQ_INT(req.content_length, 42);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 9223372036854775807\r\n\r\n"), 0);
}

static void test_400_content_length_invalid(void)
{
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 9223372036854775808\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1 2\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0x10\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5,5\r\n\r\n"), 400);
}

static void test_content_length_duplicates(void)
{
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 5\r\nContent-Length: 5\r\n\r\n"), 0);
    CHECK_EQ_INT(req.content_length, 5);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 5\r\nContent-Length: 6\r\n\r\n"), 400);
}

static void test_transfer_encoding(void)
{
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n"), 501);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\n"
                       "Transfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("POST / HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 5\r\ntransfer-encoding: chunked\r\n\r\n"), 400);
}

/* ---------- Host ---------- */

static void test_host_rules(void)
{
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\n\r\n"), 400);                       /* 1.1 에 없음 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"), 400); /* 중복 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: a\r\nhost: a\r\n\r\n"), 400); /* 같은 값 중복도 */
    CHECK_EQ_INT(parse("GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n"), 400);
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost:\r\n\r\n"), 0);                /* 빈 값은 존재로 봄 */
}

/* ---------- Connection: close 토큰 ---------- */

static void test_wants_close(void)
{
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), 0);
    CHECK_EQ_INT(http_wants_close(&req), 0);                 /* 헤더 없음 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), 0);
    CHECK_EQ_INT(http_wants_close(&req), 1);
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nconnection: CLOSE\r\n\r\n"), 0);
    CHECK_EQ_INT(http_wants_close(&req), 1);                 /* 대소문자 무시 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive , Close\r\n\r\n"), 0);
    CHECK_EQ_INT(http_wants_close(&req), 1);                 /* 토큰 목록 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n"
                       "Connection: close\r\n\r\n"), 0);
    CHECK_EQ_INT(http_wants_close(&req), 1);                 /* 두 번째 줄 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: closed, xclose\r\n\r\n"), 0);
    CHECK_EQ_INT(http_wants_close(&req), 0);                 /* 토큰 전체가 같아야 */
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nX-Connection: close\r\n\r\n"), 0);
    CHECK_EQ_INT(http_wants_close(&req), 0);                 /* 다른 헤더 */
}

/* ---------- 버퍼 초과 분류 ---------- */

static void test_classify_overflow(void)
{
    size_t pos;

    /* 요청 라인이 끝나지 않음 → 414 */
    memset(buf, 'a', 16384);
    memcpy(buf, "GET /", 5);
    CHECK_EQ_INT(http_classify_overflow(buf, 16384), 414);

    /* 요청 라인은 끝났지만 URI 가 상한 초과 → 414 */
    pos = (size_t)sprintf(buf, "GET /");
    memset(buf + pos, 'a', 9000);
    pos += 9000;
    pos += (size_t)sprintf(buf + pos, " HTTP/1.1\r\nX: ");
    memset(buf + pos, 'b', 16384 - pos);
    CHECK_EQ_INT(http_classify_overflow(buf, 16384), 414);

    /* 짧은 요청 라인 + 거대한 헤더 → 431 */
    pos = (size_t)sprintf(buf, "GET / HTTP/1.1\r\nCookie: ");
    memset(buf + pos, 'c', 16384 - pos);
    CHECK_EQ_INT(http_classify_overflow(buf, 16384), 431);
}

int main(void)
{
    RUN_TEST(test_s1_get);
    RUN_TEST(test_s5_query);
    RUN_TEST(test_s6_post_with_body);
    RUN_TEST(test_headers_lookup_and_trim);
    RUN_TEST(test_http10_without_host);
    RUN_TEST(test_obs_text_in_value_allowed);
    RUN_TEST(test_414_uri_length);
    RUN_TEST(test_414_comes_before_other_errors);
    RUN_TEST(test_400_request_line);
    RUN_TEST(test_400_version_format);
    RUN_TEST(test_400_uri_form);
    RUN_TEST(test_400_incomplete_head);
    RUN_TEST(test_501_method);
    RUN_TEST(test_505_version);
    RUN_TEST(test_check_order_line_before_headers);
    RUN_TEST(test_400_header_format);
    RUN_TEST(test_431_too_many_headers);
    RUN_TEST(test_content_length_values);
    RUN_TEST(test_400_content_length_invalid);
    RUN_TEST(test_content_length_duplicates);
    RUN_TEST(test_transfer_encoding);
    RUN_TEST(test_host_rules);
    RUN_TEST(test_wants_close);
    RUN_TEST(test_classify_overflow);
    return TEST_SUMMARY();
}
