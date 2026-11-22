#ifndef HTTP_H
#define HTTP_H

#include "config.h"

#include <signal.h>
#include <stddef.h>

#define HTTP_MAX_URI     4096
#define HTTP_MAX_HEADERS 100
#define HTTP_MAX_METHOD  16

typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_HEAD,
    HTTP_OPTIONS,
    HTTP_UNKNOWN
} http_method_t;

typedef enum {
    HTTP_PARSE_INCOMPLETE = 1,
    HTTP_OK = 200,
    HTTP_BAD_REQUEST = 400,
    HTTP_FORBIDDEN = 403,
    HTTP_NOT_FOUND = 404,
    HTTP_METHOD_NOT_ALLOWED = 405,
    HTTP_REQUEST_TIMEOUT = 408,
    HTTP_LENGTH_REQUIRED = 411,
    HTTP_PAYLOAD_TOO_LARGE = 413,
    HTTP_URI_TOO_LONG = 414,
    HTTP_HEADERS_TOO_LARGE = 431,
    HTTP_INTERNAL_SERVER_ERROR = 500,
    HTTP_NOT_IMPLEMENTED = 501,
    HTTP_GATEWAY_TIMEOUT = 504,
    HTTP_VERSION_NOT_SUPPORTED = 505
} http_status_t;

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} http_header_t;

typedef struct {
    http_method_t method;
    char method_str[HTTP_MAX_METHOD];
    char target[HTTP_MAX_URI];
    char path[HTTP_MAX_URI];
    char query[HTTP_MAX_URI];

    int version_major;
    int version_minor;

    http_header_t headers[HTTP_MAX_HEADERS];
    size_t header_count;
    size_t header_len;

    size_t content_length;
    int has_content_length;
    int has_transfer_encoding;

    char *body;
    size_t body_len;

    int keep_alive;
    int expect_continue;
    int has_host;
} http_request_t;

void http_request_init(http_request_t *req);
void http_request_reset(http_request_t *req);
void http_request_free(http_request_t *req);

http_status_t http_request_parse_buffer(const char *buf, size_t len,
                                        const config_t *cfg,
                                        http_request_t *req);

http_status_t http_request_read_body(int fd, const char *buf, size_t len,
                                     const config_t *cfg,
                                     http_request_t *req,
                                     const volatile sig_atomic_t *stop);

const char *http_request_get_header(const http_request_t *req,
                                    const char *name, size_t *value_len);

#define HTTP_MAX_RESPONSE_HEADERS 4096

typedef struct {
    int status;
    char headers[HTTP_MAX_RESPONSE_HEADERS];
    size_t headers_len;
    int close_connection;
} http_response_t;

void http_response_init(http_response_t *res);
int http_response_set_status(http_response_t *res, int status);
int http_response_add_header(http_response_t *res,
                             const char *name, const char *value);
int http_response_send_headers(int fd, const http_response_t *res);
int http_response_send_error(int fd, int status, int close_connection);
int http_response_send_redirect(int fd, const char *location,
                                int close_connection);
int http_response_send_options(int fd, int close_connection);
int http_response_send_memory(int fd, int status, const char *content_type,
                              const char *body, size_t len);
const char *http_status_reason(int status);

#endif
