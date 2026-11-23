#include "body.h"
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

static struct body_input input(const char *buf, size_t head_len, long long cl)
{
    struct body_input in;
    in.buf = buf;
    in.buffered_len = strlen(buf);
    in.head_len = head_len;
    in.content_length = cl;
    return in;
}

static void test_body_all_in_buffer(void)
{
    struct body_input in = input("HEADhello", 4, 5);
    char out[8] = {0};
    size_t consumed = 0;
    int fd = pipe_with("", 0);

    CHECK_EQ_INT(read_request_body(fd, &in, 1000, out, &consumed), READ_BODY_OK);
    CHECK_EQ_STR(out, "hello");
    CHECK_EQ_INT(consumed, 9);
    close(fd);
}

static void test_body_leftover_not_consumed(void)
{
    /* 버퍼에 다음 요청 일부가 붙어 있어도 Content-Length 까지만 쓴다 */
    struct body_input in = input("HEADhelloGET /next", 4, 5);
    char out[8] = {0};
    size_t consumed = 0;
    int fd = pipe_with("", 0);

    CHECK_EQ_INT(read_request_body(fd, &in, 1000, out, &consumed), READ_BODY_OK);
    CHECK_EQ_STR(out, "hello");
    CHECK_EQ_INT(consumed, 9);                     /* 나머지 9바이트는 다음 요청 몫 */
    close(fd);
}

static void test_body_split_across_reads(void)
{
    struct body_input in = input("HEADhe", 4, 5);
    struct timespec delay = {0, 50 * 1000 * 1000};
    char out[8] = {0};
    size_t consumed = 0;
    int p[2];
    pid_t pid;

    if (pipe(p) < 0)
        abort();
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        write_all(p[1], "l", 1);
        nanosleep(&delay, NULL);
        write_all(p[1], "lo", 2);
        _exit(0);
    }
    close(p[1]);
    CHECK_EQ_INT(read_request_body(p[0], &in, 1000, out, &consumed), READ_BODY_OK);
    CHECK_EQ_STR(out, "hello");
    CHECK_EQ_INT(consumed, 6);                     /* 헤더 4 + 버퍼에서 가져간 본문 2 */
    close(p[0]);
    waitpid(pid, NULL, 0);
}

static void test_body_does_not_over_read(void)
{
    /* 소켓에서도 Content-Length 를 넘겨 읽지 않는다 */
    struct body_input in = input("HEADhe", 4, 5);
    char out[8] = {0}, rest[8] = {0};
    size_t consumed = 0;
    int fd = pipe_with("lloEXTRA", 8);

    CHECK_EQ_INT(read_request_body(fd, &in, 1000, out, &consumed), READ_BODY_OK);
    CHECK_EQ_STR(out, "hello");
    CHECK_EQ_INT(read(fd, rest, sizeof rest - 1), 5);
    CHECK_EQ_STR(rest, "EXTRA");
    close(fd);
}

static void test_body_zero_length(void)
{
    struct body_input in = input("HEAD", 4, 0);
    size_t consumed = 0;
    int fd = pipe_with("", 0);

    CHECK_EQ_INT(read_request_body(fd, &in, 1000, NULL, &consumed), READ_BODY_OK);
    CHECK_EQ_INT(consumed, 4);
    close(fd);
}

static void test_body_closed_mid_body(void)
{
    /* S9: 본문 도중 끊김 */
    struct body_input in = input("HEADhe", 4, 5);
    char out[8];
    size_t consumed;
    int fd = pipe_with("l", 1);

    CHECK_EQ_INT(read_request_body(fd, &in, 1000, out, &consumed), READ_BODY_CLOSED);
    close(fd);
}

static void test_body_timeout(void)
{
    struct body_input in = input("HEADhe", 4, 5);
    char out[8];
    size_t consumed;
    int p[2];

    if (pipe(p) < 0)
        abort();
    write_all(p[1], "l", 1);                       /* 쓰기 끝을 열어 둔 채 멈춤 */
    CHECK_EQ_INT(read_request_body(p[0], &in, 100, out, &consumed),
                 READ_BODY_TIMEOUT);
    close(p[0]);
    close(p[1]);
}

static void test_discard(void)
{
    /* 4KB 임시 버퍼보다 큰 본문을 버리고, 그 뒤 바이트는 남겨 둔다 */
    size_t n = 10000 - 2;
    char *data = malloc(n + 1);
    struct body_input in = input("HEADhe", 4, 10000);
    char rest[4] = {0};
    size_t consumed = 0;
    int fd;

    memset(data, 'x', n);
    data[n] = 'Z';
    fd = pipe_with(data, n + 1);
    CHECK_EQ_INT(discard_request_body(fd, &in, 1000, &consumed), READ_BODY_OK);
    CHECK_EQ_INT(consumed, 6);
    CHECK_EQ_INT(read(fd, rest, sizeof rest - 1), 1);
    CHECK_EQ_STR(rest, "Z");
    close(fd);
    free(data);
}

static void test_discard_failures(void)
{
    struct body_input in = input("HEAD", 4, 5);
    size_t consumed;
    int p[2];
    int fd = pipe_with("ab", 2);

    CHECK_EQ_INT(discard_request_body(fd, &in, 1000, &consumed), READ_BODY_CLOSED);
    close(fd);

    if (pipe(p) < 0)
        abort();
    CHECK_EQ_INT(discard_request_body(p[0], &in, 100, &consumed), READ_BODY_TIMEOUT);
    close(p[0]);
    close(p[1]);
}

int main(void)
{
    RUN_TEST(test_body_all_in_buffer);
    RUN_TEST(test_body_leftover_not_consumed);
    RUN_TEST(test_body_split_across_reads);
    RUN_TEST(test_body_does_not_over_read);
    RUN_TEST(test_body_zero_length);
    RUN_TEST(test_body_closed_mid_body);
    RUN_TEST(test_body_timeout);
    RUN_TEST(test_discard);
    RUN_TEST(test_discard_failures);
    return TEST_SUMMARY();
}
