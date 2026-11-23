#include "server.h"
#include "test.h"

#include <stdlib.h>
#include <sys/socket.h>
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

    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, 1000, &len), READ_HEAD_OK);
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
    CHECK_EQ_INT(read_request_head(p[0], buf, sizeof buf, 1000, &len), READ_HEAD_OK);
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

    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, 1000, &len),
                 READ_HEAD_CLOSED);
    CHECK_EQ_INT(len, strlen(req));
    close(fd);
}

static void test_read_head_empty_connection(void)
{
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    int fd = pipe_with("", 0);

    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, 1000, &len),
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
    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, 1000, &len),
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
    CHECK_EQ_INT(read_request_head(fd, buf, sizeof buf, 1000, &len), READ_HEAD_OK);
    close(fd);
    free(req);
}

/* 쓰기 끝을 열어 둔 채(EOF 없이) 데이터만 미리 넣어 둔다 → 시간 초과 유도 */
static void test_read_head_timeout_no_data(void)
{
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    int p[2];

    if (pipe(p) < 0)
        abort();
    CHECK_EQ_INT(read_request_head(p[0], buf, sizeof buf, 100, &len),
                 READ_HEAD_TIMEOUT);
    CHECK_EQ_INT(len, 0);                          /* 무응답으로 닫을 경우 */
    close(p[0]);
    close(p[1]);
}

static void test_read_head_timeout_partial(void)
{
    const char *part = "GET / HTTP/1.1\r\nHo";
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    int p[2];

    if (pipe(p) < 0)
        abort();
    write_all(p[1], part, strlen(part));
    CHECK_EQ_INT(read_request_head(p[0], buf, sizeof buf, 100, &len),
                 READ_HEAD_TIMEOUT);
    CHECK_EQ_INT(len, strlen(part));               /* 408 로 응답할 경우 */
    close(p[0]);
    close(p[1]);
}

/* 데드라인은 read 한 번이 아니라 헤더 전체 기준: 조금씩 계속 보내도 시간이 끝나면 TIMEOUT */
static void test_read_head_deadline_is_total(void)
{
    struct timespec tick = {0, 40 * 1000 * 1000};  /* 40ms 마다 1바이트 */
    char buf[REQUEST_HEAD_MAX];
    size_t len;
    int p[2];
    pid_t pid;

    if (pipe(p) < 0)
        abort();
    pid = fork();
    if (pid == 0) {
        int i;
        close(p[0]);
        for (i = 0; i < 20; i++) {                 /* 총 ~800ms 동안 조금씩 */
            if (write_all(p[1], "a", 1) < 0)
                break;
            nanosleep(&tick, NULL);
        }
        _exit(0);
    }
    close(p[1]);
    CHECK_EQ_INT(read_request_head(p[0], buf, sizeof buf, 200, &len),
                 READ_HEAD_TIMEOUT);
    CHECK(len > 0 && len < 20);
    close(p[0]);
    waitpid(pid, NULL, 0);
}

/* send_all 용 소켓쌍: sv[0] 이 보내는 쪽. 버퍼를 작게 해서 안 읽으면 금방 차게 */
static void small_socketpair(int sv[2])
{
    int sz = 4096;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        abort();
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
}

static void test_send_all_ok(void)
{
    char out[6] = {0};
    int sv[2];

    small_socketpair(sv);
    CHECK_EQ_INT(send_all(sv[0], "hello", 5, now_ms() + 1000), SEND_OK);
    CHECK_EQ_INT(read(sv[1], out, 5), 5);
    CHECK_EQ_STR(out, "hello");
    close(sv[0]);
    close(sv[1]);
}

/* 받는 쪽이 전혀 읽지 않으면 버퍼가 찬 뒤 deadline 에서 멈춘다 */
static void test_send_all_timeout_no_reader(void)
{
    size_t n = 1024 * 1024;
    char *big = calloc(1, n);
    long long t0, elapsed;
    int sv[2];

    small_socketpair(sv);
    t0 = now_ms();
    CHECK_EQ_INT(send_all(sv[0], big, n, t0 + 200), SEND_TIMEOUT);
    elapsed = now_ms() - t0;
    CHECK(elapsed >= 200 && elapsed < 1000);
    close(sv[0]);
    close(sv[1]);
    free(big);
}

/* 데드라인은 응답 전체 기준: 조금씩 계속 받아 가도(진행은 있음) 시간이 끝나면 TIMEOUT */
static void test_send_all_deadline_is_total(void)
{
    struct timespec tick = {0, 10 * 1000 * 1000};  /* 10ms 마다 512바이트 ≈ 50KB/s */
    size_t n = 1024 * 1024;
    char *big = calloc(1, n);
    long long t0, elapsed;
    int sv[2];
    pid_t pid;

    small_socketpair(sv);
    pid = fork();
    if (pid == 0) {
        char buf[512];
        close(sv[0]);
        while (read(sv[1], buf, sizeof buf) > 0)
            nanosleep(&tick, NULL);
        _exit(0);
    }
    close(sv[1]);
    t0 = now_ms();
    CHECK_EQ_INT(send_all(sv[0], big, n, t0 + 300), SEND_TIMEOUT);
    elapsed = now_ms() - t0;
    CHECK(elapsed >= 300 && elapsed < 1500);
    close(sv[0]);                                  /* 자식의 read 가 0 → 종료 */
    waitpid(pid, NULL, 0);
    free(big);
}

/* 상대가 닫았으면 SEND_ERROR (MSG_NOSIGNAL 이라 SIGPIPE 로 죽지 않음) */
static void test_send_all_peer_closed(void)
{
    int sv[2];

    small_socketpair(sv);
    close(sv[1]);
    CHECK_EQ_INT(send_all(sv[0], "x", 1, now_ms() + 1000), SEND_ERROR);
    close(sv[0]);
}

int main(void)
{
    RUN_TEST(test_send_all_ok);
    RUN_TEST(test_send_all_timeout_no_reader);
    RUN_TEST(test_send_all_deadline_is_total);
    RUN_TEST(test_send_all_peer_closed);
    RUN_TEST(test_find_header_end);
    RUN_TEST(test_read_head_timeout_no_data);
    RUN_TEST(test_read_head_timeout_partial);
    RUN_TEST(test_read_head_deadline_is_total);
    RUN_TEST(test_read_head_ok);
    RUN_TEST(test_read_head_split_across_reads);
    RUN_TEST(test_read_head_closed_early);
    RUN_TEST(test_read_head_empty_connection);
    RUN_TEST(test_read_head_too_large);
    RUN_TEST(test_read_head_exactly_fills_buffer);
    return TEST_SUMMARY();
}
