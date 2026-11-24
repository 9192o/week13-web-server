#include "logger.h"
#include "test_harness.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

static size_t read_all(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = read(fd, buf + total, cap - total - 1);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return total;
}

void test_logger_basic(void) {
    int fds[2];
    TEST_ASSERT_EQ_INT(pipe(fds), 0);
    TEST_ASSERT_EQ_INT(logger_start(fds[1], 8), 0);

    logger_log("hello %d", 42);
    logger_access("127.0.0.1", "GET", "/index.html", 200, 123);
    logger_stop();
    close(fds[1]);

    char buf[1024];
    size_t len = read_all(fds[0], buf, sizeof buf);
    close(fds[0]);

    TEST_ASSERT(len > 0);
    TEST_ASSERT(strstr(buf, "hello 42\n") != NULL);
    TEST_ASSERT(strstr(buf, "127.0.0.1 \"GET /index.html\" 200 123\n") != NULL);
}

void test_logger_not_running(void) {
    TEST_ASSERT_EQ_INT(logger_running(), 0);
}

void test_logger_drop(void) {
    int fds[2];
    TEST_ASSERT_EQ_INT(pipe(fds), 0);
    signal(SIGPIPE, SIG_IGN);
    TEST_ASSERT_EQ_INT(logger_start(fds[1], 1), 0);

    char line[480];
    memset(line, 'x', sizeof line - 1);
    line[sizeof line - 1] = '\0';
    for (int i = 0; i < 2000; i++)
        logger_log("%s", line);

    size_t dropped = logger_dropped();
    TEST_ASSERT(dropped > 0);

    close(fds[0]);
    logger_stop();
    close(fds[1]);
    TEST_ASSERT_EQ_INT(logger_running(), 0);
}
