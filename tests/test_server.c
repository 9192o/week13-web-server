#include "server.h"
#include "test.h"

#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 파이프에 data 를 미리 써 두고 쓰기 끝을 닫은 뒤, 읽기 fd 를 돌려준다. */
static int pipe_with(const char *data, size_t len)
{
    int p[2];
    if (pipe(p) < 0)
        abort();
    if (len > 0 && write_all(p[1], data, len) < 0)
        abort();
    close(p[1]);
    return p[0];
}

static void test_find_header_end(void)
{
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\nBODY";
    const char *end = find_header_end(req, strlen(req));
    CHECK(end != NULL);
    CHECK_EQ_STR(end, "BODY");

    CHECK(find_header_end("GET / HTTP/1.1\r\n", 16) == NULL);
    CHECK(find_header_end("\r\n\r", 3) == NULL);
    CHECK(find_header_end("", 0) == NULL);
    CHECK(find_header_end("\n\n", 2) == NULL);  /* CRLF 만 경계로 인정 */
}

static void test_read_head_ok(void)
{
    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    int fd = pipe_with(req, strlen(req));

    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, &len), READ_HEAD_OK);
    CHECK_EQ_INT(len, strlen(req));
    close(fd);
}

static void test_read_head_split_across_reads(void)
{
    /* \r\n\r\n 이 두 번의 read 경계에 걸쳐 도착하는 경우 */
    const char *part1 = "GET / HTTP/1.1\r\nHost: x\r\n\r";
    const char *part2 = "\n";
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    struct timespec delay = {0, 50 * 1000 * 1000};  /* 50ms */
    int p[2];
    pid_t pid;

    if (pipe(p) < 0)
        abort();
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        write_all(p[1], part1, strlen(part1));
        nanosleep(&delay, NULL);
        write_all(p[1], part2, strlen(part2));
        _exit(0);
    }
    close(p[1]);
    CHECK_EQ_INT(read_request_head(p[0], buf, sizeof buf, &len), READ_HEAD_OK);
    CHECK_EQ_INT(len, strlen(part1) + strlen(part2));
    close(p[0]);
    waitpid(pid, NULL, 0);
}

static void test_read_head_closed_early(void)
{
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\n";  /* 빈 줄 없음 */
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    int fd = pipe_with(req, strlen(req));

    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, &len),
                 READ_HEAD_CLOSED);
    CHECK_EQ_INT(len, strlen(req));
    close(fd);
}

static void test_read_head_empty_connection(void)
{
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    int fd = pipe_with("", 0);

    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, &len),
                 READ_HEAD_CLOSED);
    CHECK_EQ_INT(len, 0);
    close(fd);
}

static void test_read_head_too_large(void)
{
    size_t n = REQUEST_HEAD_MAX + 100;
    char *junk = malloc(n);
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    int fd;

    memset(junk, 'a', n);
    fd = pipe_with(junk, n);
    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, &len),
                 READ_HEAD_TOO_LARGE);
    CHECK_EQ_INT(len, REQUEST_HEAD_MAX);
    close(fd);
    free(junk);
}

static void test_read_head_exactly_fills_buffer(void)
{
    /* 버퍼를 정확히 채우면서 마지막 4바이트가 \r\n\r\n 이면 OK 여야 한다 */
    char *req = malloc(REQUEST_HEAD_MAX);
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    int fd;

    memset(req, 'a', REQUEST_HEAD_MAX);
    memcpy(req + REQUEST_HEAD_MAX - 4, "\r\n\r\n", 4);
    fd = pipe_with(req, REQUEST_HEAD_MAX);
    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, &len), READ_HEAD_OK);
    close(fd);
    free(req);
}

int main(void)
{
    RUN_TEST(test_find_header_end);
    RUN_TEST(test_read_head_ok);
    RUN_TEST(test_read_head_split_across_reads);
    RUN_TEST(test_read_head_closed_early);
    RUN_TEST(test_read_head_empty_connection);
    RUN_TEST(test_read_head_too_large);
    RUN_TEST(test_read_head_exactly_fills_buffer);
    return TEST_SUMMARY();
}
