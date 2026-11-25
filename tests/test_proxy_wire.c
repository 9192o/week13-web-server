#include "proxy_wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct http_request make_request(const char *method, const char *path,
                                        const char *query)
{
    struct http_request req = {0};
    req.line.method = (struct http_slice){method, strlen(method)};
    req.line.target = (struct http_slice){path, strlen(path)};
    req.line.version_major = 1;
    req.line.version_minor = 1;
    req.target.form = HTTP_TARGET_ORIGIN;
    req.target.path = (struct http_slice){path, strlen(path)};
    req.target.query =
        (struct http_slice){query, query ? strlen(query) : 0};
    return req;
}

static void add_header(struct http_request *req, const char *name,
                       const char *value)
{
    assert(req->headers.count < HTTP_HEADER_COUNT_MAX);
    req->headers.fields[req->headers.count++] =
        (struct http_header){{name, strlen(name)}, {value, strlen(value)}};
}

static void assert_contains(const char *haystack, const char *needle)
{
    assert(strstr(haystack, needle) != NULL);
}

static void assert_not_contains(const char *haystack, const char *needle)
{
    assert(strstr(haystack, needle) == NULL);
}

static void test_request_simple_get(void)
{
    struct http_request req = make_request("GET", "/api/users", NULL);
    add_header(&req, "Host", "example.com");
    add_header(&req, "Accept", "application/json");
    add_header(&req, "Connection", "keep-alive");

    char out[512];
    size_t n = proxy_request_head(&req, "backend.test:8080", out, sizeof(out));
    assert(n > 0 && n < sizeof(out));
    out[n] = '\0';

    assert(strncmp(out, "GET /api/users HTTP/1.1\r\n",
                   strlen("GET /api/users HTTP/1.1\r\n")) == 0);
    assert_contains(out, "Host: backend.test:8080\r\n");
    assert_contains(out, "Accept: application/json\r\n");
    assert_contains(out, "Content-Length: 0\r\n");
    assert_contains(out, "Via: 1.1 week13\r\n");
    assert_not_contains(out, "Connection:");
    assert_not_contains(out, "Connection: keep-alive");
    assert_not_contains(out, "keep-alive");
}

static void test_request_post_with_body(void)
{
    struct http_request req = make_request("POST", "/api/echo", NULL);
    add_header(&req, "Host", "example.com");
    add_header(&req, "Content-Type", "text/plain");
    add_header(&req, "Content-Length", "5");
    add_header(&req, "Expect", "100-continue");
    req.body = (struct http_slice){"hello", 5};

    char out[512];
    size_t n = proxy_request_head(&req, "backend.test", out, sizeof(out));
    assert(n > 0 && n < sizeof(out));
    out[n] = '\0';

    assert(strncmp(out, "POST /api/echo HTTP/1.1\r\n",
                   strlen("POST /api/echo HTTP/1.1\r\n")) == 0);
    assert_contains(out, "Host: backend.test\r\n");
    assert_contains(out, "Content-Type: text/plain\r\n");
    assert_contains(out, "Content-Length: 5\r\n");
    assert(strstr(strstr(out, "Content-Length:") + 1, "Content-Length:") == NULL);
    assert_not_contains(out, "Expect:");
}

static void test_request_chunked_becomes_length(void)
{
    struct http_request req = make_request("POST", "/api/upload", NULL);
    add_header(&req, "Host", "example.com");
    add_header(&req, "Transfer-Encoding", "chunked");
    req.body = (struct http_slice){"abc", 3};

    char out[512];
    size_t n = proxy_request_head(&req, "backend.test", out, sizeof(out));
    assert(n > 0 && n < sizeof(out));
    out[n] = '\0';

    assert_contains(out, "Content-Length: 3\r\n");
    assert_not_contains(out, "Transfer-Encoding");
    assert_not_contains(out, "chunked");
}

static void test_request_origin_form_preserves_query(void)
{
    struct http_request req = make_request("GET", "/api/search", "q=test&n=3");
    add_header(&req, "Host", "example.com");

    char out[512];
    size_t n = proxy_request_head(&req, "backend.test", out, sizeof(out));
    assert(n > 0 && n < sizeof(out));
    out[n] = '\0';

    assert(strncmp(out, "GET /api/search?q=test&n=3 HTTP/1.1\r\n",
                   strlen("GET /api/search?q=test&n=3 HTTP/1.1\r\n")) == 0);
}

static void test_request_connection_nominates_fields(void)
{
    struct http_request req = make_request("GET", "/api/x", NULL);
    add_header(&req, "Host", "example.com");
    add_header(&req, "Connection", "keep-alive, X-Custom");
    add_header(&req, "X-Custom", "remove-me");
    add_header(&req, "X-Other", "keep-me");

    char out[512];
    size_t n = proxy_request_head(&req, "backend.test", out, sizeof(out));
    assert(n > 0 && n < sizeof(out));
    out[n] = '\0';

    assert_not_contains(out, "X-Custom");
    assert_contains(out, "X-Other: keep-me\r\n");
}

static void test_request_hop_by_hop_filtered(void)
{
    struct http_request req = make_request("GET", "/api/x", NULL);
    add_header(&req, "Host", "example.com");
    add_header(&req, "Transfer-Encoding", "chunked");
    add_header(&req, "Upgrade", "h2c");
    add_header(&req, "Keep-Alive", "timeout=5");
    add_header(&req, "TE", "deflate");
    add_header(&req, "Trailer", "X-Trailer");
    add_header(&req, "Proxy-Connection", "keep-alive");
    add_header(&req, "X-Keep", "yes");

    char out[512];
    size_t n = proxy_request_head(&req, "backend.test", out, sizeof(out));
    assert(n > 0 && n < sizeof(out));
    out[n] = '\0';

    assert_contains(out, "X-Keep: yes\r\n");
    assert_not_contains(out, "Transfer-Encoding");
    assert_not_contains(out, "Upgrade");
    assert_not_contains(out, "Keep-Alive");
    assert_not_contains(out, "TE");
    assert_not_contains(out, "Trailer");
    assert_not_contains(out, "Proxy-Connection");
}

static void test_request_via_appended(void)
{
    struct http_request req = make_request("GET", "/api/x", NULL);
    add_header(&req, "Host", "example.com");
    add_header(&req, "Via", "1.0 fred");

    char out[512];
    size_t n = proxy_request_head(&req, "backend.test", out, sizeof(out));
    assert(n > 0 && n < sizeof(out));
    out[n] = '\0';

    assert_contains(out, "Via: 1.0 fred\r\n");
    assert_contains(out, "Via: 1.1 week13\r\n");
}

static void test_response_length(void)
{
    const char data[] =
        "HTTP/1.1 200 OK\r\n"
        "Server: test\r\n"
        "Content-Length: 12\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, true);
    assert(rc == 1);
    assert(head.status == 200);
    assert(head.frame == PROXY_LENGTH);
    assert(head.length == 12);
    assert(head.consumed == sizeof(data) - 1);
    out[outlen] = '\0';

    assert(strncmp(out, "HTTP/1.1 200 OK\r\n", 17) == 0);
    assert_contains(out, "Server: test\r\n");
    assert_contains(out, "Content-Length: 12\r\n");
    assert(head.upstream_keep);
    assert_not_contains(out, "Connection:");
}

static void test_response_chunked(void)
{
    const char data[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "X-Data: value\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, true);
    assert(rc == 1);
    assert(head.frame == PROXY_CHUNKED);
    assert(!head.upstream_keep);
    assert(head.length == 0);
    out[outlen] = '\0';

    assert_contains(out, "Transfer-Encoding: chunked\r\n");
    assert_contains(out, "Connection: close\r\n");
    assert_contains(out, "X-Data: value\r\n");
    assert_not_contains(out, "Content-Length");
}

static void test_response_eof_framing(void)
{
    const char data[] =
        "HTTP/1.1 200 OK\r\n"
        "X-Data: value\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == 1);
    assert(head.frame == PROXY_EOF);
    out[outlen] = '\0';

    assert_contains(out, "Transfer-Encoding: chunked\r\n");
    assert_not_contains(out, "Content-Length");
}

static void test_response_head_preserves_cl(void)
{
    const char data[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 100\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, true, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == 1);
    assert(head.status == 200);
    assert(head.frame == PROXY_LENGTH);
    assert(head.length == 100);
    out[outlen] = '\0';

    assert_contains(out, "Content-Length: 100\r\n");
}

static void test_response_head_no_cl(void)
{
    const char data[] =
        "HTTP/1.1 200 OK\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, true, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == 1);
    assert(head.frame == PROXY_LENGTH);
    assert(head.length == 0);
    out[outlen] = '\0';

    assert_not_contains(out, "Content-Length");
}

static void test_response_no_body_status(void)
{
    const char data[] =
        "HTTP/1.1 204 No Content\r\n"
        "Content-Length: 5\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == 1);
    assert(head.status == 204);
    assert(head.frame == PROXY_NONE);
    assert(head.length == 0);
    out[outlen] = '\0';

    assert_not_contains(out, "Content-Length");
    assert_not_contains(out, "Transfer-Encoding");
}

static void test_response_informational(void)
{
    const char data[] = "HTTP/1.1 100 Continue\r\n\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == 1);
    assert(head.status == 100);
    assert(head.frame == PROXY_NONE);
    assert(head.consumed == sizeof(data) - 1);
    out[outlen] = '\0';

    assert(strncmp(out, "HTTP/1.1 100 Continue\r\n",
                   strlen("HTTP/1.1 100 Continue\r\n")) == 0);
}

static void test_response_connection_nominated(void)
{
    const char data[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: X-Custom\r\n"
        "X-Custom: secret\r\n"
        "X-Keep: yes\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == 1);
    out[outlen] = '\0';

    assert_not_contains(out, "X-Custom");
    assert_contains(out, "X-Keep: yes\r\n");
}

static void test_response_via_preserved(void)
{
    const char data[] =
        "HTTP/1.1 200 OK\r\n"
        "Via: 1.0 fred\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == 1);
    out[outlen] = '\0';

    assert_contains(out, "Via: 1.0 fred\r\n");
}

static void test_response_rejects_conflicting_framing(void)
{
    const char data[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == -1);
}

static void test_response_invalid_content_length(void)
{
    const char data[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: abc\r\n"
        "\r\n";

    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == -1);
}

static void test_response_invalid_status_line(void)
{
    const char data[] = "HTTP/1.1 20 OK\r\n\r\n";
    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == -1);
}

static void test_response_need_more(void)
{
    const char data[] = "HTTP/1.1 200 OK\r\n";
    char out[512];
    struct proxy_head head;
    size_t outlen;
    int rc = proxy_response_head(data, sizeof(data) - 1, false, &head, out,
                                 sizeof(out), &outlen, false);
    assert(rc == 0);
    assert(head.consumed == 0);
    assert(head.status == 0);
}

int main(void)
{
    test_request_simple_get();
    test_request_post_with_body();
    test_request_chunked_becomes_length();
    test_request_origin_form_preserves_query();
    test_request_connection_nominates_fields();
    test_request_hop_by_hop_filtered();
    test_request_via_appended();

    test_response_length();
    test_response_chunked();
    test_response_eof_framing();
    test_response_head_preserves_cl();
    test_response_head_no_cl();
    test_response_no_body_status();
    test_response_informational();
    test_response_connection_nominated();
    test_response_via_preserved();
    test_response_rejects_conflicting_framing();
    test_response_invalid_content_length();
    test_response_invalid_status_line();
    test_response_need_more();

    puts("PROXY_WIRE_TESTS_PASS");
    return 0;
}
