#include "http.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_PATH_SEGMENTS 256

static const char *find_crlf(const char *p, const char *end) {
    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n')
            return p;
        p++;
    }
    return NULL;
}

static int is_tchar(unsigned char c) {
    if (isalnum(c))
        return 1;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return 1;
    default:
        return 0;
    }
}

static int is_token(const char *s, size_t len) {
    if (len == 0)
        return 0;
    for (size_t i = 0; i < len; i++) {
        if (!is_tchar((unsigned char)s[i]))
            return 0;
    }
    return 1;
}

static int has_ctl(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t')
            continue;
        if (c < 0x20 || c == 0x7f)
            return 1;
    }
    return 0;
}

static void trim_ows(const char **s, size_t *len) {
    while (*len > 0 && (**s == ' ' || **s == '\t')) {
        (*s)++;
        (*len)--;
    }
    while (*len > 0 && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t'))
        (*len)--;
}

static int ci_equal(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    return alen == blen && strncasecmp(a, b, alen) == 0;
}

static int ci_contains(const char *s, size_t len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen)
        return 0;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (strncasecmp(s + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static http_method_t parse_method(const char *s, size_t len) {
    if (len == 3 && memcmp(s, "GET", 3) == 0)
        return HTTP_GET;
    if (len == 4 && memcmp(s, "POST", 4) == 0)
        return HTTP_POST;
    if (len == 4 && memcmp(s, "HEAD", 4) == 0)
        return HTTP_HEAD;
    if (len == 7 && memcmp(s, "OPTIONS", 7) == 0)
        return HTTP_OPTIONS;
    return HTTP_UNKNOWN;
}

static http_status_t parse_version(const char *s, size_t len,
                                   int *major, int *minor) {
    if (len != 8 || strncmp(s, "HTTP/", 5) != 0)
        return HTTP_BAD_REQUEST;
    if (!isdigit((unsigned char)s[5]) || s[6] != '.' ||
        !isdigit((unsigned char)s[7]))
        return HTTP_BAD_REQUEST;

    *major = s[5] - '0';
    *minor = s[7] - '0';

    if (*major != 1 || (*minor != 0 && *minor != 1))
        return HTTP_VERSION_NOT_SUPPORTED;
    return HTTP_OK;
}

static http_status_t parse_request_line(const char *line, size_t line_len,
                                        http_request_t *req) {
    const char *sp1 = memchr(line, ' ', line_len);
    if (sp1 == NULL)
        return HTTP_BAD_REQUEST;

    const char *after1 = sp1 + 1;
    size_t rest = line_len - (size_t)(after1 - line);
    const char *sp2 = memchr(after1, ' ', rest);
    if (sp2 == NULL)
        return HTTP_BAD_REQUEST;

    const char *method = line;
    size_t method_len = (size_t)(sp1 - line);
    const char *target = after1;
    size_t target_len = (size_t)(sp2 - after1);
    const char *version = sp2 + 1;
    size_t version_len = line_len - (size_t)(version - line);

    if (method_len == 0 || method_len >= HTTP_MAX_METHOD)
        return HTTP_BAD_REQUEST;
    if (!is_token(method, method_len))
        return HTTP_BAD_REQUEST;
    if (target_len == 0)
        return HTTP_BAD_REQUEST;
    if (target_len >= HTTP_MAX_URI)
        return HTTP_URI_TOO_LONG;

    for (size_t i = 0; i < target_len; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c < 0x21 || c > 0x7e)
            return HTTP_BAD_REQUEST;
    }

    memcpy(req->method_str, method, method_len);
    req->method_str[method_len] = '\0';
    req->method = parse_method(method, method_len);
    if (req->method == HTTP_UNKNOWN)
        return HTTP_NOT_IMPLEMENTED;

    memcpy(req->target, target, target_len);
    req->target[target_len] = '\0';

    return parse_version(version, version_len,
                         &req->version_major, &req->version_minor);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int decode_uri(const char *src, size_t src_len,
                      char *dst, size_t dst_len) {
    size_t j = 0;

    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];

        if (c == '%') {
            if (i + 2 >= src_len)
                return -1;
            int hi = hexval(src[i + 1]);
            int lo = hexval(src[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;
            c = (char)((hi << 4) | lo);
            i += 2;
        }

        if (c == '\0')
            return -1;
        if (j + 1 >= dst_len)
            return -1;
        dst[j++] = c;
    }

    dst[j] = '\0';
    return 0;
}

static int normalize_path(const char *decoded, size_t decoded_len,
                          int trailing_slash, char *out, size_t out_len) {
    char tmp[HTTP_MAX_URI];
    if (decoded_len >= sizeof tmp)
        return -1;
    memcpy(tmp, decoded, decoded_len);
    tmp[decoded_len] = '\0';

    char *parts[MAX_PATH_SEGMENTS];
    size_t n = 0;
    char *save = NULL;
    char *tok = strtok_r(tmp, "/", &save);

    while (tok != NULL) {
        if (strcmp(tok, ".") == 0) {
            /* skip */
        } else if (strcmp(tok, "..") == 0) {
            if (n == 0)
                return -1; /* docroot 탈출 */
            n--;
        } else {
            if (n >= MAX_PATH_SEGMENTS)
                return -1;
            parts[n++] = tok;
        }
        tok = strtok_r(NULL, "/", &save);
    }

    size_t pos = 0;
    if (out_len < 2)
        return -1;
    out[pos++] = '/';

    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(parts[i]);
        if (pos + len + 1 >= out_len)
            return -1;
        memcpy(out + pos, parts[i], len);
        pos += len;
        if (i + 1 < n)
            out[pos++] = '/';
    }

    if (trailing_slash && pos > 1) {
        if (pos + 1 >= out_len)
            return -1;
        out[pos++] = '/';
    }

    out[pos] = '\0';
    return 0;
}

static http_status_t parse_uri(http_request_t *req, const config_t *cfg) {
    size_t target_len = strlen(req->target);
    size_t max_uri = cfg->max_uri;
    if (max_uri > HTTP_MAX_URI - 1)
        max_uri = HTTP_MAX_URI - 1;
    if (target_len > max_uri)
        return HTTP_URI_TOO_LONG;

    for (size_t i = 0; i < target_len; i++) {
        unsigned char c = (unsigned char)req->target[i];
        if (c < 0x21 || c > 0x7e)
            return HTTP_BAD_REQUEST;
    }

    if (req->target[0] != '/')
        return HTTP_BAD_REQUEST;

    const char *q = memchr(req->target, '?', target_len);
    size_t path_len = q ? (size_t)(q - req->target) : target_len;
    const char *query = q ? q + 1 : NULL;
    size_t query_len = q ? target_len - path_len - 1 : 0;

    char decoded[HTTP_MAX_URI];
    if (decode_uri(req->target, path_len, decoded, sizeof decoded) < 0)
        return HTTP_BAD_REQUEST;

    size_t decoded_len = strlen(decoded);
    for (size_t i = 0; i < decoded_len; i++) {
        unsigned char c = (unsigned char)decoded[i];
        if (c < 0x20 || c == 0x7f)
            return HTTP_BAD_REQUEST;
    }

    int trailing_slash = (decoded_len > 1 &&
                          decoded[decoded_len - 1] == '/');
    if (normalize_path(decoded, decoded_len, trailing_slash,
                       req->path, sizeof req->path) < 0)
        return HTTP_FORBIDDEN;

    if (query_len >= sizeof req->query)
        return HTTP_URI_TOO_LONG;
    if (query_len > 0)
        memcpy(req->query, query, query_len);
    req->query[query_len] = '\0';

    return HTTP_OK;
}

static http_status_t parse_content_length(http_request_t *req,
                                          const char *value, size_t value_len,
                                          const config_t *cfg) {
    char tmp[32];
    if (value_len == 0 || value_len >= sizeof tmp)
        return HTTP_BAD_REQUEST;
    for (size_t i = 0; i < value_len; i++) {
        if (value[i] < '0' || value[i] > '9')
            return HTTP_BAD_REQUEST;
    }
    memcpy(tmp, value, value_len);
    tmp[value_len] = '\0';

    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(tmp, &end, 10);
    if (errno != 0 || end == tmp || *end != '\0')
        return HTTP_BAD_REQUEST;

    if (req->has_content_length && req->content_length != (size_t)v)
        return HTTP_BAD_REQUEST;

    req->has_content_length = 1;
    req->content_length = (size_t)v;

    if (req->content_length > cfg->max_body)
        return HTTP_PAYLOAD_TOO_LARGE;

    return HTTP_OK;
}

static http_status_t parse_headers(const char *start, const char *buf_end,
                                   const config_t *cfg, http_request_t *req,
                                   const char **headers_end_out) {
    size_t max_headers = cfg->max_headers;
    if (max_headers > HTTP_MAX_HEADERS)
        max_headers = HTTP_MAX_HEADERS;

    const char *p = start;

    while (1) {
        if (p + 1 >= buf_end)
            return HTTP_PARSE_INCOMPLETE;

        if (p[0] == '\r' && p[1] == '\n') {
            *headers_end_out = p;
            return HTTP_OK;
        }

        /* obs-fold (continuation line) is not supported. */
        if (p[0] == ' ' || p[0] == '\t')
            return HTTP_BAD_REQUEST;

        if ((size_t)(p - start) > cfg->max_header_bytes)
            return HTTP_HEADERS_TOO_LARGE;

        const char *line_end = find_crlf(p, buf_end);
        if (line_end == NULL)
            return HTTP_PARSE_INCOMPLETE;

        if ((size_t)(line_end - start) + 2 > cfg->max_header_bytes)
            return HTTP_HEADERS_TOO_LARGE;

        if (req->header_count >= max_headers)
            return HTTP_HEADERS_TOO_LARGE;

        const char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon == NULL)
            return HTTP_BAD_REQUEST;

        const char *name = p;
        size_t name_len = (size_t)(colon - p);
        const char *value = colon + 1;
        size_t value_len = (size_t)(line_end - value);

        if (!is_token(name, name_len))
            return HTTP_BAD_REQUEST;

        trim_ows(&value, &value_len);
        if (has_ctl(value, value_len))
            return HTTP_BAD_REQUEST;

        req->headers[req->header_count].name = name;
        req->headers[req->header_count].name_len = name_len;
        req->headers[req->header_count].value = value;
        req->headers[req->header_count].value_len = value_len;
        req->header_count++;

        if (ci_equal(name, name_len, "Host")) {
            if (req->has_host || value_len == 0)
                return HTTP_BAD_REQUEST;
            for (size_t i = 0; i < value_len; i++) {
                if (value[i] == ' ' || value[i] == '\t')
                    return HTTP_BAD_REQUEST;
            }
            req->has_host = 1;
        } else if (ci_equal(name, name_len, "Connection")) {
            if (ci_contains(value, value_len, "close"))
                req->keep_alive = 0;
            else if (ci_contains(value, value_len, "keep-alive"))
                req->keep_alive = 1;
        } else if (ci_equal(name, name_len, "Content-Length")) {
            http_status_t st = parse_content_length(req, value, value_len, cfg);
            if (st != HTTP_OK)
                return st;
        } else if (ci_equal(name, name_len, "Transfer-Encoding")) {
            req->has_transfer_encoding = 1;
        } else if (ci_equal(name, name_len, "Expect")) {
            if (ci_contains(value, value_len, "100-continue"))
                req->expect_continue = 1;
        }

        p = line_end + 2;
    }
}

void http_request_init(http_request_t *req) {
    memset(req, 0, sizeof *req);
    req->method = HTTP_UNKNOWN;
}

void http_request_free(http_request_t *req) {
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

void http_request_reset(http_request_t *req) {
    http_request_free(req);
    http_request_init(req);
}

http_status_t http_request_parse_buffer(const char *buf, size_t len,
                                        const config_t *cfg,
                                        http_request_t *req) {
    if (buf == NULL || cfg == NULL || req == NULL)
        return HTTP_BAD_REQUEST;

    const char *buf_end = buf + len;
    const char *line_end = find_crlf(buf, buf_end);
    if (line_end == NULL)
        return HTTP_PARSE_INCOMPLETE;

    if ((size_t)(line_end - buf) + 2 > cfg->max_header_bytes)
        return HTTP_HEADERS_TOO_LARGE;

    http_status_t st = parse_request_line(buf, (size_t)(line_end - buf), req);
    if (st != HTTP_OK)
        return st;

    req->keep_alive = (req->version_minor == 1);

    const char *headers_end = NULL;
    st = parse_headers(line_end + 2, buf_end, cfg, req, &headers_end);
    if (st != HTTP_OK)
        return st;

    if ((size_t)(headers_end - buf) + 2 > cfg->max_header_bytes)
        return HTTP_HEADERS_TOO_LARGE;

    req->header_len = (size_t)(headers_end - buf) + 2;

    if (req->has_transfer_encoding) {
        if (req->has_content_length)
            return HTTP_BAD_REQUEST;
        return HTTP_NOT_IMPLEMENTED;
    }

    if (req->version_major == 1 && req->version_minor == 1 && !req->has_host)
        return HTTP_BAD_REQUEST;

    if (req->method == HTTP_POST && !req->has_content_length)
        return HTTP_LENGTH_REQUIRED;

    return parse_uri(req, cfg);
}

http_status_t http_request_read_body(int fd, const char *buf, size_t len,
                                     const config_t *cfg,
                                     http_request_t *req,
                                     uint64_t deadline_ms,
                                     const volatile sig_atomic_t *stop) {
    free(req->body);
    req->body = NULL;
    req->body_len = 0;

    if (!req->has_content_length || req->content_length == 0)
        return HTTP_OK;

    if (req->content_length > cfg->max_body)
        return HTTP_PAYLOAD_TOO_LARGE;
    if (req->header_len > len)
        return HTTP_BAD_REQUEST;

    size_t available = len - req->header_len;
    size_t to_copy = available < req->content_length
                         ? available
                         : req->content_length;
    size_t cap = req->content_length < 16384 ? req->content_length : 16384;
    if (cap < to_copy)
        cap = to_copy;

    req->body = malloc(cap);
    if (req->body == NULL)
        return HTTP_INTERNAL_SERVER_ERROR;

    if (to_copy > 0)
        memcpy(req->body, buf + req->header_len, to_copy);

    size_t body_len = to_copy;
    while (body_len < req->content_length) {
        if (body_len == cap) {
            size_t newcap = cap * 2;
            if (newcap > req->content_length)
                newcap = req->content_length;
            char *grown = realloc(req->body, newcap);
            if (grown == NULL) {
                free(req->body);
                req->body = NULL;
                return HTTP_INTERNAL_SERVER_ERROR;
            }
            req->body = grown;
            cap = newcap;
        }

        size_t want = cap - body_len;
        ssize_t n = readn_deadline(fd, req->body + body_len, want,
                                   deadline_ms, stop);
        if (n < 0) {
            int err = errno;
            free(req->body);
            req->body = NULL;
            if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK)
                return HTTP_REQUEST_TIMEOUT;
            return HTTP_BAD_REQUEST;
        }
        if ((size_t)n != want) {
            free(req->body);
            req->body = NULL;
            return HTTP_BAD_REQUEST;
        }
        body_len += want;
    }

    req->body_len = req->content_length;
    return HTTP_OK;
}

const char *http_request_get_header(const http_request_t *req,
                                    const char *name, size_t *value_len) {
    size_t name_len = strlen(name);

    for (size_t i = 0; i < req->header_count; i++) {
        if (req->headers[i].name_len == name_len &&
            strncasecmp(req->headers[i].name, name, name_len) == 0) {
            if (value_len != NULL)
                *value_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }

    if (value_len != NULL)
        *value_len = 0;
    return NULL;
}
