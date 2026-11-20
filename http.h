#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_REQUEST_LINE_MAX 8192
#define HTTP_HEADER_LINE_MAX 8192
#define HTTP_REQUEST_HEAD_MAX 32768
#define HTTP_HEADER_COUNT_MAX 100

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

extern const char HTTP_OK_RESPONSE[];
extern const char HTTP_BAD_REQUEST_RESPONSE[];
extern const char HTTP_URI_TOO_LONG_RESPONSE[];

#endif
