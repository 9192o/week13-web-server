#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_REQUEST_LINE_MAX 8192

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

/*
 * Pass the accumulated bytes from the start of the request line, not just
 * the latest recv. COMPLETE means only the request line is complete.
 * On COMPLETE, slices borrow data and consumed includes CRLF.
 * On NEED_MORE/ERROR, consumed is zero and request fields are empty.
 * error_status is 400/414 on ERROR, otherwise zero.
 * Version support and URI semantics are checked separately.
 */
struct http_request_line_result http_parse_request_line(const char *data, size_t length);

extern const char HTTP_OK_RESPONSE[];
extern const char HTTP_BAD_REQUEST_RESPONSE[];
extern const char HTTP_URI_TOO_LONG_RESPONSE[];

#endif
