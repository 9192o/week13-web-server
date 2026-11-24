#include "http_upstream.h"
#include "test_harness.h"

#include <string.h>

static http_status_t parse_resp(const char *s, upstream_response_t *res) {
    config_t cfg;
    config_init(&cfg);
    upstream_response_init(res);
    return upstream_response_parse(s, strlen(s), &cfg, res);
}

void test_upstream_content_length(void) {
    upstream_response_t res;
    http_status_t st = parse_resp(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "\r\n",
        &res);
    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(res.status, 200);
    TEST_ASSERT_EQ_INT(res.has_content_length, 1);
    TEST_ASSERT_EQ_INT((int)res.content_length, 5);
    TEST_ASSERT_EQ_INT(res.chunked, 0);
}

void test_upstream_chunked(void) {
    upstream_response_t res;
    http_status_t st = parse_resp(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n",
        &res);
    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(res.chunked, 1);
    TEST_ASSERT_EQ_INT(res.has_content_length, 0);
}

void test_upstream_connection_close(void) {
    upstream_response_t res;
    http_status_t st = parse_resp(
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n",
        &res);
    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(res.connection_close, 1);
}

void test_upstream_incomplete(void) {
    upstream_response_t res;
    http_status_t st = parse_resp(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n",
        &res);
    TEST_ASSERT_EQ_INT(st, HTTP_PARSE_INCOMPLETE);
}

void test_upstream_cl_te_conflict(void) {
    upstream_response_t res;
    http_status_t st = parse_resp(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n",
        &res);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_GATEWAY);
}

void test_upstream_duplicate_cl(void) {
    upstream_response_t res;
    http_status_t st = parse_resp(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 6\r\n"
        "\r\n",
        &res);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_GATEWAY);
}

void test_upstream_bad_status(void) {
    upstream_response_t res;
    http_status_t st = parse_resp("HTTP/1.1 XX OK\r\n\r\n", &res);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_GATEWAY);
}
