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
void test_duplicate_host_rejected(void);
void test_host_space_before_colon_rejected(void);
void test_header_name_token_rejected(void);
void test_header_value_ctl_rejected(void);
void test_content_length_plus_rejected(void);
void test_method_token_ctl_rejected(void);
void test_target_ctl_rejected(void);
void test_percent_encoded_trailing_slash(void);
void test_percent_encoded_ctl_rejected(void);
void test_parse_incomplete_headers(void);
void test_parse_split_points(void);

void test_http_date_epoch(void);
void test_http_date_roundtrip(void);
void test_http_date_invalid(void);

void test_response_send_memory(void);
void test_response_error_404(void);
void test_response_reason(void);
void test_response_add_header_overflow(void);
void test_response_format(void);
void test_response_format_overflow(void);
void test_response_build_error(void);

void test_send_file_exact_size(void);
void test_send_file_short_eof(void);

void test_event_poll_read(void);
void test_event_poll_mod(void);
void test_event_poll_timeout(void);
void test_event_poll_multi(void);
void test_event_poll_errors(void);
void test_event_native_read(void);
void test_event_native_mod(void);
void test_event_named_backend(void);
void test_event_unknown_backend(void);

void test_task_pool_basic(void);
void test_task_pool_queue_full(void);
void test_task_pool_timeout(void);
void test_task_pool_empty(void);
void test_task_pool_invalid(void);

void test_logger_basic(void);
void test_logger_not_running(void);
void test_logger_drop(void);

void test_chunked_basic(void);
void test_chunked_partial(void);
void test_chunked_extension(void);
void test_chunked_trailer(void);
void test_chunked_bad_size(void);
void test_chunked_bad_crlf(void);

void test_upstream_content_length(void);
void test_upstream_chunked(void);
void test_upstream_connection_close(void);
void test_upstream_incomplete(void);
void test_upstream_cl_te_conflict(void);
void test_upstream_duplicate_cl(void);
void test_upstream_bad_status(void);
void test_upstream_connection_tokens(void);
void test_upstream_http10_keepalive(void);
void test_upstream_header_htab(void);
void test_upstream_reason(void);

void test_config_file_valid(void);
void test_config_file_duplicate_route(void);
void test_config_file_bad_number(void);
void test_config_file_proxy_without_backend(void);
void test_config_file_ipv6_backend(void);

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
    RUN_TEST(test_duplicate_host_rejected);
    RUN_TEST(test_host_space_before_colon_rejected);
    RUN_TEST(test_header_name_token_rejected);
    RUN_TEST(test_header_value_ctl_rejected);
    RUN_TEST(test_content_length_plus_rejected);
    RUN_TEST(test_method_token_ctl_rejected);
    RUN_TEST(test_target_ctl_rejected);
    RUN_TEST(test_percent_encoded_trailing_slash);
    RUN_TEST(test_percent_encoded_ctl_rejected);
    RUN_TEST(test_parse_incomplete_headers);
    RUN_TEST(test_parse_split_points);

    RUN_TEST(test_http_date_epoch);
    RUN_TEST(test_http_date_roundtrip);
    RUN_TEST(test_http_date_invalid);

    RUN_TEST(test_response_send_memory);
    RUN_TEST(test_response_error_404);
    RUN_TEST(test_response_reason);
    RUN_TEST(test_response_add_header_overflow);
    RUN_TEST(test_response_format);
    RUN_TEST(test_response_format_overflow);
    RUN_TEST(test_response_build_error);

    RUN_TEST(test_send_file_exact_size);
    RUN_TEST(test_send_file_short_eof);

    RUN_TEST(test_event_poll_read);
    RUN_TEST(test_event_poll_mod);
    RUN_TEST(test_event_poll_timeout);
    RUN_TEST(test_event_poll_multi);
    RUN_TEST(test_event_poll_errors);
    RUN_TEST(test_event_native_read);
    RUN_TEST(test_event_native_mod);
    RUN_TEST(test_event_named_backend);
    RUN_TEST(test_event_unknown_backend);

    RUN_TEST(test_task_pool_basic);
    RUN_TEST(test_task_pool_queue_full);
    RUN_TEST(test_task_pool_timeout);
    RUN_TEST(test_task_pool_empty);
    RUN_TEST(test_task_pool_invalid);
    RUN_TEST(test_logger_basic);
    RUN_TEST(test_logger_not_running);
    RUN_TEST(test_logger_drop);
    RUN_TEST(test_chunked_basic);
    RUN_TEST(test_chunked_partial);
    RUN_TEST(test_chunked_extension);
    RUN_TEST(test_chunked_trailer);
    RUN_TEST(test_chunked_bad_size);
    RUN_TEST(test_chunked_bad_crlf);
    RUN_TEST(test_upstream_content_length);
    RUN_TEST(test_upstream_chunked);
    RUN_TEST(test_upstream_connection_close);
    RUN_TEST(test_upstream_incomplete);
    RUN_TEST(test_upstream_cl_te_conflict);
    RUN_TEST(test_upstream_duplicate_cl);
    RUN_TEST(test_upstream_bad_status);
    RUN_TEST(test_upstream_connection_tokens);
    RUN_TEST(test_upstream_http10_keepalive);
    RUN_TEST(test_upstream_header_htab);
    RUN_TEST(test_upstream_reason);
    RUN_TEST(test_config_file_valid);
    RUN_TEST(test_config_file_duplicate_route);
    RUN_TEST(test_config_file_bad_number);
    RUN_TEST(test_config_file_proxy_without_backend);
    RUN_TEST(test_config_file_ipv6_backend);

    printf("%d assertions, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
