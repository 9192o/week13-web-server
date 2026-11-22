#include "http_response.h"
#include "test.h"

#include <stdlib.h>

/* RFC 9110 예시 시각: Sun, 06 Nov 1994 08:49:37 GMT */
#define FIXED_TIME ((time_t)784111777)

static char buf[4096];

static void test_reason_table(void)
{
    CHECK_EQ_STR(http_reason(200), "OK");
    CHECK_EQ_STR(http_reason(404), "Not Found");
    CHECK_EQ_STR(http_reason(408), "Request Timeout");
    CHECK_EQ_STR(http_reason(414), "URI Too Long");
    CHECK_EQ_STR(http_reason(431), "Request Header Fields Too Large");
    CHECK_EQ_STR(http_reason(505), "HTTP Version Not Supported");
    CHECK(http_reason(418) == NULL);
    CHECK(http_reason(0) == NULL);
}

static void test_normalize_status(void)
{
    CHECK_EQ_INT(http_normalize_status(404), 404);
    CHECK_EQ_INT(http_normalize_status(418), 500);   /* stderr 에 BUG 로그 */
    CHECK_EQ_INT(http_normalize_status(-1), 500);
}

static void test_format_head_exact(void)
{
    const char *expected =
        "HTTP/1.1 404 Not Found\r\n"
        "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
        "Server: week13-webserver\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: 162\r\n"
        "Connection: close\r\n"
        "\r\n";
    int n = http_format_head(buf, sizeof buf, 404, "text/html; charset=utf-8",
                             162, NULL, FIXED_TIME);
    CHECK_EQ_INT(n, strlen(expected));
    CHECK_EQ_STR(buf, expected);
}

static void test_format_head_large_length(void)
{
    /* S10: 100MB 파일도 Content-Length 로 그대로 표현 */
    CHECK(http_format_head(buf, sizeof buf, 200, "application/octet-stream",
                           104857600LL, NULL, FIXED_TIME) > 0);
    CHECK(strstr(buf, "\r\nContent-Length: 104857600\r\n") != NULL);
    CHECK(strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0);
}

static void test_format_head_extra_headers(void)
{
    /* 301 Location, 405 Allow: Content-Length 뒤, Connection 앞에 그대로 붙는다 */
    CHECK(http_format_head(buf, sizeof buf, 301, "text/html; charset=utf-8", 0,
                           "Location: /docs/\r\n", FIXED_TIME) > 0);
    CHECK(strncmp(buf, "HTTP/1.1 301 Moved Permanently\r\n", 32) == 0);
    CHECK(strstr(buf, "Content-Length: 0\r\nLocation: /docs/\r\n"
                      "Connection: close\r\n\r\n") != NULL);

    CHECK(http_format_head(buf, sizeof buf, 405, "text/html; charset=utf-8", 0,
                           "Allow: GET\r\n", FIXED_TIME) > 0);
    CHECK(strncmp(buf, "HTTP/1.1 405 Method Not Allowed\r\n", 33) == 0);
    CHECK(strstr(buf, "\r\nAllow: GET\r\n") != NULL);
}

static void test_format_head_unknown_status_becomes_500(void)
{
    CHECK(http_format_head(buf, sizeof buf, 418, "text/plain", 0, NULL, FIXED_TIME) > 0);
    CHECK(strncmp(buf, "HTTP/1.1 500 Internal Server Error\r\n", 36) == 0);
}

static void test_format_head_overflow(void)
{
    char small[32];
    CHECK_EQ_INT(http_format_head(small, sizeof small, 200, "text/plain", 0,
                                  NULL, FIXED_TIME), -1);
    /* 경계: 정확히 필요한 크기보다 1 작으면 실패, 딱 맞으면(+NUL) 성공 */
    int need = http_format_head(buf, sizeof buf, 200, "text/plain", 0, NULL, FIXED_TIME);
    CHECK_EQ_INT(http_format_head(buf, (size_t)need, 200, "text/plain", 0,
                                  NULL, FIXED_TIME), -1);
    CHECK_EQ_INT(http_format_head(buf, (size_t)need + 1, 200, "text/plain", 0,
                                  NULL, FIXED_TIME), need);
}

static void test_error_body(void)
{
    int n = http_format_error_body(buf, sizeof buf, 404);
    CHECK(n > 0);
    CHECK_EQ_INT(n, strlen(buf));                     /* Content-Length 와 일치 */
    CHECK(strstr(buf, "<h1>404 Not Found</h1>") != NULL);
    CHECK(strstr(buf, "<title>404 Not Found</title>") != NULL);

    n = http_format_error_body(buf, sizeof buf, 418);
    CHECK(strstr(buf, "<h1>500 Internal Server Error</h1>") != NULL);

    CHECK_EQ_INT(http_format_error_body(buf, 10, 404), -1);
}

static void test_error_body_fits_server_buffer(void)
{
    /* server.c 의 send_error 는 512 바이트 버퍼를 쓴다: 가장 긴 문구도 들어가야 함 */
    static const int all[] = {200, 301, 400, 403, 404, 408, 411, 413, 414, 431,
                              405, 500, 501, 505};
    size_t i;
    char b[512];
    for (i = 0; i < sizeof all / sizeof all[0]; i++)
        CHECK(http_format_error_body(b, sizeof b, all[i]) > 0);
}

int main(void)
{
    RUN_TEST(test_reason_table);
    RUN_TEST(test_normalize_status);
    RUN_TEST(test_format_head_exact);
    RUN_TEST(test_format_head_large_length);
    RUN_TEST(test_format_head_extra_headers);
    RUN_TEST(test_format_head_unknown_status_becomes_500);
    RUN_TEST(test_format_head_overflow);
    RUN_TEST(test_error_body);
    RUN_TEST(test_error_body_fits_server_buffer);
    return TEST_SUMMARY();
}
