#include "http.h"
#include "test_harness.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static size_t read_all(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = read(fd, buf + total, cap - total - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return total;
}

static void make_pair(int sv[2]) {
    TEST_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
}

void test_response_send_memory(void) {
    int sv[2];
    make_pair(sv);

    TEST_ASSERT_EQ_INT(
        http_response_send_memory(sv[0], 200, "text/plain", "hello", 5), 0);
    close(sv[0]);

    char buf[1024];
    read_all(sv[1], buf, sizeof buf);
    close(sv[1]);

    TEST_ASSERT(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "Content-Type: text/plain\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "Content-Length: 5\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "\r\n\r\nhello") != NULL);
}

void test_response_error_404(void) {
    int sv[2];
    make_pair(sv);

    TEST_ASSERT_EQ_INT(http_response_send_error(sv[0], 404, 1), 0);
    close(sv[0]);

    char buf[2048];
    read_all(sv[1], buf, sizeof buf);
    close(sv[1]);

    TEST_ASSERT(strstr(buf, "HTTP/1.1 404 Not Found\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "Connection: close\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "<h1>404 Not Found</h1>") != NULL);
}

void test_response_reason(void) {
    TEST_ASSERT_EQ_STR(http_status_reason(200), "OK");
    TEST_ASSERT_EQ_STR(http_status_reason(400), "Bad Request");
    TEST_ASSERT_EQ_STR(http_status_reason(404), "Not Found");
    TEST_ASSERT_EQ_STR(http_status_reason(500), "Internal Server Error");
    TEST_ASSERT_EQ_STR(http_status_reason(599), "Unknown");
}

void test_response_add_header_overflow(void) {
    http_response_t res;
    http_response_init(&res);

    char big[HTTP_MAX_RESPONSE_HEADERS + 16];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';

    TEST_ASSERT_EQ_INT(http_response_add_header(&res, "X-Big", big), -1);
}

void test_response_format(void) {
    http_response_t res;
    http_response_init(&res);
    res.status = 200;
    TEST_ASSERT_EQ_INT(
        http_response_add_header(&res, "X-Test", "yes"), 0);

    char buf[512];
    size_t len = 0;
    TEST_ASSERT_EQ_INT(
        http_response_format(&res, buf, sizeof buf, &len), 0);
    TEST_ASSERT(len > 0);
    TEST_ASSERT(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "X-Test: yes\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "\r\n\r\n") != NULL);
}

void test_response_format_overflow(void) {
    http_response_t res;
    http_response_init(&res);
    res.status = 200;
    TEST_ASSERT_EQ_INT(
        http_response_add_header(&res, "X-Test", "yes"), 0);

    char small[8];
    size_t len = 0;
    TEST_ASSERT_EQ_INT(
        http_response_format(&res, small, sizeof small, &len), -1);
}

void test_response_build_error(void) {
    http_response_t res;
    char body[512];
    size_t body_len = 0;

    TEST_ASSERT_EQ_INT(
        http_response_build_error(404, 1, &res, body, sizeof body,
                                  &body_len), 0);
    TEST_ASSERT_EQ_INT(res.status, 404);
    TEST_ASSERT(body_len > 0);
    TEST_ASSERT(strstr(body, "404 Not Found") != NULL);

    char buf[1024];
    size_t len = 0;
    TEST_ASSERT_EQ_INT(
        http_response_format(&res, buf, sizeof buf, &len), 0);
    TEST_ASSERT(strstr(buf, "HTTP/1.1 404 Not Found\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "Connection: close\r\n") != NULL);
}
