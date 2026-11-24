#include "event_backend.h"
#include "test_harness.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void make_pipe(int p[2]) {
    TEST_ASSERT_EQ_INT(pipe(p), 0);
}

static void test_read_basic(ev_loop_t *loop) {
    int p[2];
    make_pipe(p);

    int marker = 0;
    TEST_ASSERT_EQ_INT(ev_add(loop, p[0], EV_READ, &marker), 0);

    ev_event_t out[4];
    TEST_ASSERT_EQ_INT(ev_wait(loop, out, 4, 0), 0);

    TEST_ASSERT_EQ_INT(write(p[1], "x", 1), 1);
    int n = ev_wait(loop, out, 4, 1000);
    TEST_ASSERT_EQ_INT(n, 1);
    if (n == 1) {
        TEST_ASSERT_EQ_INT(out[0].fd, p[0]);
        TEST_ASSERT((out[0].events & EV_READ) != 0);
        TEST_ASSERT(out[0].ptr == &marker);

        char c;
        TEST_ASSERT_EQ_INT(read(p[0], &c, 1), 1);
    }

    TEST_ASSERT_EQ_INT(ev_del(loop, p[0]), 0);
    close(p[0]);
    close(p[1]);
}

static void test_mod_basic(ev_loop_t *loop) {
    int sv[2];
    TEST_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    int marker = 0;
    TEST_ASSERT_EQ_INT(ev_add(loop, sv[0], EV_READ, &marker), 0);
    TEST_ASSERT_EQ_INT(ev_mod(loop, sv[0], EV_WRITE, &marker), 0);

    ev_event_t out[4];
    int n = ev_wait(loop, out, 4, 1000);
    TEST_ASSERT_EQ_INT(n, 1);
    if (n == 1)
        TEST_ASSERT((out[0].events & EV_WRITE) != 0);

    TEST_ASSERT_EQ_INT(ev_mod(loop, sv[0], EV_READ, &marker), 0);
    TEST_ASSERT_EQ_INT(write(sv[1], "y", 1), 1);

    n = ev_wait(loop, out, 4, 1000);
    TEST_ASSERT_EQ_INT(n, 1);
    if (n == 1) {
        TEST_ASSERT((out[0].events & EV_READ) != 0);
        char c;
        TEST_ASSERT_EQ_INT(read(sv[0], &c, 1), 1);
    }

    TEST_ASSERT_EQ_INT(ev_del(loop, sv[0]), 0);
    close(sv[0]);
    close(sv[1]);
}

void test_event_poll_read(void) {
    ev_loop_t *loop = ev_create_backend("poll");
    TEST_ASSERT(loop != NULL);
    TEST_ASSERT_EQ_STR(ev_loop_backend_name(loop), "poll");
    test_read_basic(loop);
    ev_destroy(loop);
}

void test_event_poll_mod(void) {
    ev_loop_t *loop = ev_create_backend("poll");
    TEST_ASSERT(loop != NULL);
    test_mod_basic(loop);
    ev_destroy(loop);
}

void test_event_poll_timeout(void) {
    ev_loop_t *loop = ev_create_backend("poll");
    TEST_ASSERT(loop != NULL);

    ev_event_t out[2];
    TEST_ASSERT_EQ_INT(ev_wait(loop, out, 2, 50), 0);

    ev_destroy(loop);
}

void test_event_poll_multi(void) {
    ev_loop_t *loop = ev_create_backend("poll");
    TEST_ASSERT(loop != NULL);

    int p1[2], p2[2];
    make_pipe(p1);
    make_pipe(p2);

    int m1 = 0, m2 = 0;
    TEST_ASSERT_EQ_INT(ev_add(loop, p1[0], EV_READ, &m1), 0);
    TEST_ASSERT_EQ_INT(ev_add(loop, p2[0], EV_READ, &m2), 0);

    TEST_ASSERT_EQ_INT(write(p1[1], "a", 1), 1);
    TEST_ASSERT_EQ_INT(write(p2[1], "b", 1), 1);

    ev_event_t out[2];
    int n = ev_wait(loop, out, 1, 1000);
    TEST_ASSERT_EQ_INT(n, 1);
    int first_fd = n == 1 ? out[0].fd : -1;
    if (first_fd >= 0) {
        char c;
        TEST_ASSERT_EQ_INT(read(first_fd, &c, 1), 1);
    }

    n = ev_wait(loop, out, 1, 1000);
    TEST_ASSERT_EQ_INT(n, 1);
    int second_fd = n == 1 ? out[0].fd : -1;
    if (second_fd >= 0) {
        char c;
        TEST_ASSERT_EQ_INT(read(second_fd, &c, 1), 1);
    }
    TEST_ASSERT(first_fd != second_fd);

    TEST_ASSERT_EQ_INT(ev_del(loop, p1[0]), 0);
    TEST_ASSERT_EQ_INT(ev_del(loop, p2[0]), 0);
    close(p1[0]);
    close(p1[1]);
    close(p2[0]);
    close(p2[1]);
    ev_destroy(loop);
}

void test_event_poll_errors(void) {
    ev_loop_t *loop = ev_create_backend("poll");
    TEST_ASSERT(loop != NULL);

    int p[2];
    make_pipe(p);

    errno = 0;
    TEST_ASSERT_EQ_INT(ev_add(loop, -1, EV_READ, NULL), -1);
    TEST_ASSERT_EQ_INT(errno, EINVAL);

    errno = 0;
    TEST_ASSERT_EQ_INT(ev_add(loop, p[0], 0, NULL), -1);
    TEST_ASSERT_EQ_INT(errno, EINVAL);

    TEST_ASSERT_EQ_INT(ev_add(loop, p[0], EV_READ, NULL), 0);

    errno = 0;
    TEST_ASSERT_EQ_INT(ev_add(loop, p[0], EV_READ, NULL), -1);
    TEST_ASSERT_EQ_INT(errno, EEXIST);

    errno = 0;
    TEST_ASSERT_EQ_INT(ev_mod(loop, p[1], EV_READ, NULL), -1);
    TEST_ASSERT_EQ_INT(errno, ENOENT);

    errno = 0;
    TEST_ASSERT_EQ_INT(ev_del(loop, p[1]), -1);
    TEST_ASSERT_EQ_INT(errno, ENOENT);

    ev_event_t out[2];
    errno = 0;
    TEST_ASSERT_EQ_INT(ev_wait(loop, NULL, 2, 0), -1);
    TEST_ASSERT_EQ_INT(errno, EINVAL);

    errno = 0;
    TEST_ASSERT_EQ_INT(ev_wait(loop, out, 0, 0), -1);
    TEST_ASSERT_EQ_INT(errno, EINVAL);

    TEST_ASSERT_EQ_INT(ev_del(loop, p[0]), 0);
    close(p[0]);
    close(p[1]);
    ev_destroy(loop);
}

void test_event_native_read(void) {
    ev_loop_t *loop = ev_create();
    TEST_ASSERT(loop != NULL);

    const char *name = ev_loop_backend_name(loop);
    TEST_ASSERT(name != NULL);
    TEST_ASSERT(strcmp(name, "?") != 0);

    test_read_basic(loop);
    ev_destroy(loop);
}

void test_event_native_mod(void) {
    ev_loop_t *loop = ev_create();
    TEST_ASSERT(loop != NULL);
    test_mod_basic(loop);
    ev_destroy(loop);
}

void test_event_named_backend(void) {
    const char *name = ev_backend_name();
    ev_loop_t *loop = ev_create_backend(name);
    TEST_ASSERT(loop != NULL);
    TEST_ASSERT_EQ_STR(ev_loop_backend_name(loop), name);
    test_read_basic(loop);
    ev_destroy(loop);
}

void test_event_unknown_backend(void) {
    errno = 0;
    TEST_ASSERT(ev_create_backend("nope") == NULL);
    TEST_ASSERT_EQ_INT(errno, ENOTSUP);
}
