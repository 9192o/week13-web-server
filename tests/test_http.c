#include "http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void assert_slice(struct http_slice actual, const char *expected)
{
    assert(actual.length == strlen(expected));
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

int main(void)
{
    test_request_line();
    test_fragmented_line();
    test_invalid_lines();
    test_syntax_is_not_routing();
    test_line_limit();
    puts("HTTP_REQUEST_LINE_TESTS_PASS");
    return 0;
}
