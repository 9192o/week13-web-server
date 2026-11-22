#define _GNU_SOURCE
#include "connection.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define ASSERT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static struct timespec mock_now;

static int mock_clock_gettime(clockid_t clk, struct timespec *ts) {
    (void)clk;
    *ts = mock_now;
    return 0;
}

static int mock_setsockopt_last_fd;
static int mock_setsockopt_last_level;
static int mock_setsockopt_last_optname;
static struct timeval mock_setsockopt_last_tv;
static int mock_setsockopt_fail;

static int mock_setsockopt(int fd, int level, int optname, const void *val,
                           socklen_t len) {
    mock_setsockopt_last_fd = fd;
    mock_setsockopt_last_level = level;
    mock_setsockopt_last_optname = optname;
    if (len == sizeof(mock_setsockopt_last_tv)) {
        memcpy(&mock_setsockopt_last_tv, val, sizeof(mock_setsockopt_last_tv));
    }
    if (mock_setsockopt_fail) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static const char *mock_recv_data;
static size_t mock_recv_len;
static size_t mock_recv_pos;
static size_t mock_recv_chunk;
static int mock_recv_err;
static int mock_recv_err_after;
static int mock_recv_call;

static ssize_t mock_recv(int fd, void *buf, size_t len, int flags) {
    (void)fd;
    (void)flags;
    if (mock_recv_err_after >= 0 && mock_recv_call++ == mock_recv_err_after) {
        errno = mock_recv_err;
        return -1;
    }
    if (mock_recv_pos >= mock_recv_len) {
        return 0;
    }
    size_t want = mock_recv_chunk ? mock_recv_chunk : len;
    size_t left = mock_recv_len - mock_recv_pos;
    if (want > left) {
        want = left;
    }
    if (want > len) {
        want = len;
    }
    memcpy(buf, mock_recv_data + mock_recv_pos, want);
    mock_recv_pos += want;
    return (ssize_t)want;
}

static char mock_send_buf[4096];
static size_t mock_send_len;
static size_t mock_send_chunk;
static int mock_send_err;
static int mock_send_err_after;
static int mock_send_zero_after;
static int mock_send_call;

static ssize_t mock_send(int fd, const void *buf, size_t len, int flags) {
    (void)fd;
    (void)flags;
    if (mock_send_zero_after >= 0 && mock_send_call == mock_send_zero_after) {
        mock_send_call++;
        return 0;
    }
    if (mock_send_err_after >= 0 && mock_send_call++ == mock_send_err_after) {
        errno = mock_send_err;
        return -1;
    }
    size_t want = mock_send_chunk ? mock_send_chunk : len;
    if (want > len) {
        want = len;
    }
    size_t space = sizeof(mock_send_buf) - mock_send_len;
    if (want > space) {
        want = space;
    }
    memcpy(mock_send_buf + mock_send_len, buf, want);
    mock_send_len += want;
    return (ssize_t)want;
}

static void reset_mocks(void) {
    mock_now.tv_sec = 1000;
    mock_now.tv_nsec = 0;
    memset(&mock_setsockopt_last_tv, 0, sizeof(mock_setsockopt_last_tv));
    mock_setsockopt_last_fd = -1;
    mock_setsockopt_last_level = -1;
    mock_setsockopt_last_optname = -1;
    mock_setsockopt_fail = 0;
    mock_recv_data = NULL;
    mock_recv_len = 0;
    mock_recv_pos = 0;
    mock_recv_chunk = 0;
    mock_recv_err = 0;
    mock_recv_err_after = -1;
    mock_recv_call = 0;
    mock_send_len = 0;
    mock_send_chunk = 0;
    mock_send_err = 0;
    mock_send_err_after = -1;
    mock_send_zero_after = -1;
    mock_send_call = 0;
}

static void install_mocks(void) {
    connection_test_set_clock_gettime(mock_clock_gettime);
    connection_test_set_setsockopt(mock_setsockopt);
    connection_test_set_recv(mock_recv);
    connection_test_set_send(mock_send);
}

static void test_deadline_add(void) {
    struct timespec base = {1000, 0};
    struct timespec d;
    reset_mocks();
    install_mocks();

    ASSERT(connection_deadline_add(&base, 5000, NULL, &d) == 0);
    ASSERT(d.tv_sec == 1005);
    ASSERT(d.tv_nsec == 0);

    struct timespec earlier = {1002, 0};
    ASSERT(connection_deadline_add(&base, 5000, &earlier, &d) == 0);
    ASSERT(d.tv_sec == 1002);

    struct timespec later = {1007, 0};
    ASSERT(connection_deadline_add(&base, 5000, &later, &d) == 0);
    ASSERT(d.tv_sec == 1005);
}

static void test_deadline_from_now(void) {
    struct timespec d;
    reset_mocks();
    install_mocks();

    ASSERT(connection_deadline_from_now(5000, NULL, &d) == 0);
    ASSERT(d.tv_sec == 1005);

    struct timespec tighter = {1002, 0};
    ASSERT(connection_deadline_from_now(5000, &tighter, &d) == 0);
    ASSERT(d.tv_sec == 1002);
}

static void test_deadline_remaining(void) {
    struct timespec dl = {1010, 500000000};
    reset_mocks();
    install_mocks();

    mock_now.tv_sec = 1005;
    mock_now.tv_nsec = 0;
    ASSERT(connection_deadline_remaining_ms(&dl) == 5500);

    mock_now.tv_sec = 1010;
    mock_now.tv_nsec = 500000000;
    ASSERT(connection_deadline_remaining_ms(&dl) == 0);

    mock_now.tv_sec = 1011;
    ASSERT(connection_deadline_remaining_ms(&dl) == 0);
}

static void test_set_socket_timeout(void) {
    struct timespec dl;
    reset_mocks();
    install_mocks();

    mock_now.tv_sec = 1000;
    ASSERT(connection_deadline_from_now(50000, NULL, &dl) == 0);
    ASSERT(connection_set_socket_timeout(3, SO_RCVTIMEO, &dl) == 0);
    ASSERT(mock_setsockopt_last_fd == 3);
    ASSERT(mock_setsockopt_last_level == SOL_SOCKET);
    ASSERT(mock_setsockopt_last_optname == SO_RCVTIMEO);
    ASSERT(mock_setsockopt_last_tv.tv_sec == 50);
    ASSERT(mock_setsockopt_last_tv.tv_usec == 0);

    mock_now.tv_sec = 1055;
    ASSERT(connection_set_socket_timeout(3, SO_SNDTIMEO, &dl) == -1);
    ASSERT(mock_setsockopt_last_optname == SO_RCVTIMEO);
}

static void test_recv_some(void) {
    char buf[16];
    size_t got;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_recv_data = "hello";
    mock_recv_len = 5;
    mock_recv_chunk = 2;

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_recv_some(7, buf, 10, &dl, &got);
    ASSERT(r == CONN_RESULT_OK);
    ASSERT(got == 2);
    ASSERT(memcmp(buf, "he", 2) == 0);
    ASSERT(mock_setsockopt_last_fd == 7);
    ASSERT(mock_setsockopt_last_optname == SO_RCVTIMEO);
}

static void test_recv_some_eof(void) {
    char buf[16];
    size_t got;
    struct timespec dl;
    reset_mocks();
    install_mocks();

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_recv_some(7, buf, 10, &dl, &got);
    ASSERT(r == CONN_RESULT_EOF);
    ASSERT(got == 0);
}

static void test_recv_some_timeout(void) {
    char buf[16];
    size_t got;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_now.tv_sec = 1000;
    ASSERT(connection_deadline_from_now(100, NULL, &dl) == 0);
    mock_now.tv_sec = 1005;

    conn_result_t r = connection_recv_some(7, buf, 10, &dl, &got);
    ASSERT(r == CONN_RESULT_TIMEOUT);
    ASSERT(got == 0);
    ASSERT(mock_recv_call == 0);
}

static void test_recv_some_setsockopt_error(void) {
    char buf[16];
    size_t got;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_setsockopt_fail = 1;

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_recv_some(7, buf, 10, &dl, &got);
    ASSERT(r == CONN_RESULT_ERROR);
    ASSERT(got == 0);
    ASSERT(mock_recv_call == 0);
}

static void test_recv_some_eintr(void) {
    char buf[16];
    size_t got;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_recv_data = "xyz";
    mock_recv_len = 3;
    mock_recv_chunk = 1;
    mock_recv_err_after = 0;
    mock_recv_err = EINTR;

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_recv_some(7, buf, 10, &dl, &got);
    ASSERT(r == CONN_RESULT_OK);
    ASSERT(got == 1);
    ASSERT(buf[0] == 'x');
}

static void test_recv_some_reset(void) {
    char buf[16];
    size_t got;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_recv_err_after = 0;
    mock_recv_err = ECONNRESET;

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_recv_some(7, buf, 10, &dl, &got);
    ASSERT(r == CONN_RESULT_RESET);
    ASSERT(got == 0);
}

static void test_recv_all_complete(void) {
    char buf[16];
    size_t done;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_recv_data = "hello";
    mock_recv_len = 5;
    mock_recv_chunk = 2;

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_recv_all(7, buf, 5, &dl, &done);
    ASSERT(r == CONN_RESULT_OK);
    ASSERT(done == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);
}

static void test_recv_all_eof(void) {
    char buf[16];
    size_t done;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_recv_data = "ab";
    mock_recv_len = 2;

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_recv_all(7, buf, 5, &dl, &done);
    ASSERT(r == CONN_RESULT_EOF);
    ASSERT(done == 2);
}

static void test_send_all_complete(void) {
    size_t done;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_send_chunk = 2;

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_send_all(7, "hello", 5, &dl, &done);
    ASSERT(r == CONN_RESULT_OK);
    ASSERT(done == 5);
    ASSERT(mock_send_len == 5);
    ASSERT(memcmp(mock_send_buf, "hello", 5) == 0);
}

static void test_send_timeout(void) {
    size_t done;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_now.tv_sec = 1000;
    ASSERT(connection_deadline_from_now(100, NULL, &dl) == 0);
    mock_now.tv_sec = 1005;

    conn_result_t r = connection_send_all(7, "hello", 5, &dl, &done);
    ASSERT(r == CONN_RESULT_TIMEOUT);
    ASSERT(done == 0);
    ASSERT(mock_send_call == 0);
}

static void test_send_zero(void) {
    size_t done;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_send_zero_after = 0;

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_send_all(7, "hello", 5, &dl, &done);
    ASSERT(r == CONN_RESULT_ERROR);
    ASSERT(done == 0);
}

static void test_send_reset(void) {
    size_t done;
    struct timespec dl;
    reset_mocks();
    install_mocks();
    mock_send_err_after = 0;
    mock_send_err = EPIPE;

    ASSERT(connection_deadline_from_now(10000, NULL, &dl) == 0);
    conn_result_t r = connection_send_all(7, "hello", 5, &dl, &done);
    ASSERT(r == CONN_RESULT_RESET);
    ASSERT(done == 0);
}

static void test_stopwatch(void) {
    struct timespec start;
    reset_mocks();
    install_mocks();
    mock_now.tv_sec = 1000;
    mock_now.tv_nsec = 0;
    ASSERT(connection_stopwatch_start(&start) == 0);
    mock_now.tv_sec = 1001;
    mock_now.tv_nsec = 500000000;
    ASSERT(connection_stopwatch_elapsed_ms(&start) == 1500);
}

static int read_line(int fd, char *buf, size_t max) {
    size_t i = 0;
    char c;
    while (i + 1 < max && read(fd, &c, 1) == 1) {
        buf[i++] = c;
        if (c == '\n') {
            break;
        }
    }
    buf[i] = '\0';
    return (int)i;
}

static void test_log_access(void) {
    int pfd[2];
    char line[PIPE_BUF + 1];
    reset_mocks();
    install_mocks();
    ASSERT(pipe(pfd) == 0);

    conn_access_record_t rec = {
        .method = "GET",
        .path = "/index.html",
        .status = 200,
        .duration_ms = 42,
        .bytes_sent = 123,
        .complete = true,
    };
    ASSERT(connection_log_access(pfd[1], &rec) > 0);
    ASSERT(read_line(pfd[0], line, sizeof(line)) > 0);
    ASSERT(strstr(line, "\"method\":\"GET\"") != NULL);
    ASSERT(strstr(line, "\"path\":\"/index.html\"") != NULL);
    ASSERT(strstr(line, "\"status\":200") != NULL);
    ASSERT(strstr(line, "\"duration_ms\":42") != NULL);
    ASSERT(strstr(line, "\"bytes_sent\":123") != NULL);
    ASSERT(strstr(line, "\"complete\":true") != NULL);

    close(pfd[0]);
    close(pfd[1]);
}

static void test_log_access_null_and_escape(void) {
    int pfd[2];
    char line[PIPE_BUF + 1];
    reset_mocks();
    install_mocks();
    ASSERT(pipe(pfd) == 0);

    conn_access_record_t rec = {
        .method = NULL,
        .path = "/a\tb\"c",
        .status = -1,
        .duration_ms = 0,
        .bytes_sent = 0,
        .complete = false,
    };
    ASSERT(connection_log_access(pfd[1], &rec) > 0);
    ASSERT(read_line(pfd[0], line, sizeof(line)) > 0);
    ASSERT(strstr(line, "\"method\":null") != NULL);
    ASSERT(strstr(line, "\"path\":\"/a\\tb\\\"c\"") != NULL);
    ASSERT(strstr(line, "\"status\":null") != NULL);
    ASSERT(strstr(line, "\"complete\":false") != NULL);

    close(pfd[0]);
    close(pfd[1]);
}

static void test_log_access_truncation(void) {
    int pfd[2];
    char line[PIPE_BUF + 1];
    char long_path[CONNECTION_MAX_PATH_LEN + 10];
    reset_mocks();
    install_mocks();
    ASSERT(pipe(pfd) == 0);

    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    conn_access_record_t rec = {
        .method = "POST",
        .path = long_path,
        .status = 404,
        .duration_ms = 1,
        .bytes_sent = 0,
        .complete = true,
    };
    ASSERT(connection_log_access(pfd[1], &rec) > 0);
    ASSERT(read_line(pfd[0], line, sizeof(line)) > 0);
    ASSERT(strstr(line, "\"path\":\"" CONNECTION_PATH_TRUNCATED "\"") != NULL);

    close(pfd[0]);
    close(pfd[1]);
}

static void test_log_ready(void) {
    int pfd[2];
    char line[64];
    reset_mocks();
    install_mocks();
    ASSERT(pipe(pfd) == 0);

    ASSERT(connection_log_ready(pfd[1], 8080) > 0);
    ASSERT(read_line(pfd[0], line, sizeof(line)) > 0);
    ASSERT(strcmp(line, "READY 8080\n") == 0);

    ASSERT(connection_log_ready(pfd[1], -1) > 0);
    ASSERT(read_line(pfd[0], line, sizeof(line)) > 0);
    ASSERT(strcmp(line, "READY\n") == 0);

    close(pfd[0]);
    close(pfd[1]);
}

static void test_log_diag(void) {
    int pfd[2];
    char line[PIPE_BUF + 1];
    reset_mocks();
    install_mocks();
    ASSERT(pipe(pfd) == 0);

    conn_diag_record_t rec = {
        .phase = "send",
        .error_code = EPIPE,
        .message = "broken\npipe",
    };
    ASSERT(connection_log_diag(pfd[1], &rec) > 0);
    ASSERT(read_line(pfd[0], line, sizeof(line)) > 0);
    ASSERT(strstr(line, "\"phase\":\"send\"") != NULL);
    ASSERT(strstr(line, "\"error\":32") != NULL);
    ASSERT(strstr(line, "\"message\":\"broken\\npipe\"") != NULL);

    close(pfd[0]);
    close(pfd[1]);
}

static void test_log_broken_pipe_safe(void) {
    int pfd[2];
    reset_mocks();
    install_mocks();
    ASSERT(pipe(pfd) == 0);
    close(pfd[0]);

    signal(SIGPIPE, SIG_IGN);
    conn_access_record_t rec = {
        .method = "GET",
        .path = "/",
        .status = 200,
        .duration_ms = 0,
        .bytes_sent = 0,
        .complete = true,
    };
    ASSERT(connection_log_access(pfd[1], &rec) == -1);
    close(pfd[1]);
}

static void test_drain(void) {
    int pfd[2];
    char data[] = "abcd";
    size_t drained;
    reset_mocks();
    install_mocks();
    ASSERT(pipe(pfd) == 0);
    ASSERT(write(pfd[1], data, sizeof(data) - 1) == (ssize_t)(sizeof(data) - 1));
    close(pfd[1]);

    conn_result_t r = connection_drain(pfd[0], 2, &drained);
    ASSERT(r == CONN_RESULT_OK);
    ASSERT(drained == 2);

    r = connection_drain(pfd[0], 10, &drained);
    ASSERT(r == CONN_RESULT_OK);
    ASSERT(drained == 2);

    close(pfd[0]);
}

static void test_log_fd_prepare_devnull(void) {
    int fd = open("/dev/null", O_WRONLY);
    ASSERT(fd >= 0);
    int log_fd = connection_log_fd_prepare(fd);
    ASSERT(log_fd >= 0);
    ASSERT(log_fd != fd);
    ASSERT(write(log_fd, "x", 1) == 1);

    struct stat st;
    ASSERT(fstat(log_fd, &st) == 0);
    ASSERT(S_ISCHR(st.st_mode));

    close(log_fd);
    close(fd);
}

int main(void) {
    failures = 0;

    test_deadline_add();
    test_deadline_from_now();
    test_deadline_remaining();
    test_set_socket_timeout();
    test_recv_some();
    test_recv_some_eof();
    test_recv_some_timeout();
    test_recv_some_eintr();
    test_recv_some_setsockopt_error();
    test_recv_some_reset();
    test_recv_all_complete();
    test_recv_all_eof();
    test_send_all_complete();
    test_send_timeout();
    test_send_zero();
    test_send_reset();
    test_stopwatch();
    test_log_access();
    test_log_access_null_and_escape();
    test_log_access_truncation();
    test_log_ready();
    test_log_diag();
    test_log_broken_pipe_safe();
    test_drain();
    test_log_fd_prepare_devnull();

    if (failures) {
        fprintf(stderr, "CONNECTION_TESTS_FAIL: %d\n", failures);
        return 1;
    }
    printf("CONNECTION_TESTS_PASS\n");
    return 0;
}
