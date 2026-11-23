#include "test_harness.h"

void test_parse_get(void);
void test_missing_host(void);
void test_http10_no_host(void);
void test_bad_method(void);
void test_bad_version(void);
void test_header_case_insensitive(void);
void test_query_split(void);
void test_percent_decode(void);
void test_bad_request_line(void);
void test_traversal_rejected(void);
void test_post_without_length(void);
void test_duplicate_content_length_same(void);
void test_duplicate_content_length_diff(void);
void test_cl_te_conflict(void);
void test_chunked_rejected(void);
void test_header_ows_trim(void);
void test_connection_close_keepalive(void);
void test_get_header(void);

void test_http_date_epoch(void);
void test_http_date_roundtrip(void);
void test_http_date_invalid(void);

void test_response_send_memory(void);
void test_response_error_404(void);
void test_response_reason(void);
void test_response_add_header_overflow(void);

void test_send_file_exact_size(void);
void test_send_file_short_eof(void);

int g_tests_run = 0;
int g_tests_failed = 0;

int main(void) {
    RUN_TEST(test_parse_get);
    RUN_TEST(test_missing_host);
    RUN_TEST(test_http10_no_host);
    RUN_TEST(test_bad_method);
    RUN_TEST(test_bad_version);
    RUN_TEST(test_header_case_insensitive);
    RUN_TEST(test_query_split);
    RUN_TEST(test_percent_decode);
    RUN_TEST(test_bad_request_line);
    RUN_TEST(test_traversal_rejected);
    RUN_TEST(test_post_without_length);
    RUN_TEST(test_duplicate_content_length_same);
    RUN_TEST(test_duplicate_content_length_diff);
    RUN_TEST(test_cl_te_conflict);
    RUN_TEST(test_chunked_rejected);
    RUN_TEST(test_header_ows_trim);
    RUN_TEST(test_connection_close_keepalive);
    RUN_TEST(test_get_header);

    RUN_TEST(test_http_date_epoch);
    RUN_TEST(test_http_date_roundtrip);
    RUN_TEST(test_http_date_invalid);

    RUN_TEST(test_response_send_memory);
    RUN_TEST(test_response_error_404);
    RUN_TEST(test_response_reason);
    RUN_TEST(test_response_add_header_overflow);

    RUN_TEST(test_send_file_exact_size);
    RUN_TEST(test_send_file_short_eof);

    printf("%d assertions, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
