#include "http.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int init(struct http_body_parser *parser, const char *fields)
{
    struct http_headers_result headers = http_parse_headers(fields, strlen(fields), 16);
    assert(headers.status == HTTP_PARSE_COMPLETE);
    return http_body_init(parser, &headers.headers);
}

static void test_length_body(void)
{
    struct http_body_parser parser;
    assert(init(&parser, "cOnTeNt-LeNgTh: 5\r\n\r\n") == 0);
    const char input[] = "a\0b\xffzextra";
    struct http_body_result result = http_parse_body(&parser, input, 2);
    assert(result.status == HTTP_PARSE_NEED_MORE);
    assert(result.consumed == 2);
    result = http_parse_body(&parser, input + 2, sizeof(input) - 3);
    assert(result.status == HTTP_PARSE_COMPLETE);
    assert(result.consumed == 3);
    assert(parser.length == 5);
    assert(memcmp(parser.data, input, 5) == 0);
    http_body_destroy(&parser);

    assert(init(&parser, "\r\n") == 0);
    result = http_parse_body(&parser, "not a body", 10);
    assert(result.status == HTTP_PARSE_COMPLETE);
    assert(result.consumed == 0);
    assert(parser.length == 0);
    http_body_destroy(&parser);
}

static void test_framing_errors(void)
{
    const struct
    {
        const char *fields;
        int status;
    } cases[] = {
        {"Content-Length:\r\n\r\n", 400},
        {"Content-Length: -1\r\n\r\n", 400},
        {"Content-Length: +1\r\n\r\n", 400},
        {"Content-Length: 1x\r\n\r\n", 400},
        {"Content-Length: 1, 1\r\n\r\n", 400},
        {"Content-Length: 1\r\ncontent-length: 1\r\n\r\n", 400},
        {"Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
        {"Content-Length: 184467440737095516160\r\n\r\n", 400},
        {"Content-Length: 1048577\r\n\r\n", 413},
        {"Transfer-Encoding:\r\n\r\n", 400},
        {"Transfer-Encoding: chunked, chunked\r\n\r\n", 400},
        {"Transfer-Encoding: chunked, gzip\r\n\r\n", 400},
        {"Transfer-Encoding: gzip\r\n\r\n", 400},
        {"Transfer-Encoding: gzip, chunked\r\n\r\n", 501},
        {"Transfer-Encoding: gzip;level=\"a,b\", chunked\r\n\r\n", 501},
        {"Transfer-Encoding: gzip;level=, chunked\r\n\r\n", 400},
        {"Transfer-Encoding: chunked;x=1\r\n\r\n", 400},
        {"Transfer-Encoding: chunked,\r\n\r\n", 400},
        {"Expect: unknown\r\n\r\n", 417},
        {"Expect: other=\"x,y\";a=b\r\n\r\n", 417},
        {"Expect: bad value\r\n\r\n", 400},
        {"Expect: 100-continue=bad\r\n\r\n", 417},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        struct http_body_parser parser;
        assert(init(&parser, cases[i].fields) == cases[i].status);
        http_body_destroy(&parser);
    }
    struct http_body_parser parser;
    assert(init(&parser, "Content-Length: 1\r\nExpect: 100-Continue\r\n"
                         "Expect: 100-continue\r\n\r\n") == 0);
    assert(parser.expect_continue);
    http_body_destroy(&parser);
    assert(init(&parser, "Transfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n") == 501);
    http_body_destroy(&parser);
}

static void test_chunked_splits(void)
{
    const char wire[] = "3; x=\"a\\\"b\"\r\na\0b\r\n2\r\n\xffz\r\n0\r\nX-Note: yes\r\n\r\n";
    const char decoded[] = "a\0b\xffz";
    for (size_t split = 0; split < sizeof(wire) - 1; ++split)
    {
        struct http_body_parser parser;
        assert(init(&parser, "Transfer-Encoding: ChUnKeD\r\n\r\n") == 0);
        struct http_body_result result = http_parse_body(&parser, wire, split);
        assert(result.status == HTTP_PARSE_NEED_MORE);
        assert(result.consumed == split);
        result = http_parse_body(&parser, wire + split, sizeof(wire) - 1 - split);
        assert(result.status == HTTP_PARSE_COMPLETE);
        assert(result.consumed == sizeof(wire) - 1 - split);
        assert(parser.length == sizeof(decoded) - 1);
        assert(memcmp(parser.data, decoded, sizeof(decoded) - 1) == 0);
        http_body_destroy(&parser);
    }
    struct http_body_parser parser;
    assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
    for (size_t i = 0; i < sizeof(wire) - 1; ++i)
    {
        struct http_body_result result = http_parse_body(&parser, wire + i, 1);
        assert(result.status == (i == sizeof(wire) - 2
                                     ? HTTP_PARSE_COMPLETE : HTTP_PARSE_NEED_MORE));
        assert(result.consumed == 1);
    }
    http_body_destroy(&parser);
    assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
    struct http_body_result result = http_parse_body(&parser, "0\r\n\r\nnext", 9);
    assert(result.status == HTTP_PARSE_COMPLETE);
    assert(result.consumed == 5);
    http_body_destroy(&parser);
}

static void test_invalid_chunks(void)
{
    const struct
    {
        const char *wire;
        int status;
    } cases[] = {
        {"g\r\n", 400}, {"\r\n", 400}, {"+1\r\n", 400}, {"1\n", 400},
        {"1\rX", 400}, {"1\r\naX", 400}, {"1\r\na\rX", 400},
        {"1;x=\r\n", 400}, {"1;x=\"bad\r\n", 400},
        {"1 ;x=y \r\n", 400},
        {"100001\r\n", 413},
        {"100000000000000000000\r\n", 400},
        {"0\r\nContent-Length: 0\r\n\r\n", 400},
        {"0\r\nHost: changed\r\n\r\n", 400},
        {"0\r\nAuthorization: secret\r\n\r\n", 400},
        {"0\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
        {"0\r\nConnection: close\r\n\r\n", 400},
        {"0\r\nTrailer: X\r\n\r\n", 400},
        {"0\r\nTE: trailers\r\n\r\n", 400},
        {"0\r\nUpgrade: x\r\n\r\n", 400},
        {"0\r\nExpect: 100-continue\r\n\r\n", 400},
        {"0\r\nContent-Encoding: gzip\r\n\r\n", 400},
        {"0\r\nContent-Type: text/plain\r\n\r\n", 400},
        {"0\r\nProxy-Authorization: secret\r\n\r\n", 400},
        {"0\r\ncOoKiE: secret\r\n\r\n", 400},
        {"0\r\n Bad: fold\r\n\r\n", 400},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        struct http_body_parser parser;
        assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
        struct http_body_result result =
            http_parse_body(&parser, cases[i].wire, strlen(cases[i].wire));
        assert(result.status == HTTP_PARSE_ERROR);
        assert(result.error_status == cases[i].status);
        http_body_destroy(&parser);
    }
}

static void test_body_limit(void)
{
    char block[4096];
    memset(block, 'a', sizeof(block));
    struct http_body_parser parser;
    assert(init(&parser, "Content-Length: 1048576\r\n\r\n") == 0);
    assert(parser.capacity == 0);
    for (size_t n = 0; n < HTTP_BODY_MAX; n += sizeof(block))
    {
        struct http_body_result result = http_parse_body(&parser, block, sizeof(block));
        assert(result.status == (n + sizeof(block) == HTTP_BODY_MAX
                                     ? HTTP_PARSE_COMPLETE : HTTP_PARSE_NEED_MORE));
    }
    assert(parser.length == HTTP_BODY_MAX);
    assert(parser.data[0] == 'a' && parser.data[HTTP_BODY_MAX - 1] == 'a');
    http_body_destroy(&parser);

    assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
    assert(http_parse_body(&parser, "100000\r\n", 8).status == HTTP_PARSE_NEED_MORE);
    for (size_t n = 0; n < HTTP_BODY_MAX; n += sizeof(block))
        assert(http_parse_body(&parser, block, sizeof(block)).status == HTTP_PARSE_NEED_MORE);
    struct http_body_result result = http_parse_body(&parser, "\r\n1\r\n", 5);
    assert(result.status == HTTP_PARSE_ERROR);
    assert(result.error_status == 413);
    http_body_destroy(&parser);
}

static void test_body_boundary_successes(void)
{
    const size_t lengths[] = {0, 1, 1048575, 1048576};
    char block[4096];
    for (size_t i = 0; i < sizeof(block); ++i)
        block[i] = (char)(i % 256);
    for (size_t chunked = 0; chunked <= 1; ++chunked)
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
        {
            struct http_body_parser parser;
            char fields[64];
            if (chunked)
                memcpy(fields, "Transfer-Encoding: chunked\r\n\r\n", 31);
            else
                snprintf(fields, sizeof(fields), "Content-Length: %zu\r\n\r\n", lengths[i]);
            assert(init(&parser, fields) == 0);
            struct http_body_result result = http_parse_body(&parser, "", 0);
            if (chunked && lengths[i])
            {
                char size[32];
                int n = snprintf(size, sizeof(size), "%zx\r\n", lengths[i]);
                result = http_parse_body(&parser, size, (size_t)n);
                assert(result.status == HTTP_PARSE_NEED_MORE);
            }
            for (size_t offset = 0; offset < lengths[i];)
            {
                size_t take = lengths[i] - offset;
                if (take > sizeof(block))
                    take = sizeof(block);
                result = http_parse_body(&parser, block, take);
                offset += take;
            }
            if (chunked)
                result = http_parse_body(&parser, lengths[i] ? "\r\n0\r\n\r\n" : "0\r\n\r\n",
                                         lengths[i] ? 7 : 5);
            assert(result.status == HTTP_PARSE_COMPLETE);
            assert(parser.length == lengths[i]);
            for (size_t n = 0; n < parser.length; ++n)
                assert((unsigned char)parser.data[n] == n % 256);
            http_body_destroy(&parser);
        }
}

static void test_metadata_limits(void)
{
    char line[8193];
    memset(line, 'a', sizeof(line));
    memcpy(line, "1;x=", 4);
    struct http_body_parser parser;
    assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
    memcpy(line + 8190, "\r\n", 2);
    assert(http_parse_body(&parser, line, 8192).status == HTTP_PARSE_NEED_MORE);
    assert(http_parse_body(&parser, "a\r\n0\r\n\r\n", 8).status == HTTP_PARSE_COMPLETE);
    http_body_destroy(&parser);
    assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
    memset(line + 8190, 'a', 3);
    struct http_body_result result = http_parse_body(&parser, line, 8192);
    assert(result.status == HTTP_PARSE_ERROR && result.error_status == 431);
    http_body_destroy(&parser);

    assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
    assert(http_parse_body(&parser, "0\r\n", 3).status == HTTP_PARSE_NEED_MORE);
    for (size_t i = 0; i < 100; ++i)
        assert(http_parse_body(&parser, "X:a\r\n", 5).status == HTTP_PARSE_NEED_MORE);
    result = http_parse_body(&parser, "X:a\r\n", 5);
    assert(result.status == HTTP_PARSE_ERROR && result.error_status == 431);
    http_body_destroy(&parser);
}

static void test_extension_total_limit(void)
{
    char line[8192];
    memset(line, 'a', sizeof(line));
    memcpy(line, "1;x=", 4);
    memcpy(line + 8190, "\r\n", 2);
    const char *last[] = {"0;x=aaaaaaaaa\r\n\r\n", "0;x=aaaaaaaaaa\r\n\r\n"};
    for (size_t i = 0; i < 2; ++i)
    {
        struct http_body_parser parser;
        assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
        for (size_t n = 0; n < 4; ++n)
        {
            assert(http_parse_body(&parser, line, sizeof(line)).status == HTTP_PARSE_NEED_MORE);
            assert(http_parse_body(&parser, "a\r\n", 3).status == HTTP_PARSE_NEED_MORE);
        }
        struct http_body_result result = http_parse_body(&parser, last[i], strlen(last[i]));
        if (i == 0)
        {
            assert(result.status == HTTP_PARSE_COMPLETE);
            assert(parser.length == 4);
        }
        else
            assert(result.status == HTTP_PARSE_ERROR && result.error_status == 431);
        http_body_destroy(&parser);
    }
}

static void test_trailer_total_limit(void)
{
    char line[8192];
    memset(line, 'a', sizeof(line));
    memcpy(line, "X:", 2);
    memcpy(line + 8190, "\r\n", 2);
    for (size_t total = 32767; total <= 32769; ++total)
    {
        struct http_body_parser parser;
        assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
        assert(http_parse_body(&parser, "0\r\n", 3).status == HTTP_PARSE_NEED_MORE);
        for (size_t n = 0; n < 3; ++n)
            assert(http_parse_body(&parser, line, sizeof(line)).status == HTTP_PARSE_NEED_MORE);
        size_t last_length = total - 3 * sizeof(line) - 2;
        char last[8192];
        memset(last, 'a', sizeof(last));
        memcpy(last, "X:", 2);
        memcpy(last + last_length - 2, "\r\n", 2);
        assert(http_parse_body(&parser, last, last_length).status == HTTP_PARSE_NEED_MORE);
        struct http_body_result result = http_parse_body(&parser, "\r\n", 2);
        if (total <= 32768)
            assert(result.status == HTTP_PARSE_COMPLETE);
        else
            assert(result.status == HTTP_PARSE_ERROR && result.error_status == 431);
        http_body_destroy(&parser);
    }
    for (size_t count = 99; count <= 100; ++count)
    {
        struct http_body_parser parser;
        assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
        assert(http_parse_body(&parser, "0\r\n", 3).status == HTTP_PARSE_NEED_MORE);
        for (size_t i = 0; i < count; ++i)
            assert(http_parse_body(&parser, "X:a\r\n", 5).status == HTTP_PARSE_NEED_MORE);
        assert(http_parse_body(&parser, "\r\n", 2).status == HTTP_PARSE_COMPLETE);
        http_body_destroy(&parser);
    }
}

static void test_many_small_chunks(void)
{
    struct http_body_parser parser;
    assert(init(&parser, "Transfer-Encoding: chunked\r\n\r\n") == 0);
    for (size_t i = 0; i < 40000; ++i)
        assert(http_parse_body(&parser, "1\r\na\r\n", 6).status == HTTP_PARSE_NEED_MORE);
    assert(http_parse_body(&parser, "0\r\n\r\n", 5).status == HTTP_PARSE_COMPLETE);
    assert(parser.length == 40000);
    for (size_t i = 0; i < parser.length; ++i)
        assert(parser.data[i] == 'a');
    http_body_destroy(&parser);
}

int main(void)
{
    test_length_body();
    test_framing_errors();
    test_chunked_splits();
    test_invalid_chunks();
    test_body_limit();
    test_body_boundary_successes();
    test_metadata_limits();
    test_extension_total_limit();
    test_trailer_total_limit();
    test_many_small_chunks();
    puts("HTTP_BODY_TESTS_PASS");
    return 0;
}
