#include "chunked.h"
#include "test_harness.h"

#include <string.h>

static void feed_text(chunk_parser_t *p, const char *s) {
    size_t len = strlen(s);
    size_t used = 0;
    while (used < len) {
        size_t n = chunk_parser_feed(p, s + used, len - used);
        if (n == 0)
            break;
        used += n;
    }
}

void test_chunked_basic(void) {
    chunk_parser_t p;
    chunk_parser_init(&p);
    feed_text(&p, "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    TEST_ASSERT(chunk_parser_done(&p));
    TEST_ASSERT(!chunk_parser_error(&p));
}

void test_chunked_partial(void) {
    chunk_parser_t p;
    chunk_parser_init(&p);
    const char *s = "5\r\nhello\r\n0\r\n\r\n";
    for (size_t i = 0; s[i] != '\0'; i++) {
        size_t n = chunk_parser_feed(&p, s + i, 1);
        TEST_ASSERT_EQ_INT((int)n, 1);
    }
    TEST_ASSERT(chunk_parser_done(&p));
}

void test_chunked_extension(void) {
    chunk_parser_t p;
    chunk_parser_init(&p);
    feed_text(&p, "1;foo=bar\r\nx\r\n0\r\n\r\n");
    TEST_ASSERT(chunk_parser_done(&p));
}

void test_chunked_trailer(void) {
    chunk_parser_t p;
    chunk_parser_init(&p);
    feed_text(&p, "0\r\nX-Test: yes\r\n\r\n");
    TEST_ASSERT(chunk_parser_done(&p));
}

void test_chunked_bad_size(void) {
    chunk_parser_t p;
    chunk_parser_init(&p);
    feed_text(&p, "Z\r\n");
    TEST_ASSERT(chunk_parser_error(&p));
}

void test_chunked_bad_crlf(void) {
    chunk_parser_t p;
    chunk_parser_init(&p);
    feed_text(&p, "1\r\nxXX");
    TEST_ASSERT(chunk_parser_error(&p));
}
