#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "app.h"

#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <fcntl.h>

#include "client_session.c"

static void handle_client(int unused_fd, int root)
{
    (void)unused_fd;
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    struct client_session *session = client_session_create(fd, root, -1);
    assert(session);
    bool alive = true;
    for (size_t steps = 0; steps < 128 && alive &&
         session->state != CS_IDLE && !session->request_count; ++steps)
        alive = client_session_advance(session, true, true);
    while (alive && (session->state == CS_WRITING || session->state == CS_READ_BODY))
        alive = client_session_advance(session, true, true);
    assert(!alive || session->state == CS_IDLE || session->state == CS_READ_HEAD);
    client_session_destroy(session);
}

#define MAX_RECV_CALLS 64
static int index_fd = -1;
static const struct http_response *response_override;

static struct
{
    const char *recv_data[MAX_RECV_CALLS];
    size_t recv_len[MAX_RECV_CALLS];
    int recv_errno[MAX_RECV_CALLS];
    size_t recv_calls;
    size_t recv_next;
    size_t recv_offset;

    char send_buf[65536];
    size_t send_len;
    size_t send_calls;
    int send_next_errno;
    size_t send_next_partial;
    int send_last_flags;
} mock;

static struct
{
    int called;
    char method[32];
    char target[HTTP_REQUEST_LINE_MAX];
    unsigned int version_major;
    unsigned int version_minor;
    size_t header_count;
    char path[HTTP_REQUEST_LINE_MAX];
    char query[HTTP_REQUEST_LINE_MAX];
    char authority[HTTP_REQUEST_LINE_MAX];
    char body[HTTP_BODY_MAX];
    size_t body_length;
} app_stub;

static void reset(void)
{
    memset(&mock, 0, sizeof(mock));
    memset(&app_stub, 0, sizeof(app_stub));
    index_fd = -1;
    response_override = NULL;
}

static void feedv(const char *data[], const size_t len[], size_t n)
{
    assert(n <= MAX_RECV_CALLS);
    for (size_t i = 0; i < n; ++i)
    {
        mock.recv_data[i] = data[i];
        mock.recv_len[i] = len[i];
    }
    mock.recv_calls = n;
    mock.recv_next = 0;
    mock.recv_offset = 0;
}

static void feed(const char *s)
{
    const char *data[] = {s};
    size_t len[] = {strlen(s)};
    feedv(data, len, 1);
}

static void feed2(const char *a, const char *b)
{
    const char *data[] = {a, b};
    size_t len[] = {strlen(a), strlen(b)};
    feedv(data, len, 2);
}

ssize_t __wrap_recv(int sockfd, void *buf, size_t len, int flags)
{
    (void)sockfd;
    (void)flags;
    assert(mock.recv_next < mock.recv_calls);
    assert(len > 0);
    assert(app_stub.called == 0 || index_fd >= 0);
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

/* This harness isolates transport; test_router and real sockets use the router. */
static struct http_response stub_response(const struct http_request *request)
{
    (void)request;
    if (response_override)
        return *response_override;
    return (struct http_response){
        .status = 200,
        .content_type = "application/octet-stream",
        .file_fd = index_fd};
}

struct router_result app_match_request(const struct http_request *request)
{
    (void)request;
    return (struct router_result){.handler = index_fd >= 0 ? NULL : stub_response,
                                  .status = 200};
}

struct http_response app_handle_request(int root_fd,
                                        const struct http_request *request,
                                        const struct router_result *route,
                                        char location[HTTP_REQUEST_LINE_MAX + 2])
{
    (void)root_fd;
    (void)location;
    app_stub.called++;
    assert(request->line.method.length < sizeof(app_stub.method));
    memcpy(app_stub.method, request->line.method.data, request->line.method.length);
    app_stub.method[request->line.method.length] = '\0';
    assert(request->line.target.length < sizeof(app_stub.target));
    memcpy(app_stub.target, request->line.target.data, request->line.target.length);
    app_stub.target[request->line.target.length] = '\0';
    app_stub.version_major = request->line.version_major;
    app_stub.version_minor = request->line.version_minor;
    app_stub.header_count = request->headers.count;
    const struct http_slice components[] = {
        request->target.path, request->target.query, request->target.authority};
    char *copies[] = {app_stub.path, app_stub.query, app_stub.authority};
    for (size_t i = 0; i < 3; ++i)
    {
        assert(components[i].length < HTTP_REQUEST_LINE_MAX);
        if (components[i].length)
            memcpy(copies[i], components[i].data, components[i].length);
        copies[i][components[i].length] = '\0';
    }
    assert(request->body.length <= sizeof(app_stub.body));
    if (request->body.length)
        memcpy(app_stub.body, request->body.data, request->body.length);
    app_stub.body_length = request->body.length;
    return route->handler ? route->handler(request) : stub_response(request);
}

static void assert_response_prefix(const char *prefix)
{
    size_t len = strlen(prefix);
    assert(mock.send_len >= len);
    assert(memcmp(mock.send_buf, prefix, len) == 0);
}

static void assert_response_contains(const char *needle);

static void assert_ok_response(void)
{
    assert_response_prefix("HTTP/1.1 200 OK\r\n");
    assert_response_contains("Content-Length: 0\r\n");
    assert_response_contains("Date: ");
    assert_response_contains("X-Content-Type-Options: nosniff\r\n");
    char *end = strstr(mock.send_buf, "\r\n\r\n");
    assert(end && mock.send_len == (size_t)(end + 4 - mock.send_buf));
}

static void assert_error_response(int status, const char *reason, int head)
{
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d", status);
    assert_response_prefix(prefix);
    assert_response_contains("Date: ");
    assert_response_contains("Content-Type: text/plain; charset=utf-8\r\n");
    assert_response_contains("Connection: close\r\n");
    assert_response_contains("X-Content-Type-Options: nosniff\r\n");
    size_t body_len = strlen(reason);
    char cl[64];
    snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", body_len);
    assert_response_contains(cl);
    char *end = strstr(mock.send_buf, "\r\n\r\n");
    assert(end);
    size_t header_len = (size_t)(end + 4 - mock.send_buf);
    assert(mock.send_len == header_len + (head ? 0 : body_len));
    if (!head)
        assert(memcmp(mock.send_buf + header_len, reason, body_len) == 0);
}

static void assert_response_contains(const char *needle)
{
    size_t len = strlen(needle);
    assert(mock.send_len >= len);
    int found = 0;
    for (size_t i = 0; i + len <= mock.send_len; ++i)
    {
        if (memcmp(mock.send_buf + i, needle, len) == 0)
        {
            found = 1;
            break;
        }
    }
    assert(found);
}

static void assert_app_called_with(const char *method, const char *target,
                                   unsigned int major, unsigned int minor,
                                   size_t headers, const void *body, size_t body_len)
{
    assert(app_stub.called == 1);
    assert(strcmp(app_stub.method, method) == 0);
    assert(strcmp(app_stub.target, target) == 0);
    assert(app_stub.version_major == major);
    assert(app_stub.version_minor == minor);
    assert(app_stub.header_count == headers);
    assert(app_stub.body_length == body_len);
    assert(memcmp(app_stub.body, body, body_len) == 0);
}

static void test_header_only_valid(void)
{
    reset();
    feed("GET /foo HTTP/1.1\r\nHost: ignored\r\n\r\n");
    handle_client(0, -1);
    assert_app_called_with("GET", "/foo", 1, 1, 1, "", 0);
    assert_ok_response();
    assert(mock.send_last_flags == MSG_NOSIGNAL);
}

static void test_fragmented_line_and_headers(void)
{
    reset();
    const char part1[] = "POST /echo HTTP/1.1\r";
    const char part2[] = "\nHost: x\r\n\r\n";
    const char *data[] = {part1, part2};
    size_t len[] = {sizeof(part1) - 1, sizeof(part2) - 1};
    feedv(data, len, 2);
    handle_client(0, -1);
    assert_app_called_with("POST", "/echo", 1, 1, 1, "", 0);
    assert_ok_response();
}

static void test_long_line_over_1024(void)
{
    reset();
    char line[2000];
    memset(line, 'A', sizeof(line));
    memcpy(line, "GET /", 5);
    memcpy(line + sizeof(line) - strlen(" HTTP/1.1\r\n"),
           " HTTP/1.1\r\n", strlen(" HTTP/1.1\r\n"));
    const char *data[] = {line, line + 1024, "Host: x\r\n\r\n"};
    size_t len[] = {1024, sizeof(line) - 1024, sizeof("Host: x\r\n\r\n") - 1};
    feedv(data, len, 3);
    handle_client(0, -1);
    assert(app_stub.called == 1);
    assert(app_stub.body_length == 0);
    assert_ok_response();
}

static void test_exact_limit_8192(void)
{
    reset();
    char line[8192];
    memset(line, 'A', sizeof(line));
    memcpy(line, "GET /", 5);
    memcpy(line + sizeof(line) - strlen(" HTTP/1.1\r\n"),
           " HTTP/1.1\r\n", strlen(" HTTP/1.1\r\n"));
    const char *data[9];
    size_t len[9];
    for (int i = 0; i < 8; ++i)
    {
        data[i] = line + i * 1024;
        len[i] = 1024;
    }
    data[8] = "Host: x\r\n\r\n";
    len[8] = sizeof("Host: x\r\n\r\n") - 1;
    feedv(data, len, 9);
    handle_client(0, -1);
    assert(app_stub.called == 1);
    assert_ok_response();
}

static void test_malformed_400(void)
{
    reset();
    feed("GET / HTTP/1.1 extra\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_oversize_414(void)
{
    reset();
    char line[8192];
    memset(line, 'A', sizeof(line));
    memcpy(line, "GET /", 5);
    const char *data[9];
    size_t len[9];
    for (int i = 0; i < 8; ++i)
    {
        data[i] = line + i * 1024;
        len[i] = 1024;
    }
    data[8] = " HTTP/1.1\r\n";
    len[8] = strlen(data[8]);
    feedv(data, len, 9);
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 414");
}

static void test_eof_empty_400(void)
{
    reset();
    feed("");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_recv_eintr_retries(void)
{
    reset();
    feed("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    mock.recv_errno[0] = EINTR;
    mock.recv_data[1] = mock.recv_data[0];
    mock.recv_len[1] = mock.recv_len[0];
    mock.recv_data[0] = "";
    mock.recv_len[0] = 0;
    mock.recv_calls = 2;
    handle_client(0, -1);
    assert(app_stub.called == 1);
    assert_ok_response();
}

static void test_recv_error_cleanup(void)
{
    reset();
    mock.recv_errno[0] = EIO;
    mock.recv_data[0] = "";
    mock.recv_len[0] = 0;
    mock.recv_calls = 1;
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert(mock.send_len == 0);
}

static void test_send_partial_and_eintr(void)
{
    reset();
    feed("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    mock.send_next_partial = 5;
    mock.send_next_errno = EINTR;
    handle_client(0, -1);
    assert(app_stub.called == 1);
    assert_ok_response();
    assert(mock.send_calls > 1);
}

static void test_content_length_body(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    handle_client(0, -1);
    assert_app_called_with("POST", "/x", 1, 1, 2, "hello", 5);
    assert_ok_response();
}

static void test_content_length_fragmented(void)
{
    reset();
    const char part1[] = "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 12\r\n\r\nhel";
    const char part2[] = "lo\0world\xff";
    const char *data[] = {part1, part2};
    size_t len[] = {sizeof(part1) - 1, sizeof(part2) - 1};
    feedv(data, len, 2);
    handle_client(0, -1);
    assert(app_stub.called == 1);
    assert(app_stub.body_length == 12);
    assert(memcmp(app_stub.body, "hello\0world\xff", 12) == 0);
    assert_ok_response();
}

static void test_content_length_early_eof(void)
{
    reset();
    feed2("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nab", "");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_no_content_length_no_body(void)
{
    reset();
    feed("GET /x HTTP/1.1\r\nHost: x\r\n\r\nsurplus-not-body");
    handle_client(0, -1);
    assert_app_called_with("GET", "/x", 1, 1, 1, "", 0);
    assert_ok_response();
}

static void test_duplicate_content_length(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_content_length_and_transfer_encoding(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_invalid_content_length(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_content_length_overflow(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 184467440737095516160\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_content_length_overlimit(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 413");
}

static void test_transfer_encoding_gzip(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 501");
}

static void test_transfer_encoding_chunked(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
         "5\r\nhello\r\n0\r\nX-Note: yes\r\n\r\n");
    handle_client(0, -1);
    assert_app_called_with("POST", "/x", 1, 1, 2, "hello", 5);
    assert_ok_response();
}

static void test_chunked_fragmented(void)
{
    reset();
    feed2("POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhe",
          "llo\r\n0\r\n\r\n");
    handle_client(0, -1);
    assert_app_called_with("POST", "/x", 1, 1, 2, "hello", 5);
    assert_ok_response();
}

static void test_chunked_early_eof(void)
{
    reset();
    feed2("POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhe", "");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_chunked_size_overlimit(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n100001\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 413");
}

static void test_expect_100_continue_length(void)
{
    reset();
    feed2("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n",
          "hello");
    handle_client(0, -1);
    assert_app_called_with("POST", "/x", 1, 1, 3, "hello", 5);
    assert_response_contains("HTTP/1.1 100 Continue");
    assert_response_contains("HTTP/1.1 200 OK");
}

static void test_expect_100_continue_chunked(void)
{
    reset();
    feed2("POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n",
          "5\r\nhello\r\n0\r\n\r\n");
    handle_client(0, -1);
    assert_app_called_with("POST", "/x", 1, 1, 3, "hello", 5);
    assert_response_contains("HTTP/1.1 100 Continue");
    assert_response_contains("HTTP/1.1 200 OK");
}

static void test_expect_unknown(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nExpect: unknown\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 417");
}

static void test_expect_malformed(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nExpect: bad value\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_header_syntax_error(void)
{
    reset();
    feed("GET /x HTTP/1.1\r\nBad(Name: x\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_header_too_large(void)
{
    reset();
    char big[HTTP_HEADER_LINE_MAX + 8];
    memset(big, 'a', sizeof(big));
    memcpy(big, "X:", 2);
    memcpy(big + HTTP_HEADER_LINE_MAX - 1, "\r\n\r\n", 4);
    const char req[] = "GET /x HTTP/1.1\r\n";
    const char *data[] = {req, big};
    size_t len[] = {sizeof(req) - 1, HTTP_HEADER_LINE_MAX + 3};
    feedv(data, len, 2);
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 431");
}

static void test_eof_before_headers(void)
{
    reset();
    feed2("GET /x HTTP/1.1\r\nHost: x", "");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_no_premature_app_callback(void)
{
    reset();
    feed2("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n", "");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_response_prefix("HTTP/1.1 400");
}

static void test_later_bytes_not_body(void)
{
    reset();
    feed("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabcleftover");
    handle_client(0, -1);
    assert_app_called_with("POST", "/x", 1, 1, 2, "abc", 3);
    assert_ok_response();
}

static void test_custom_method_still_200(void)
{
    reset();
    feed("CUSTOM /x HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_client(0, -1);
    assert_app_called_with("CUSTOM", "/x", 1, 1, 1, "", 0);
    assert_ok_response();
}

static void test_multiple_recv_before_app(void)
{
    reset();
    const char *data[] = {
        "POST /x HTTP/1.1\r\n",
        "Host: x\r\nContent-Length: 5\r\n",
        "\r\n",
        "hel",
        "lo",
    };
    size_t len[] = {
        sizeof("POST /x HTTP/1.1\r\n") - 1,
        sizeof("Host: x\r\nContent-Length: 5\r\n") - 1,
        2,
        3,
        2,
    };
    feedv(data, len, 5);
    handle_client(0, -1);
    assert_app_called_with("POST", "/x", 1, 1, 2, "hello", 5);
    assert_ok_response();
}

static void test_all_request_splits(void)
{
    const char wire[] =
        "POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\na\0b\r\n0\r\nX-T: ignored\r\n\r\n";
    const char body[] = "a\0b";
    for (size_t split = 1; split < sizeof(wire) - 1; ++split)
    {
        reset();
        const char *data[] = {wire, wire + split};
        size_t len[] = {split, sizeof(wire) - 1 - split};
        feedv(data, len, 2);
        handle_client(0, -1);
        assert_app_called_with("POST", "/x", 1, 1, 2, body, sizeof(body) - 1);
        assert_ok_response();
    }
}

static void test_expect_shortcuts_and_errors(void)
{
    const char *requests[] = {
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nExpect: 100-continue\r\n\r\na",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\nExpect: 100-continue\r\n\r\n"};
    for (size_t i = 0; i < 3; ++i)
    {
        reset();
        feed(requests[i]);
        handle_client(0, -1);
        assert(app_stub.called == (i == 2 ? 0 : 1));
        if (i == 2)
            assert_error_response(413, "Content Too Large", 0);
        else
            assert_ok_response();
    }
}

static void test_allocated_body_io_errors(void)
{
    reset();
    feed2("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nab", "");
    mock.recv_errno[1] = EIO;
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert(mock.send_len == 0);

    reset();
    feed("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nab");
    mock.send_next_errno = EPIPE;
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert(mock.send_len == 0);

    reset();
    feed("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\na");
    mock.send_next_errno = EPIPE;
    handle_client(0, -1);
    assert(app_stub.called == 1);
    assert(mock.send_len == 0);
}

static void test_invalid_head_before_body(void)
{
    const char *requests[] = {
        "POST / HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nhOsT: x\r\nContent-Length: 5\r\n\r\n",
        "POST /bad% HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n",
        "POST / HTTP/1.0\r\nHost: x\r\nContent-Length: 5\r\n\r\n",
        "POST / HTTP/2.0\r\nHost: x\r\nExpect: 100-continue\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i)
    {
        reset();
        feed(requests[i]);
        handle_client(0, -1);
        assert(app_stub.called == 0);
        assert(mock.recv_next == 1);
        assert_response_prefix(i < 3 ? "HTTP/1.1 400" : "HTTP/1.1 505");
    }
}

static void test_absolute_target_all_splits(void)
{
    const char wire[] =
        "GET http://example.org/a%3Fb?q=x?y HTTP/1.1\r\nHost: other\r\n\r\n";
    for (size_t split = 1; split < sizeof(wire) - 1; ++split)
    {
        reset();
        const char *data[] = {wire, wire + split};
        size_t lengths[] = {split, sizeof(wire) - 1 - split};
        feedv(data, lengths, 2);
        handle_client(0, -1);
        assert(app_stub.called == 1);
        assert(strcmp(app_stub.path, "/a%3Fb") == 0);
        assert(strcmp(app_stub.query, "q=x?y") == 0);
        assert(strcmp(app_stub.authority, "example.org") == 0);
        assert_ok_response();
    }
}

static void send_fixture_response(const struct http_response *response, int head)
{
    struct http_response fixture = *response;
    if (fixture.file_fd >= 0)
    {
        fixture.file_fd = dup(fixture.file_fd);
        assert(fixture.file_fd >= 0);
    }
    response_override = &fixture;
    feed(head ? "HEAD / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
              : "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    handle_client(0, -1);
    response_override = NULL;
}

static void test_binary_response_and_head(void)
{
    const char bytes[] = "a\0\xff\r\nHTTP/1.1 500 fake\r\n";
    struct http_response response = {
        .status = 200, .content_type = "application/octet-stream",
        .body = {bytes, sizeof(bytes) - 1}, .file_fd = -1};
    reset();
    mock.send_next_partial = 3;
    send_fixture_response(&response, 0);
    char *end = strstr(mock.send_buf, "\r\n\r\n");
    assert(end);
    size_t header_length = (size_t)(end + 4 - mock.send_buf);
    assert(mock.send_len == header_length + sizeof(bytes) - 1);
    assert(memcmp(mock.send_buf + header_length, bytes, sizeof(bytes) - 1) == 0);
    assert_response_contains("Content-Length: 24\r\n");
    assert_response_contains("X-Content-Type-Options: nosniff\r\n");
    reset();
    send_fixture_response(&response, 1);
    end = strstr(mock.send_buf, "\r\n\r\n");
    assert(end && mock.send_len == (size_t)(end + 4 - mock.send_buf));
    assert_response_contains("Content-Length: 24\r\n");
}

static void test_file_response_streaming(void)
{
    FILE *file = tmpfile();
    assert(file);
    char bytes[3079];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (char)i;
    assert(fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
    assert(fflush(file) == 0);
    rewind(file);
    struct http_response response = {
        .status = 200, .content_type = "text/html; charset=utf-8",
        .body.length = sizeof(bytes), .file_fd = fileno(file)};
    reset();
    send_fixture_response(&response, 0);
    char *end = strstr(mock.send_buf, "\r\n\r\n");
    assert(end);
    size_t header_length = (size_t)(end + 4 - mock.send_buf);
    assert(mock.send_len == header_length + sizeof(bytes));
    assert(memcmp(mock.send_buf + header_length, bytes, sizeof(bytes)) == 0);
    assert_response_contains("Content-Length: 3079\r\n");

    rewind(file);
    response.body.length = sizeof(bytes) + 1;
    reset();
    send_fixture_response(&response, 0);
    assert(mock.send_len == header_length + sizeof(bytes));
    assert_response_prefix("HTTP/1.1 200 OK\r\n");
    assert(fclose(file) == 0);
}

static void test_method_not_allowed_response(void)
{
    const char message[] = "Method Not Allowed";
    struct http_response response = {
        .status = 405, .content_type = "text/plain; charset=utf-8",
        .allow = "POST", .body = {message, sizeof(message) - 1}, .file_fd = -1};
    for (int head = 0; head <= 1; ++head)
    {
        reset();
        send_fixture_response(&response, head);
        assert_response_prefix("HTTP/1.1 405 Method Not Allowed\r\n");
        assert_response_contains("Allow: POST\r\n");
        assert_response_contains("Content-Length: 18\r\n");
        char *end = strstr(mock.send_buf, "\r\n\r\n");
        assert(end);
        size_t header_length = (size_t)(end + 4 - mock.send_buf);
        assert(mock.send_len == header_length + (head ? 0 : sizeof(message) - 1));
        if (!head)
            assert(memcmp(mock.send_buf + header_length, message, sizeof(message) - 1) == 0);
    }
}

static void test_prepared_file_cleanup(void)
{
    FILE *file = tmpfile();
    assert(file);
    const char *requests[] = {
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nab",
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nab",
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n",
        "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n",
    };
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i)
    {
        reset();
        index_fd = dup(fileno(file));
        assert(index_fd >= 0);
        feed2(requests[i], "");
        if (i == 2)
            mock.recv_errno[1] = EIO;
        if (i == 3 || i == 4)
            mock.send_next_errno = EPIPE;
        handle_client(0, -1);
        char fd_path[64], destination;
        assert(snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", index_fd) > 0);
        assert(readlink(fd_path, &destination, 1) == -1 && errno == ENOENT);
        if (i == 1 || i == 5)
            assert_error_response(400, "Bad Request", 0);
        if (i == 2 || i == 3 || i == 4)
            assert(mock.send_len == 0);
    }
    assert(fclose(file) == 0);
}

static void test_head_ok_no_body(void)
{
    reset();
    feed("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 1);
    assert_ok_response();
    char *end = strstr(mock.send_buf, "\r\n\r\n");
    assert(end && mock.send_len == (size_t)(end + 4 - mock.send_buf));
}

static void test_head_error_suppresses_body(void)
{
    const char *requests[] = {
        "HEAD / HTTP/1.1\r\nHost: x\r\nBad(Name: x\r\n\r\n",
        "HEAD / HTTP/1.1\r\nHost: x\r\nHost: x\r\n\r\n",
        "HEAD / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i)
    {
        reset();
        feed(requests[i]);
        handle_client(0, -1);
        assert(app_stub.called == 0);
        assert_error_response(i == 2 ? 413 : 400,
                              i == 2 ? "Content Too Large" : "Bad Request", 1);
    }
}

static void test_get_error_includes_body(void)
{
    reset();
    feed("GET / HTTP/1.1\r\nHost: x\r\nBad(Name: x\r\n\r\n");
    handle_client(0, -1);
    assert(app_stub.called == 0);
    assert_error_response(400, "Bad Request", 0);
}

int main(void)
{
    test_binary_response_and_head();
    test_file_response_streaming();
    test_method_not_allowed_response();
    test_prepared_file_cleanup();
    test_invalid_head_before_body();
    test_absolute_target_all_splits();
    test_header_only_valid();
    test_fragmented_line_and_headers();
    test_long_line_over_1024();
    test_exact_limit_8192();
    test_malformed_400();
    test_oversize_414();
    test_eof_empty_400();
    test_recv_eintr_retries();
    test_recv_error_cleanup();
    test_send_partial_and_eintr();
    test_content_length_body();
    test_content_length_fragmented();
    test_content_length_early_eof();
    test_no_content_length_no_body();
    test_duplicate_content_length();
    test_content_length_and_transfer_encoding();
    test_invalid_content_length();
    test_content_length_overflow();
    test_content_length_overlimit();
    test_transfer_encoding_gzip();
    test_transfer_encoding_chunked();
    test_chunked_fragmented();
    test_chunked_early_eof();
    test_chunked_size_overlimit();
    test_expect_100_continue_length();
    test_expect_100_continue_chunked();
    test_expect_unknown();
    test_expect_malformed();
    test_header_syntax_error();
    test_header_too_large();
    test_eof_before_headers();
    test_no_premature_app_callback();
    test_later_bytes_not_body();
    test_custom_method_still_200();
    test_multiple_recv_before_app();
    test_all_request_splits();
    test_expect_shortcuts_and_errors();
    test_allocated_body_io_errors();
    test_head_ok_no_body();
    test_head_error_suppresses_body();
    test_get_error_includes_body();
    puts("WEBSERVER_BODY_FRAMING_TESTS_PASS");
    puts("WEBSERVER_ERROR_RESPONSE_TESTS_PASS");
    return 0;
}
