#include "util.h"
#include "test_harness.h"

#include <string.h>
#include <time.h>

void test_http_date_epoch(void) {
    char buf[64];
    TEST_ASSERT_EQ_INT(http_date_format(0, buf, sizeof buf), 0);
    TEST_ASSERT_EQ_STR(buf, "Thu, 01 Jan 1970 00:00:00 GMT");
}

void test_http_date_roundtrip(void) {
    char buf[64];
    time_t t = (time_t)1700000000;
    time_t out = 0;

    TEST_ASSERT_EQ_INT(http_date_format(t, buf, sizeof buf), 0);
    TEST_ASSERT_EQ_INT(http_date_parse(buf, &out), 0);
    TEST_ASSERT_EQ_INT(out, t);
}

void test_http_date_invalid(void) {
    time_t out = 0;

    TEST_ASSERT_EQ_INT(http_date_parse("not a date", &out), -1);
    TEST_ASSERT_EQ_INT(
        http_date_parse("Wed, 21 Oct 2015 07:28:00 XYZ", &out), -1);
}
