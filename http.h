#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_REQUEST_LINE_MAX 8192
#define HTTP_HEADER_LINE_MAX 8192
#define HTTP_REQUEST_HEAD_MAX 32768
#define HTTP_HEADER_COUNT_MAX 100
#define HTTP_BODY_MAX 1048576
#define HTTP_CHUNK_LINE_MAX 8192
#define HTTP_CHUNK_EXTENSIONS_MAX 32768
#define HTTP_TRAILER_MAX 32768
#define HTTP_TRAILER_COUNT_MAX 100

enum http_parse_status
{
    HTTP_PARSE_NEED_MORE,
    HTTP_PARSE_COMPLETE,
    HTTP_PARSE_ERROR
};

struct http_slice
{
    const char *data;
    size_t length;
};

struct http_request_line
{
    struct http_slice method;
    struct http_slice target;
    unsigned int version_major;
    unsigned int version_minor;
};

struct http_request_line_result
{
    enum http_parse_status status;
    size_t consumed;
    int error_status;
    struct http_request_line request;
};

struct http_header
{
    struct http_slice name;
    struct http_slice value;
};

struct http_headers
{
    struct http_header fields[HTTP_HEADER_COUNT_MAX];
    size_t count;
};

struct http_headers_result
{
    enum http_parse_status status;
    size_t consumed;
    int error_status;
    struct http_headers headers;
};

enum http_target_form
{
    HTTP_TARGET_ORIGIN,
    HTTP_TARGET_ABSOLUTE,
    HTTP_TARGET_AUTHORITY,
    HTTP_TARGET_ASTERISK
};

struct http_target
{
    enum http_target_form form;
    struct http_slice authority;
    struct http_slice path;
    struct http_slice query;
};

struct http_request
{
    struct http_request_line line;
    struct http_headers headers;
    struct http_target target;
    struct http_slice body;
};

struct http_response
{
    int status;
    const char *content_type;
    const char *allow;
    /* Borrowed bytes, or a file's snapshot length when file_fd >= 0. */
    struct http_slice body;
    int file_fd;
};

const char *http_reason_phrase(int status);
size_t http_format_response_head(const struct http_response *response, char *buffer, size_t capacity);

enum http_body_state
{
    HTTP_BODY_LENGTH,
    HTTP_BODY_CHUNK_SIZE,
    HTTP_BODY_CHUNK_DATA,
    HTTP_BODY_CHUNK_CR,
    HTTP_BODY_CHUNK_LF,
    HTTP_BODY_TRAILERS,
    HTTP_BODY_DONE,
    HTTP_BODY_FAILED
};

struct http_body_parser
{
    enum http_body_state state;
    size_t remaining;
    char *data;
    size_t length;
    size_t capacity;
    char line[HTTP_CHUNK_LINE_MAX];
    size_t line_length;
    size_t extension_bytes;
    size_t trailer_bytes;
    size_t trailer_count;
    int expect_continue;
    int error_status;
};

struct http_body_result
{
    enum http_parse_status status;
    size_t consumed;
    int error_status;
};

/*
 * Pass the accumulated bytes from the start of the request line, not just
 * the latest recv. COMPLETE means only the request line is complete.
 * On COMPLETE, slices borrow data and consumed includes CRLF.
 * On NEED_MORE/ERROR, consumed is zero and request fields are empty.
 * error_status is 400/414 on ERROR, otherwise zero.
 * Version support and URI semantics are checked separately.
 */
struct http_request_line_result http_parse_request_line(const char *data, size_t length);

/*
 * Pass accumulated bytes AFTER the request line, including the final empty line.
 * request_line_length must be consumed from a COMPLETE request-line result.
 * It counts toward HTTP_REQUEST_HEAD_MAX; body bytes do not.
 * COMPLETE means header syntax only, not Host/framing validation or body completion.
 * Names retain original case; consumers must compare them case-insensitively.
 * Values exclude leading/trailing SP/HTAB. Duplicates remain separate, in order.
 * Slices borrow data; input is unchanged and need not be NUL-terminated.
 * consumed includes the final CRLF, relative to data (the body starts there).
 * NEED_MORE/ERROR return consumed zero and empty headers; ERROR is 400/431.
 */
struct http_headers_result http_parse_headers(const char *data, size_t length, size_t request_line_length);

/*
 * Call only after COMPLETE line and headers, before body init/100 Continue.
 * Returns 0, 400 or 505; clears target on error. Validation order is version,
 * target syntax, then Host. Does not approve method/resource support.
 * Target components borrow input; an empty absolute path uses static "/".
 * Path/query stay percent-encoded. No decoding or filesystem normalization.
 * Absolute-form authority overrides a syntactically valid Host value.
 */
int http_validate_request_head(const struct http_request_line *line,
                               const struct http_headers *headers,
                               struct http_target *target);

/*
 * Initialize once from COMPLETE generic headers. Returns 0 or HTTP error status.
 * Validates CL/TE framing and Expect, not Host, methods, targets or versions.
 * Feed ONLY new bytes to http_parse_body; consumed applies even to NEED_MORE.
 * Decoded binary bytes accumulate in parser->data/length, growing on demand.
 * COMPLETE stops at the body boundary, leaving surplus input unconsumed.
 * EOF before COMPLETE is an error for the caller to handle.
 * Call destroy on every path, including failed init. Do not re-init a live parser.
 */
int http_body_init(struct http_body_parser *parser, const struct http_headers *headers);
struct http_body_result http_parse_body(struct http_body_parser *parser, const char *data, size_t length);
void http_body_destroy(struct http_body_parser *parser);

extern const char HTTP_CONTINUE_RESPONSE[];

#endif
