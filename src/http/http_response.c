#include "http.h"
#include "access_log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

const char *http_status_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

void http_response_init(http_response_t *res) {
    memset(res, 0, sizeof *res);
}

int http_response_set_status(http_response_t *res, int status) {
    res->status = status;
    return 0;
}

int http_response_add_header(http_response_t *res,
                             const char *name, const char *value) {
    if (res == NULL || name == NULL || value == NULL)
        return -1;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7f)
            return -1;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '\t')
            continue;
        if (*p < 0x20 || *p == 0x7f)
            return -1;
    }

    size_t remaining = sizeof res->headers - res->headers_len;
    if (remaining == 0)
        return -1;

    int n = snprintf(res->headers + res->headers_len, remaining,
                     "%s: %s\r\n", name, value);
    if (n < 0 || (size_t)n >= remaining)
        return -1;

    res->headers_len += (size_t)n;
    return 0;
}

int http_response_format(const http_response_t *res, char *buf, size_t cap,
                         size_t *out_len) {
    if (res == NULL || buf == NULL || out_len == NULL)
        return -1;

    int n = snprintf(buf, cap, "HTTP/1.1 %d %s\r\n%s\r\n",
                     res->status, http_status_reason(res->status),
                     res->headers);
    if (n < 0 || (size_t)n >= cap)
        return -1;

    *out_len = (size_t)n;
    return 0;
}

int http_response_send_headers(int fd, const http_response_t *res) {
    if (res == NULL)
        return -1;

    access_log_t *log = access_log_current();
    if (log != NULL)
        access_log_set_status(log, res->status);

    char buf[HTTP_MAX_RESPONSE_HEADERS + 128];
    size_t len = 0;
    if (http_response_format(res, buf, sizeof buf, &len) < 0)
        return -1;
    if (writen(fd, buf, len) < 0)
        return -1;

    return 0;
}

int http_response_send_redirect(int fd, const char *location,
                                int close_connection) {
    if (location == NULL)
        return -1;

    http_response_t res;
    http_response_init(&res);
    res.status = 301;
    res.close_connection = close_connection;

    if (http_response_add_header(&res, "Location", location) < 0)
        return -1;
    if (http_response_add_header(&res, "Content-Length", "0") < 0)
        return -1;
    if (http_response_add_header(&res, "Connection",
                                 close_connection ? "close" : "keep-alive") < 0)
        return -1;

    return http_response_send_headers(fd, &res);
}

int http_response_send_options(int fd, int close_connection) {
    http_response_t res;
    http_response_init(&res);
    res.status = 200;
    res.close_connection = close_connection;

    if (http_response_add_header(&res, "Allow",
                                 "GET, POST, HEAD, OPTIONS") < 0)
        return -1;
    if (http_response_add_header(&res, "Content-Length", "0") < 0)
        return -1;
    if (http_response_add_header(&res, "Connection",
                                 close_connection ? "close" : "keep-alive") < 0)
        return -1;

    return http_response_send_headers(fd, &res);
}

int http_response_send_memory(int fd, int status, const char *content_type,
                              const char *body, size_t len) {
    if (content_type == NULL || (body == NULL && len > 0))
        return -1;

    http_response_t res;
    http_response_init(&res);
    res.status = status;
    res.close_connection = 1;

    char lenbuf[32];
    int n = snprintf(lenbuf, sizeof lenbuf, "%zu", len);
    if (n < 0 || (size_t)n >= sizeof lenbuf)
        return -1;

    if (http_response_add_header(&res, "Content-Type", content_type) < 0)
        return -1;
    if (http_response_add_header(&res, "Content-Length", lenbuf) < 0)
        return -1;
    if (http_response_add_header(&res, "Connection", "close") < 0)
        return -1;

    if (http_response_send_headers(fd, &res) < 0)
        return -1;
    if (len > 0 && write_body(fd, body, len) < 0)
        return -1;

    return 0;
}

int http_response_build_error(int status, int close_connection,
                              http_response_t *res, char *body,
                              size_t body_cap, size_t *body_len) {
    if (res == NULL || body == NULL || body_len == NULL)
        return -1;

    const char *reason = http_status_reason(status);

    int n = snprintf(body, body_cap,
                     "<!DOCTYPE html>\n"
                     "<html><head><title>%d %s</title></head>"
                     "<body><h1>%d %s</h1></body></html>\n",
                     status, reason, status, reason);
    if (n < 0 || (size_t)n >= body_cap)
        return -1;

    http_response_init(res);
    res->status = status;
    res->close_connection = close_connection;

    char lenbuf[32];
    int m = snprintf(lenbuf, sizeof lenbuf, "%d", n);
    if (m < 0 || (size_t)m >= sizeof lenbuf)
        return -1;

    if (http_response_add_header(res, "Content-Type",
                                 "text/html; charset=utf-8") < 0)
        return -1;
    if (status == HTTP_METHOD_NOT_ALLOWED &&
        http_response_add_header(res, "Allow", "GET, HEAD") < 0)
        return -1;
    if (http_response_add_header(res, "Content-Length", lenbuf) < 0)
        return -1;
    if (http_response_add_header(res, "Connection",
                                 close_connection ? "close" : "keep-alive") < 0)
        return -1;

    *body_len = (size_t)n;
    return 0;
}

int http_response_send_error_ex(int fd, int status, int close_connection,
                                int head) {
    http_response_t res;
    char body[512];
    size_t body_len = 0;

    if (http_response_build_error(status, close_connection, &res, body,
                                  sizeof body, &body_len) < 0)
        return -1;

    if (http_response_send_headers(fd, &res) < 0)
        return -1;
    if (!head && write_body(fd, body, body_len) < 0)
        return -1;

    return 0;
}

int http_response_send_error(int fd, int status, int close_connection) {
    return http_response_send_error_ex(fd, status, close_connection, 0);
}
