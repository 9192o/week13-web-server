#include "http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void assert_slice(struct http_slice actual, const char *expected)
{
    assert(actual.length == strlen(expected));
    if (actual.length)
        assert(memcmp(actual.data, expected, actual.length) == 0);
}

static void test_request_line(void)
{
    const char input[] = "GET /index.html?x=1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    struct http_request_line_result result =
        http_parse_request_line(input, sizeof(input) - 1);

    assert(result.status == HTTP_PARSE_COMPLETE);
    assert(result.error_status == 0);
    assert(result.consumed == strlen("GET /index.html?x=1 HTTP/1.1\r\n"));
    assert_slice(result.request.method, "GET");
    assert_slice(result.request.target, "/index.html?x=1");
    assert(result.request.method.data == input);
    assert(result.request.target.data == input + 4);
    assert(result.request.version_major == 1);
    assert(result.request.version_minor == 1);
}

static void test_fragmented_line(void)
{
    const char input[] = "POST /echo HTTP/1.1\r\n";
    char buffer[sizeof(input) - 1];

    for (size_t split = 0; split < sizeof(buffer); ++split)
    {
        memcpy(buffer, input, split);
        struct http_request_line_result result =
            http_parse_request_line(buffer, split);
        assert(result.status == HTTP_PARSE_NEED_MORE);
        assert(result.consumed == 0);
        assert(result.error_status == 0);
        assert(result.request.method.data == NULL);

        memcpy(buffer + split, input + split, sizeof(buffer) - split);
        result = http_parse_request_line(buffer, sizeof(buffer));
        assert(result.status == HTTP_PARSE_COMPLETE);
        assert(result.consumed == sizeof(buffer));
        assert_slice(result.request.method, "POST");
        assert_slice(result.request.target, "/echo");
    }

    for (size_t length = 1; length <= sizeof(buffer); ++length)
    {
        struct http_request_line_result result =
            http_parse_request_line(input, length);
        assert(result.status == (length == sizeof(buffer)
                                     ? HTTP_PARSE_COMPLETE
                                     : HTTP_PARSE_NEED_MORE));
    }
}

static void test_invalid_lines(void)
{
    const char *cases[] = {
        "\r\n",
        " GET / HTTP/1.1\r\n",
        "GET  / HTTP/1.1\r\n",
        "GET /  HTTP/1.1\r\n",
        "GET / HTTP/1.1 \r\n",
        "GET / HTTP/1.1 extra\r\n",
        "GET / HTTP/1.1\n",
        "GET / HTTP/1.1\rX",
        "GET\t/ HTTP/1.1\r\n",
        "G(ET / HTTP/1.1\r\n",
        "GET / http/1.1\r\n",
        "GET / HTTP/11.1\r\n",
        "GET / HTTP/1.10\r\n",
        "GET / HTTP/x.1\r\n",
        "GET / HTTP/1.x\r\n",
        "GET / HTTP/1\r\n",
        "GET / HTTP/1.1\r\r\n",
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        struct http_request_line_result result =
            http_parse_request_line(cases[i], strlen(cases[i]));
        assert(result.status == HTTP_PARSE_ERROR);
        assert(result.error_status == 400);
        assert(result.consumed == 0);
        assert(result.request.method.data == NULL);
    }

    char input[] = "GET /x HTTP/1.1\r\n";
    for (unsigned int byte = 0; byte <= 127; ++byte)
    {
        if (byte > 32 && byte != 127)
        {
            continue;
        }
        input[5] = (char)byte;
        struct http_request_line_result result =
            http_parse_request_line(input, sizeof(input) - 1);
        assert(result.status == HTTP_PARSE_ERROR);
        assert(result.error_status == 400);
    }
}

static void test_syntax_is_not_routing(void)
{
    const char input[] = "CUSTOM! /raw%2Fpath?q=1 HTTP/2.0\r\n";
    struct http_request_line_result result =
        http_parse_request_line(input, sizeof(input) - 1);

    assert(result.status == HTTP_PARSE_COMPLETE);
    assert_slice(result.request.method, "CUSTOM!");
    assert_slice(result.request.target, "/raw%2Fpath?q=1");
    assert(result.request.version_major == 2);
    assert(result.request.version_minor == 0);

    const char token[] = "!#$%&'*+-.^_`|~012Az / HTTP/1.9\r\n";
    result = http_parse_request_line(token, sizeof(token) - 1);
    assert(result.status == HTTP_PARSE_COMPLETE);
    assert(result.request.version_minor == 9);
}

static void test_line_limit(void)
{
    char input[HTTP_REQUEST_LINE_MAX + 1];
    const char suffix[] = " HTTP/1.1\r\n";

    for (size_t length = HTTP_REQUEST_LINE_MAX - 1;
         length <= HTTP_REQUEST_LINE_MAX + 1; ++length)
    {
        memset(input, 'a', length);
        memcpy(input, "GET /", 5);
        memcpy(input + length - (sizeof(suffix) - 1),
               suffix, sizeof(suffix) - 1);
        struct http_request_line_result result =
            http_parse_request_line(input, length);
        if (length <= HTTP_REQUEST_LINE_MAX)
        {
            assert(result.status == HTTP_PARSE_COMPLETE);
            assert(result.consumed == length);
        }
        else
        {
            assert(result.status == HTTP_PARSE_ERROR);
            assert(result.error_status == 414);
        }
    }

    memset(input, 'a', sizeof(input));
    struct http_request_line_result result =
        http_parse_request_line(input, HTTP_REQUEST_LINE_MAX);
    assert(result.status == HTTP_PARSE_ERROR);
    assert(result.error_status == 414);

    memcpy(input, "GET / HTTP/1.1\r\n", 16);
    result = http_parse_request_line(input, sizeof(input));
    assert(result.status == HTTP_PARSE_COMPLETE);
    assert(result.consumed == 16);
}

static void assert_headers_incomplete(struct http_headers_result result)
{
    assert(result.status == HTTP_PARSE_NEED_MORE);
    assert(result.error_status == 0);
    assert(result.consumed == 0);
    assert(result.headers.count == 0);
    assert(result.headers.fields[0].name.data == NULL);
}

static void assert_headers_error(struct http_headers_result result, int status)
{
    assert(result.status == HTTP_PARSE_ERROR);
    assert(result.error_status == status);
    assert(result.consumed == 0);
    assert(result.headers.count == 0);
    assert(result.headers.fields[0].name.data == NULL);
}

static void assert_response_head_prefix(const char *buffer, const char *prefix)
{
    size_t len = strlen(prefix);
    assert(memcmp(buffer, prefix, len) == 0);
}

static void assert_response_head_contains(const char *buffer, const char *needle)
{
    assert(strstr(buffer, needle) != NULL);
}

static void test_headers(void)
{
    const char input[] =
        "hOsT:\t localhost \t\r\n"
        "X-Data: a: b\tc\r\n"
        "X-Empty:\t \r\n"
        "X-Data: second\r\n"
        "\r\n"
        "body\0\xff";
    char original[sizeof(input)];
    memcpy(original, input, sizeof(input));
    struct http_headers_result result =
        http_parse_headers(input, sizeof(input) - 1, 16);

    assert(result.status == HTTP_PARSE_COMPLETE);
    assert(result.error_status == 0);
    assert(result.consumed == sizeof(input) - 1 - 6);
    assert(result.headers.count == 4);
    assert_slice(result.headers.fields[0].name, "hOsT");
    assert_slice(result.headers.fields[0].value, "localhost");
    assert(result.headers.fields[0].name.data == input);
    assert(result.headers.fields[0].value.data == input + 7);
    assert_slice(result.headers.fields[1].value, "a: b\tc");
    assert_slice(result.headers.fields[2].value, "");
    assert_slice(result.headers.fields[3].name, "X-Data");
    assert_slice(result.headers.fields[3].value, "second");
    assert(memcmp(input, original, sizeof(input)) == 0);

    result = http_parse_headers("\r\nbody", 6, 16);
    assert(result.status == HTTP_PARSE_COMPLETE);
    assert(result.headers.count == 0);
    assert(result.consumed == 2);

    const char opaque[] = "X: \x80\xff\r\n\r\n";
    result = http_parse_headers(opaque, sizeof(opaque) - 1, 16);
    assert(result.status == HTTP_PARSE_COMPLETE);
    assert(result.headers.fields[0].value.length == 2);
    assert(memcmp(result.headers.fields[0].value.data, opaque + 3, 2) == 0);

    const char token[] = "!#$%&'*+-.^_`|~012Az: value\r\n\r\n";
    result = http_parse_headers(token, sizeof(token) - 1, 16);
    assert(result.status == HTTP_PARSE_COMPLETE);
    assert_slice(result.headers.fields[0].name, "!#$%&'*+-.^_`|~012Az");
}

static void test_fragmented_headers(void)
{
    const char input[] = "Host: localhost\r\nX: y\r\n\r\n";
    char buffer[sizeof(input) - 1];
    for (size_t split = 0; split < sizeof(buffer); ++split)
    {
        memcpy(buffer, input, split);
        assert_headers_incomplete(http_parse_headers(buffer, split, 16));
        memcpy(buffer + split, input + split, sizeof(buffer) - split);
        struct http_headers_result result =
            http_parse_headers(buffer, sizeof(buffer), 16);
        assert(result.status == HTTP_PARSE_COMPLETE);
        assert(result.consumed == sizeof(buffer));
        assert(result.headers.count == 2);
        assert_slice(result.headers.fields[1].value, "y");
    }
}

static void test_invalid_headers(void)
{
    const char *cases[] = {
        ": value\r\n\r\n",
        "Host : value\r\n\r\n",
        "Host\t: value\r\n\r\n",
        "Bad(Name: value\r\n\r\n",
        "Host value\r\n\r\n",
        " Host: value\r\n\r\n",
        "Host: value\r\n folded\r\n\r\n",
        "Host: value\r\n\tfolded\r\n\r\n",
        "Host: value\n\n",
        "Host: value\rX",
        "Host: value\r\n\n",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        assert_headers_error(http_parse_headers(cases[i], strlen(cases[i]), 16), 400);

    char value[] = "X: a\r\n\r\n";
    char name[] = "X: a\r\n\r\n";
    for (unsigned int byte = 0; byte <= 255; ++byte)
    {
        if ((byte < 32 && byte != '\t') || byte == 127)
        {
            value[3] = (char)byte;
            assert_headers_error(http_parse_headers(value, sizeof(value) - 1, 16), 400);
        }
        if (byte <= 32 || byte >= 127)
        {
            name[0] = (char)byte;
            assert_headers_error(http_parse_headers(name, sizeof(name) - 1, 16), 400);
        }
    }
}

static void test_header_line_limit(void)
{
    char input[HTTP_HEADER_LINE_MAX + 3];
    for (size_t length = HTTP_HEADER_LINE_MAX - 1;
         length <= HTTP_HEADER_LINE_MAX + 1; ++length)
    {
        memset(input, 'a', sizeof(input));
        input[0] = 'X';
        input[1] = ':';
        memcpy(input + length - 2, "\r\n\r\n", 4);
        struct http_headers_result result =
            http_parse_headers(input, length + 2, 16);
        if (length <= HTTP_HEADER_LINE_MAX)
        {
            assert(result.status == HTTP_PARSE_COMPLETE);
            assert(result.consumed == length + 2);
        }
        else
            assert_headers_error(result, 431);
    }
    memset(input, 'a', sizeof(input));
    input[1] = ':';
    assert_headers_incomplete(http_parse_headers(input, HTTP_HEADER_LINE_MAX - 1, 16));
    assert_headers_error(http_parse_headers(input, HTTP_HEADER_LINE_MAX, 16), 431);
    input[HTTP_HEADER_LINE_MAX - 1] = '\r';
    assert_headers_error(http_parse_headers(input, HTTP_HEADER_LINE_MAX, 16), 431);
}

static void test_header_count_limit(void)
{
    char input[(HTTP_HEADER_COUNT_MAX + 1) * 5 + 2];
    for (size_t count = HTTP_HEADER_COUNT_MAX - 1;
         count <= HTTP_HEADER_COUNT_MAX + 1; ++count)
    {
        for (size_t i = 0; i < count; ++i)
            memcpy(input + i * 5, "X:a\r\n", 5);
        memcpy(input + count * 5, "\r\n", 2);
        struct http_headers_result result =
            http_parse_headers(input, count * 5 + 2, 16);
        if (count <= HTTP_HEADER_COUNT_MAX)
        {
            assert(result.status == HTTP_PARSE_COMPLETE);
            assert(result.headers.count == count);
        }
        else
            assert_headers_error(result, 431);
    }
    input[HTTP_HEADER_COUNT_MAX * 5] = '\r';
    assert_headers_incomplete(http_parse_headers(input, HTTP_HEADER_COUNT_MAX * 5 + 1, 16));
}

static void test_header_total_limit(void)
{
    char input[HTTP_REQUEST_HEAD_MAX + 8];
    const size_t request_line_lengths[] = {16, HTTP_REQUEST_LINE_MAX};
    for (size_t r = 0; r < 2; ++r)
    {
        size_t request_line_length = request_line_lengths[r];
        size_t budget = HTTP_REQUEST_HEAD_MAX - request_line_length;
        for (size_t length = budget - 1; length <= budget + 1; ++length)
        {
            size_t offset = 0;
            memset(input, 'a', sizeof(input));
            while (length - offset > HTTP_HEADER_LINE_MAX + 2)
            {
                input[offset] = 'X';
                input[offset + 1] = ':';
                memcpy(input + offset + HTTP_HEADER_LINE_MAX - 2, "\r\n", 2);
                offset += HTTP_HEADER_LINE_MAX;
            }
            input[offset] = 'X';
            input[offset + 1] = ':';
            memcpy(input + length - 4, "\r\n\r\n", 4);
            struct http_headers_result result =
                http_parse_headers(input, length, request_line_length);
            if (length <= budget)
            {
                assert(result.status == HTTP_PARSE_COMPLETE);
                assert(result.consumed == length);
                if (length == budget)
                {
                    assert_headers_incomplete(
                        http_parse_headers(input, length - 1, request_line_length));
                    memset(input + length, 0, sizeof(input) - length);
                    result = http_parse_headers(input, sizeof(input), request_line_length);
                    assert(result.status == HTTP_PARSE_COMPLETE);
                    assert(result.consumed == length);
                    input[length - 2] = 'a';
                    input[length - 1] = 'a';
                    assert_headers_error(
                        http_parse_headers(input, length, request_line_length), 431);
                }
            }
            else
                assert_headers_error(result, 431);
        }
    }
}

static void test_request_head_semantics(void)
{
    const struct
    {
        const char *wire;
        int status;
        const char *path;
        const char *query;
        const char *authority;
    } cases[] = {
        {"GET /a%3Fb?q=x?y HTTP/1.1\r\nhOsT: example.org:8080\r\n\r\n",
         0, "/a%3Fb", "q=x?y", "example.org:8080"},
        {"GET /%ED%95%9C%20a HTTP/1.9\r\nHost: [::1]:80\r\n\r\n",
         0, "/%ED%95%9C%20a", "", "[::1]:80"},
        {"GET HTTP://example.org/a?b HTTP/1.1\r\nHost: other.org\r\n\r\n",
         0, "/a", "b", "example.org"},
        {"GET https://[::1]?x HTTP/1.1\r\nHost: x\r\n\r\n",
         0, "/", "x", "[::1]"},
        {"GET http://example.org HTTP/1.1\r\nHost:\r\n\r\n",
         0, "/", "", "example.org"},
        {"OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n", 0, "", "", "x"},
        {"CONNECT [::1]:443 HTTP/1.1\r\nHost: x\r\n\r\n", 0, "", "", "[::1]:443"},
        {"GET / HTTP/1.1\r\nHost: [v1.a:b]\r\n\r\n", 0, "/", "", "[v1.a:b]"},
        {"GET / HTTP/1.1\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.1\r\nHost: x\r\nHOST: x\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.1\r\nHost:\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.1\r\nHost: x, y\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.1\r\nHost: user@x\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.1\r\nHost: x:abc\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.1\r\nHost: ::1\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.1\r\nHost: [xyz]\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.1\r\nHost: [::1]junk\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.1\r\nHost: x%zz\r\n\r\n", 400, "", "", ""},
        {"GET /bad% HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET /%00 HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET /?q=%GG HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET /a#b HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET /a\\b HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET /\xff HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET relative HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET * HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"CONNECT / HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"CONNECT x: HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET http:///a HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET http://user@x/a HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET ftp://x/a HTTP/1.1\r\nHost: x\r\n\r\n", 400, "", "", ""},
        {"GET / HTTP/1.0\r\nHost: x\r\n\r\n", 505, "", "", ""},
        {"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505, "", "", ""},
        {"GET / HTTP/0.9\r\nHost: x\r\n\r\n", 505, "", "", ""},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        size_t length = strlen(cases[i].wire);
        char input[256];
        memcpy(input, cases[i].wire, length);
        struct http_request_line_result line = http_parse_request_line(input, length);
        assert(line.status == HTTP_PARSE_COMPLETE);
        struct http_headers_result headers = http_parse_headers(
            input + line.consumed, length - line.consumed, line.consumed);
        assert(headers.status == HTTP_PARSE_COMPLETE);
        struct http_target target;
        memset(&target, 0xff, sizeof(target));
        assert(http_validate_request_head(&line.request, &headers.headers, &target) ==
               cases[i].status);
        assert_slice(target.path, cases[i].path);
        assert_slice(target.query, cases[i].query);
        assert_slice(target.authority, cases[i].authority);
        if (cases[i].status)
            assert(target.path.data == NULL && target.authority.data == NULL);
        if (i == 0)
            assert(target.path.data == input + 4);
        assert(memcmp(input, cases[i].wire, length) == 0);
    }
}

static void test_reason_phrases(void)
{
    assert(strcmp(http_reason_phrase(200), "OK") == 0);
    assert(strcmp(http_reason_phrase(400), "Bad Request") == 0);
    assert(strcmp(http_reason_phrase(403), "Forbidden") == 0);
    assert(strcmp(http_reason_phrase(404), "Not Found") == 0);
    assert(strcmp(http_reason_phrase(405), "Method Not Allowed") == 0);
    assert(strcmp(http_reason_phrase(408), "Request Timeout") == 0);
    assert(strcmp(http_reason_phrase(413), "Content Too Large") == 0);
    assert(strcmp(http_reason_phrase(414), "URI Too Long") == 0);
    assert(strcmp(http_reason_phrase(417), "Expectation Failed") == 0);
    assert(strcmp(http_reason_phrase(431), "Request Header Fields Too Large") == 0);
    assert(strcmp(http_reason_phrase(500), "Internal Server Error") == 0);
    assert(strcmp(http_reason_phrase(501), "Not Implemented") == 0);
    assert(strcmp(http_reason_phrase(505), "HTTP Version Not Supported") == 0);
}

static void test_error_response_format(void)
{
    const char *body = http_reason_phrase(400);
    struct http_response response = {
        .status = 400,
        .content_type = "text/plain; charset=utf-8",
        .body = {body, strlen(body)},
        .file_fd = -1};
    char buffer[512];
    size_t length = http_format_response_head(&response, buffer, sizeof(buffer));
    assert(length > 0);
    assert_response_head_prefix(buffer, "HTTP/1.1 400 Bad Request\r\n");
    assert_response_head_contains(buffer, "Date: ");
    assert_response_head_contains(buffer, "Content-Type: text/plain; charset=utf-8\r\n");
    assert_response_head_contains(buffer, "Content-Length: 11\r\n");
    assert_response_head_contains(buffer, "Connection: close\r\n");
    assert_response_head_contains(buffer, "X-Content-Type-Options: nosniff\r\n");
    char *end = strstr(buffer, "\r\n\r\n");
    assert(end && (size_t)(end + 4 - buffer) == length);

    body = http_reason_phrase(408);
    response.status = 408;
    response.body = (struct http_slice){body, strlen(body)};
    length = http_format_response_head(&response, buffer, sizeof(buffer));
    assert(length > 0);
    assert_response_head_prefix(buffer, "HTTP/1.1 408 Request Timeout\r\n");
    assert_response_head_contains(buffer, "Content-Length: 15\r\n");
}

static void test_method_not_allowed_format(void)
{
    const char *body = http_reason_phrase(405);
    struct http_response response = {
        .status = 405,
        .content_type = "text/plain; charset=utf-8",
        .allow = "GET, HEAD",
        .body = {body, strlen(body)},
        .file_fd = -1};
    char buffer[512];
    size_t length = http_format_response_head(&response, buffer, sizeof(buffer));
    assert(length > 0);
    assert_response_head_prefix(buffer, "HTTP/1.1 405 Method Not Allowed\r\n");
    assert_response_head_contains(buffer, "Allow: GET, HEAD\r\n");
    assert_response_head_contains(buffer, "Content-Length: 18\r\n");
}

static void test_directory_redirect_format(void)
{
    struct http_response response = {
        .status = 308,
        .content_type = "text/plain; charset=utf-8",
        .location = "/assets/",
        .file_fd = -1};
    char buffer[512];
    assert(http_format_response_head(&response, buffer, sizeof(buffer)) > 0);
    assert_response_head_prefix(buffer, "HTTP/1.1 308 Permanent Redirect\r\n");
    assert_response_head_contains(buffer, "Location: /assets/\r\n");
    assert_response_head_contains(buffer, "Content-Length: 0\r\n");
}

static void test_internal_error_format(void)
{
    const char *body = http_reason_phrase(500);
    struct http_response response = {
        .status = 500,
        .content_type = "text/plain; charset=utf-8",
        .body = {body, strlen(body)},
        .file_fd = -1};
    char buffer[512];
    size_t length = http_format_response_head(&response, buffer, sizeof(buffer));
    assert(length > 0);
    assert_response_head_prefix(buffer, "HTTP/1.1 500 Internal Server Error\r\n");
    assert_response_head_contains(buffer, "Content-Type: text/plain; charset=utf-8\r\n");
    assert_response_head_contains(buffer, "Content-Length: 21\r\n");
}

static void test_connection_policy(void)
{
    const char wire[] = "Connection: disclose, Keep-Alive \r\n"
                        "cOnNeCtIoN: , \tClOsE\t , other\r\n\r\n";
    struct http_headers_result parsed = http_parse_headers(wire, sizeof(wire) - 1, 0);
    assert(parsed.status == HTTP_PARSE_COMPLETE);
    assert(http_connection_wants_close(&parsed.headers));
    parsed.headers.count = 1;
    assert(!http_connection_wants_close(&parsed.headers));

    struct http_response response = {
        .status = 200, .content_type = "text/plain", .file_fd = -1, .keep_alive = 1};
    char buffer[512];
    size_t length = http_format_response_head(&response, buffer, sizeof(buffer));
    assert(length > 0 && !strstr(buffer, "\r\nConnection:"));
    assert_response_head_contains(buffer, "Content-Length: 0\r\n");
    assert(strstr(buffer, "\r\n\r\n") == buffer + length - 4);
}

int main(void)
{
    test_request_line();
    test_fragmented_line();
    test_invalid_lines();
    test_syntax_is_not_routing();
    test_line_limit();
    puts("HTTP_REQUEST_LINE_TESTS_PASS");
    test_headers();
    test_fragmented_headers();
    test_invalid_headers();
    test_header_line_limit();
    test_header_count_limit();
    test_header_total_limit();
    puts("HTTP_HEADERS_TESTS_PASS");
    test_request_head_semantics();
    puts("HTTP_HEAD_VALIDATION_TESTS_PASS");
    test_reason_phrases();
    puts("HTTP_REASON_PHRASE_TESTS_PASS");
    test_error_response_format();
    test_method_not_allowed_format();
    test_directory_redirect_format();
    test_internal_error_format();
    test_connection_policy();
    puts("HTTP_RESPONSE_FORMAT_TESTS_PASS");
    return 0;
}
