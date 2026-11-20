#include "http.h"
#include "app.h"

#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stddef.h>

#include "webserver.c"

#define MAX_RECV_CALLS 16

static struct
{
    const char *recv_data[MAX_RECV_CALLS];
    size_t recv_len[MAX_RECV_CALLS];
    int recv_errno[MAX_RECV_CALLS];
    size_t recv_calls;
    size_t recv_next;
    size_t recv_offset;

    char send_buf[16384];
    size_t send_len;
    size_t send_calls;
    int send_next_errno;
    size_t send_next_partial;
    int send_last_flags;
} mock;

static struct
{
    int called;
    char data[HTTP_REQUEST_LINE_MAX];
    size_t length;
} app_stub;

static void reset(void)
{
    memset(&mock, 0, sizeof(mock));
    memset(&app_stub, 0, sizeof(app_stub));
}

ssize_t __wrap_recv(int sockfd, void *buf, size_t len, int flags)
{
    (void)sockfd;
    (void)flags;
    assert(mock.recv_next < mock.recv_calls);
    assert(len > 0);
    assert(app_stub.called == 0);
    size_t i = mock.recv_next;
    if (mock.recv_errno[i] != 0)
    {
        mock.recv_next++;
        errno = mock.recv_errno[i];
        return -1;
    }
    size_t n = mock.recv_len[i] - mock.recv_offset;
    if (n > len)
        n = len;
    memcpy(buf, mock.recv_data[i] + mock.recv_offset, n);
    mock.recv_offset += n;
    if (mock.recv_offset == mock.recv_len[i])
    {
        mock.recv_next++;
        mock.recv_offset = 0;
    }
    return (ssize_t)n;
}

ssize_t __wrap_send(int sockfd, const void *buf, size_t len, int flags)
{
    (void)sockfd;
    assert(flags == MSG_NOSIGNAL);
    mock.send_calls++;
    mock.send_last_flags = flags;
    if (mock.send_next_errno != 0)
    {
        int e = mock.send_next_errno;
        mock.send_next_errno = 0;
        errno = e;
        return -1;
    }
    size_t n = len;
    if (mock.send_next_partial != 0 && mock.send_next_partial < n)
    {
        n = mock.send_next_partial;
        mock.send_next_partial = 0;
    }
    if (mock.send_len + n > sizeof(mock.send_buf))
        n = sizeof(mock.send_buf) - mock.send_len;
    memcpy(mock.send_buf + mock.send_len, buf, n);
    mock.send_len += n;
    return (ssize_t)n;
}

const char *app_handle_data(const char *data, size_t data_length)
{
    app_stub.called++;
    assert(data_length <= sizeof(app_stub.data));
    memcpy(app_stub.data, data, data_length);
    app_stub.length = data_length;
    return HTTP_OK_RESPONSE;
}

static void assert_response_prefix(const char *prefix)
{
    size_t len = strlen(prefix);
    assert(mock.send_len >= len);
    assert(memcmp(mock.send_buf, prefix, len) == 0);
}

static void assert_response_is(const char *response)
{
    size_t len = strlen(response);
    assert(mock.send_len == len);
    assert(memcmp(mock.send_buf, response, len) == 0);
}

static void test_normal_valid(void)
{
    reset();
    const char req[] = "GET /foo HTTP/1.1\r\nHost: ignored\r\n";
    mock.recv_data[0] = req;
    mock.recv_len[0] = sizeof(req) - 1;
    mock.recv_calls = 1;
    handle_client(0);
    assert(app_stub.called == 1);
    assert(app_stub.length == strlen("GET /foo HTTP/1.1\r\n"));
    assert(memcmp(app_stub.data, "GET /foo HTTP/1.1\r\n", app_stub.length) == 0);
    assert_response_is(HTTP_OK_RESPONSE);
    assert(mock.send_last_flags == MSG_NOSIGNAL);
}

static void test_fragmented_line(void)
{
    reset();
    const char part1[] = "POST /echo HTTP/1.1\r";
    const char part2[] = "\n";
    mock.recv_data[0] = part1;
    mock.recv_len[0] = sizeof(part1) - 1;
    mock.recv_data[1] = part2;
    mock.recv_len[1] = sizeof(part2) - 1;
    mock.recv_calls = 2;
    handle_client(0);
    assert(app_stub.called == 1);
    assert(app_stub.length == strlen("POST /echo HTTP/1.1\r\n"));
    assert_response_is(HTTP_OK_RESPONSE);
}

static void test_long_line_over_1024(void)
{
    reset();
    char line[2000];
    memset(line, 'A', sizeof(line));
    memcpy(line, "GET /", 5);
    memcpy(line + sizeof(line) - strlen(" HTTP/1.1\r\n"),
           " HTTP/1.1\r\n", strlen(" HTTP/1.1\r\n"));
    mock.recv_data[0] = line;
    mock.recv_len[0] = 1024;
    mock.recv_data[1] = line + 1024;
    mock.recv_len[1] = sizeof(line) - 1024;
    mock.recv_calls = 2;
    handle_client(0);
    assert(app_stub.called == 1);
    assert(app_stub.length == sizeof(line));
    assert(memcmp(app_stub.data, line, sizeof(line)) == 0);
    assert_response_is(HTTP_OK_RESPONSE);
}

static void test_exact_limit_8192(void)
{
    reset();
    char line[8192];
    memset(line, 'A', sizeof(line));
    memcpy(line, "GET /", 5);
    memcpy(line + sizeof(line) - strlen(" HTTP/1.1\r\n"),
           " HTTP/1.1\r\n", strlen(" HTTP/1.1\r\n"));
    for (int i = 0; i < 8; i++)
    {
        mock.recv_data[i] = line + i * 1024;
        mock.recv_len[i] = 1024;
    }
    mock.recv_calls = 8;
    handle_client(0);
    assert(app_stub.called == 1);
    assert(app_stub.length == sizeof(line));
    assert_response_is(HTTP_OK_RESPONSE);
}

static void test_malformed_400(void)
{
    reset();
    const char req[] = "GET / HTTP/1.1 extra\r\n";
    mock.recv_data[0] = req;
    mock.recv_len[0] = sizeof(req) - 1;
    mock.recv_calls = 1;
    handle_client(0);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_oversize_414(void)
{
    reset();
    char line[8192];
    memset(line, 'A', sizeof(line));
    memcpy(line, "GET /", 5);
    for (int i = 0; i < 8; i++)
    {
        mock.recv_data[i] = line + i * 1024;
        mock.recv_len[i] = 1024;
    }
    mock.recv_calls = 8;
    handle_client(0);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 414");
}

static void test_eof_empty_400(void)
{
    reset();
    mock.recv_data[0] = "";
    mock.recv_len[0] = 0;
    mock.recv_calls = 1;
    handle_client(0);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_recv_eintr_retries(void)
{
    reset();
    const char req[] = "GET / HTTP/1.1\r\n";
    mock.recv_errno[0] = EINTR;
    mock.recv_data[1] = req;
    mock.recv_len[1] = sizeof(req) - 1;
    mock.recv_calls = 2;
    handle_client(0);
    assert(app_stub.called == 1);
    assert_response_is(HTTP_OK_RESPONSE);
}

static void test_recv_error_cleanup(void)
{
    reset();
    mock.recv_errno[0] = EIO;
    mock.recv_calls = 1;
    handle_client(0);
    assert(app_stub.called == 0);
    assert(mock.send_len == 0);
}

static void test_send_partial_and_eintr(void)
{
    reset();
    const char req[] = "GET / HTTP/1.1\r\n";
    mock.recv_data[0] = req;
    mock.recv_len[0] = sizeof(req) - 1;
    mock.recv_calls = 1;
    mock.send_next_partial = 5;
    mock.send_next_errno = EINTR;
    handle_client(0);
    assert(app_stub.called == 1);
    assert_response_is(HTTP_OK_RESPONSE);
    assert(mock.send_calls > 1);
}

int main(void)
{
    test_normal_valid();
    test_fragmented_line();
    test_long_line_over_1024();
    test_exact_limit_8192();
    test_malformed_400();
    test_oversize_414();
    test_eof_empty_400();
    test_recv_eintr_retries();
    test_recv_error_cleanup();
    test_send_partial_and_eintr();
    puts("WEBSERVER_REQUEST_LINE_TESTS_PASS");
    return 0;
}
