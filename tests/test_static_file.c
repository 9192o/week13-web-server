#include "test_harness.h"
#include "static_file_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int make_temp_file(char *path, size_t path_size,
                          const char *contents, size_t len) {
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || *dir == '\0')
        dir = "/tmp";

    int n = snprintf(path, path_size, "%s/week13_send_file.XXXXXX", dir);
    if (n < 0 || (size_t)n >= path_size)
        return -1;

    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, contents + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (w == 0) {
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }

    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void test_send_file_exact_size(void) {
    char path[PATH_MAX];
    int filefd = make_temp_file(path, sizeof path, "hello", 5);
    if (filefd < 0) {
        TEST_ASSERT(0);
        return;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        TEST_ASSERT(0);
        close(filefd);
        return;
    }

    TEST_ASSERT_EQ_INT(send_file(sv[0], filefd, 5), 0);

    char buf[8] = {0};
    ssize_t n = read(sv[1], buf, sizeof buf);
    TEST_ASSERT_EQ_INT(n, 5);
    TEST_ASSERT_EQ_STR(buf, "hello");

    close(sv[0]);
    close(sv[1]);
    close(filefd);
}

void test_send_file_short_eof(void) {
    char path[PATH_MAX];
    int filefd = make_temp_file(path, sizeof path, "hello", 5);
    if (filefd < 0) {
        TEST_ASSERT(0);
        return;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        TEST_ASSERT(0);
        close(filefd);
        return;
    }

    /* The file has 5 bytes, but Content-Length said 10. */
    TEST_ASSERT_EQ_INT(send_file(sv[0], filefd, 10), -1);

    char buf[8] = {0};
    ssize_t n = read(sv[1], buf, sizeof buf);
    TEST_ASSERT_EQ_INT(n, 5);
    TEST_ASSERT_EQ_STR(buf, "hello");

    close(sv[0]);
    close(sv[1]);
    close(filefd);
}
