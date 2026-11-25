#include "proxy_internal.h"
#include "test_harness.h"
#include "util.h"

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int make_pair(int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(sv[i], F_GETFL, 0);
        if (flags < 0 || fcntl(sv[i], F_SETFL, flags | O_NONBLOCK) < 0)
            return -1;
    }
    return 0;
}

static int fd_is_closed(int fd) {
    return fcntl(fd, F_GETFD) < 0;
}

void test_proxy_pool_reuse(void) {
    proxy_backend_t b;
    memset(&b, 0, sizeof b);
    proxy_pool_init(2, 1000);

    int sv[2];
    TEST_ASSERT_EQ_INT(make_pair(sv), 0);
    proxy_pool_release(&b, sv[0]);
    TEST_ASSERT_EQ_INT(b.idle_count, 1);

    int fd = proxy_pool_acquire(&b);
    TEST_ASSERT_EQ_INT(fd, sv[0]);
    TEST_ASSERT_EQ_INT(b.idle_count, 0);
    TEST_ASSERT(proxy_pool_acquire(&b) == -1);

    close(fd);
    close(sv[1]);
    proxy_pool_close(&b);
}

void test_proxy_pool_max_idle(void) {
    proxy_backend_t b;
    memset(&b, 0, sizeof b);
    proxy_pool_init(1, 1000);

    int sv1[2], sv2[2];
    TEST_ASSERT_EQ_INT(make_pair(sv1), 0);
    TEST_ASSERT_EQ_INT(make_pair(sv2), 0);
    proxy_pool_release(&b, sv1[0]);
    proxy_pool_release(&b, sv2[0]);
    TEST_ASSERT_EQ_INT(b.idle_count, 1);
    TEST_ASSERT(fd_is_closed(sv2[0]));

    proxy_pool_close(&b);
    close(sv1[1]);
    close(sv2[1]);
}

void test_proxy_pool_stale(void) {
    proxy_backend_t b;
    memset(&b, 0, sizeof b);
    proxy_pool_init(2, 1000);

    int sv[2];
    TEST_ASSERT_EQ_INT(make_pair(sv), 0);
    close(sv[1]);
    proxy_pool_release(&b, sv[0]);
    TEST_ASSERT_EQ_INT(b.idle_count, 1);

    TEST_ASSERT(proxy_pool_acquire(&b) == -1);
    TEST_ASSERT_EQ_INT(b.idle_count, 0);
    TEST_ASSERT(fd_is_closed(sv[0]));
}

void test_proxy_pool_timeout(void) {
    proxy_backend_t b;
    memset(&b, 0, sizeof b);
    proxy_pool_init(2, 1000);

    int sv[2];
    TEST_ASSERT_EQ_INT(make_pair(sv), 0);
    proxy_pool_release(&b, sv[0]);
    TEST_ASSERT(b.idle_head != NULL);
    b.idle_head->idle_since_ms = now_ms() - 5000;
    proxy_pool_sweep(&b, now_ms());
    TEST_ASSERT_EQ_INT(b.idle_count, 0);
    TEST_ASSERT(fd_is_closed(sv[0]));
    close(sv[1]);
}

void test_proxy_pool_next_timeout(void) {
    proxy_backend_t b;
    memset(&b, 0, sizeof b);
    proxy_pool_init(2, 1000);

    TEST_ASSERT(proxy_pool_next_timeout_ms(&b) == -1);

    int sv[2];
    TEST_ASSERT_EQ_INT(make_pair(sv), 0);
    proxy_pool_release(&b, sv[0]);
    TEST_ASSERT(proxy_pool_next_timeout_ms(&b) >= 0);

    proxy_pool_close(&b);
    close(sv[1]);
}

void test_proxy_pool_zero_idle(void) {
    proxy_backend_t b;
    memset(&b, 0, sizeof b);
    proxy_pool_init(0, 1000);

    int sv[2];
    TEST_ASSERT_EQ_INT(make_pair(sv), 0);
    proxy_pool_release(&b, sv[0]);
    TEST_ASSERT_EQ_INT(b.idle_count, 0);
    TEST_ASSERT(fd_is_closed(sv[0]));
    close(sv[1]);
}
