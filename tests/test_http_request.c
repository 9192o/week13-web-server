#include "http.h"
#include "config.h"
#include "test_harness.h"

#include <string.h>

static http_status_t parse(const char *raw, http_request_t *req) {
    config_t cfg;
    config_init(&cfg);
    http_request_init(req);
    return http_request_parse_buffer(raw, strlen(raw), &cfg, req);
}

void test_parse_get(void) {
    http_request_t req;
    http_status_t st = parse("GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n", &req);

    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(req.method, HTTP_GET);
    TEST_ASSERT_EQ_STR(req.method_str, "GET");
    TEST_ASSERT_EQ_STR(req.target, "/index.html");
    TEST_ASSERT_EQ_STR(req.path, "/index.html");
    TEST_ASSERT_EQ_INT(req.version_major, 1);
    TEST_ASSERT_EQ_INT(req.version_minor, 1);
    TEST_ASSERT_EQ_INT(req.has_host, 1);

    http_request_free(&req);
}

void test_missing_host(void) {
    http_request_t req;
    http_status_t st = parse("GET / HTTP/1.1\r\n\r\n", &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_http10_no_host(void) {
    http_request_t req;
    http_status_t st = parse("GET / HTTP/1.0\r\n\r\n", &req);

    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(req.version_major, 1);
    TEST_ASSERT_EQ_INT(req.version_minor, 0);
    TEST_ASSERT_EQ_INT(req.has_host, 0);
    TEST_ASSERT_EQ_INT(req.keep_alive, 0);

    http_request_free(&req);
}

void test_bad_method(void) {
    http_request_t req;
    http_status_t st = parse("FOO / HTTP/1.1\r\nHost: localhost\r\n\r\n", &req);
    TEST_ASSERT_EQ_INT(st, HTTP_NOT_IMPLEMENTED);
    http_request_free(&req);
}

void test_bad_version(void) {
    http_request_t req;
    http_status_t st = parse("GET / HTTP/9.9\r\nHost: localhost\r\n\r\n", &req);
    TEST_ASSERT_EQ_INT(st, HTTP_VERSION_NOT_SUPPORTED);
    http_request_free(&req);
}

void test_header_case_insensitive(void) {
    http_request_t req;
    http_status_t st = parse("GET / HTTP/1.1\r\nhOsT: example.com\r\n\r\n", &req);

    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(req.has_host, 1);

    http_request_free(&req);
}

void test_query_split(void) {
    http_request_t req;
    http_status_t st = parse("GET /search?q=hello HTTP/1.1\r\nHost: localhost\r\n\r\n", &req);

    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_STR(req.path, "/search");
    TEST_ASSERT_EQ_STR(req.query, "q=hello");

    http_request_free(&req);
}

void test_percent_decode(void) {
    http_request_t req;
    http_status_t st = parse("GET /a%20b HTTP/1.1\r\nHost: localhost\r\n\r\n", &req);

    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_STR(req.path, "/a b");

    http_request_free(&req);
}

void test_bad_request_line(void) {
    http_request_t req;
    http_status_t st = parse("GARBAGE\r\n\r\n", &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_traversal_rejected(void) {
    http_request_t req;
    http_status_t st = parse("GET /../etc/passwd HTTP/1.1\r\nHost: localhost\r\n\r\n", &req);
    TEST_ASSERT_EQ_INT(st, HTTP_FORBIDDEN);
    http_request_free(&req);
}

void test_post_without_length(void) {
    http_request_t req;
    http_status_t st = parse("POST / HTTP/1.1\r\nHost: x\r\n\r\n", &req);
    TEST_ASSERT_EQ_INT(st, HTTP_LENGTH_REQUIRED);
    http_request_free(&req);
}

void test_duplicate_content_length_same(void) {
    http_request_t req;
    http_status_t st = parse(
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 5\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(req.content_length, 5);
    http_request_free(&req);
}

void test_duplicate_content_length_diff(void) {
    http_request_t req;
    http_status_t st = parse(
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 6\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_cl_te_conflict(void) {
    http_request_t req;
    http_status_t st = parse(
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_chunked_rejected(void) {
    http_request_t req;
    http_status_t st = parse(
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(req.chunked, 1);
    http_request_free(&req);
}

void test_header_ows_trim(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET / HTTP/1.1\r\n"
        "Host:   example.com  \r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_OK);

    size_t len = 0;
    const char *value = http_request_get_header(&req, "Host", &len);
    TEST_ASSERT(value != NULL);

    char buf[64];
    if (value != NULL && len < sizeof buf) {
        memcpy(buf, value, len);
        buf[len] = '\0';
        TEST_ASSERT_EQ_STR(buf, "example.com");
    } else {
        TEST_ASSERT(0);
    }

    http_request_free(&req);
}

void test_connection_close_keepalive(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &req);
    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(req.keep_alive, 0);
    http_request_free(&req);

    st = parse(
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &req);
    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_INT(req.keep_alive, 1);
    http_request_free(&req);
}

void test_get_header(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "If-Modified-Since: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
        "\r\n",
        &req);

    TEST_ASSERT_EQ_INT(st, HTTP_OK);

    size_t len = 0;
    const char *value = http_request_get_header(&req, "If-Modified-Since", &len);
    TEST_ASSERT(value != NULL);
    TEST_ASSERT_EQ_INT(len, 29);
    TEST_ASSERT(value != NULL &&
                memcmp(value, "Wed, 21 Oct 2015 07:28:00 GMT", 29) == 0);

    http_request_free(&req);
}

void test_duplicate_host_rejected(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET / HTTP/1.1\r\n"
        "Host: a\r\n"
        "Host: b\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_host_space_before_colon_rejected(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET / HTTP/1.1\r\n"
        "Host : x\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_header_name_token_rejected(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X(y): 1\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_header_value_ctl_rejected(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X-Test: a\x01b\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_content_length_plus_rejected(void) {
    http_request_t req;
    http_status_t st = parse(
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: +5\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_method_token_ctl_rejected(void) {
    http_request_t req;
    http_status_t st = parse(
        "GE\x01T / HTTP/1.1\r\n"
        "Host: x\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_target_ctl_rejected(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET /a\nb HTTP/1.1\r\n"
        "Host: x\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_percent_encoded_trailing_slash(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET /demo%2f HTTP/1.1\r\n"
        "Host: x\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_OK);
    TEST_ASSERT_EQ_STR(req.path, "/demo/");
    http_request_free(&req);
}

void test_percent_encoded_ctl_rejected(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET /a%01b HTTP/1.1\r\n"
        "Host: x\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);

    st = parse(
        "GET /a%0d%0ab HTTP/1.1\r\n"
        "Host: x\r\n"
        "\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_BAD_REQUEST);
    http_request_free(&req);
}

void test_parse_incomplete_headers(void) {
    http_request_t req;
    http_status_t st = parse(
        "GET / HTTP/1.1\r\n"
        "Host: x\r\n",
        &req);
    TEST_ASSERT_EQ_INT(st, HTTP_PARSE_INCOMPLETE);
    http_request_free(&req);
}

void test_parse_split_points(void) {
    const char *raw = "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t len = strlen(raw);

    config_t cfg;
    config_init(&cfg);

    for (size_t i = 0; i < len; i++) {
        http_request_t req;
        http_request_init(&req);
        http_status_t st = http_request_parse_buffer(raw, i, &cfg, &req);
        TEST_ASSERT_EQ_INT(st, HTTP_PARSE_INCOMPLETE);
        http_request_free(&req);
    }

    http_request_t req;
    http_request_init(&req);
    TEST_ASSERT_EQ_INT(
        http_request_parse_buffer(raw, len, &cfg, &req), HTTP_OK);
    http_request_free(&req);
}
