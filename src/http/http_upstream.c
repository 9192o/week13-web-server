#include "http_upstream.h"

#include <ctype.h>
#include <string.h>

static const char *find_crlf(const char *p, const char *end) {
    for (; p + 1 < end; p++) {
        if (p[0] == '\r' && p[1] == '\n')
            return p;
    }
    return NULL;
}

static int ci_equal(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen)
        return 0;
    for (size_t i = 0; i < alen; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

static int is_token(const char *s, size_t len) {
    if (len == 0)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c >= 0x7f)
            return 0;
        if (strchr("()<>@,;:\\\"/[]?={}", c) != NULL)
            return 0;
    }
    return 1;
}

static void trim_ows(const char **s, size_t *len) {
    while (*len > 0 && ((*s)[0] == ' ' || (*s)[0] == '\t')) {
        (*s)++;
        (*len)--;
    }
    while (*len > 0) {
        char c = (*s)[*len - 1];
        if (c != ' ' && c != '\t')
            break;
        (*len)--;
    }
}

static int has_ctl(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return 1;
    }
    return 0;
}

static int connection_has_token(const char *s, size_t len, const char *token) {
    size_t tlen = strlen(token);
    while (len > 0) {
        const char *comma = memchr(s, ',', len);
        size_t tok_len = comma ? (size_t)(comma - s) : len;
        while (tok_len > 0 && (s[0] == ' ' || s[0] == '\t')) {
            s++;
            tok_len--;
        }
        while (tok_len > 0 && (s[tok_len - 1] == ' ' ||
                               s[tok_len - 1] == '\t'))
            tok_len--;
        if (tok_len == tlen) {
            size_t i = 0;
            while (i < tlen &&
                   tolower((unsigned char)s[i]) ==
                       tolower((unsigned char)token[i]))
                i++;
            if (i == tlen)
                return 1;
        }
        if (comma == NULL)
            break;
        size_t used = (size_t)(comma - s) + 1;
        s += used;
        len -= used;
    }
    return 0;
}

void upstream_response_init(upstream_response_t *res) {
    if (res != NULL)
        memset(res, 0, sizeof *res);
}

static http_status_t parse_content_length(upstream_response_t *res,
                                          const char *value, size_t len) {
    while (len > 0 && (*value == ' ' || *value == '\t')) {
        value++;
        len--;
    }
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t'))
        len--;
    if (len == 0)
        return HTTP_BAD_GATEWAY;

    size_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (value[i] < '0' || value[i] > '9')
            return HTTP_BAD_GATEWAY;
        unsigned digit = (unsigned)(value[i] - '0');
        if (v > ((size_t)-1 - digit) / 10)
            return HTTP_BAD_GATEWAY;
        v = v * 10 + digit;
    }

    if (res->has_content_length && res->content_length != v)
        return HTTP_BAD_GATEWAY;
    res->has_content_length = 1;
    res->content_length = v;
    return HTTP_OK;
}

http_status_t upstream_response_parse(const char *buf, size_t len,
                                      const config_t *cfg,
                                      upstream_response_t *res) {
    if (buf == NULL || cfg == NULL || res == NULL)
        return HTTP_BAD_GATEWAY;

    const char *end = buf + len;
    const char *line_end = find_crlf(buf, end);
    if (line_end == NULL)
        return HTTP_PARSE_INCOMPLETE;
    if ((size_t)(line_end - buf) + 2 > cfg->max_header_bytes)
        return HTTP_BAD_GATEWAY;

    const char *p = buf;
    if (line_end - p < 12 || memcmp(p, "HTTP/1.", 7) != 0)
        return HTTP_BAD_GATEWAY;
    p += 7;
    if (*p < '0' || *p > '9')
        return HTTP_BAD_GATEWAY;
    res->version_minor = *p - '0';
    res->version_major = 1;
    p++;
    if (p >= line_end || *p != ' ')
        return HTTP_BAD_GATEWAY;
    p++;
    if (line_end - p < 3 ||
        p[0] < '0' || p[0] > '9' ||
        p[1] < '0' || p[1] > '9' ||
        p[2] < '0' || p[2] > '9')
        return HTTP_BAD_GATEWAY;
    int status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    if (status < 100 || status > 599)
        return HTTP_BAD_GATEWAY;
    res->status = status;

    const char *reason = p + 3;
    if (reason < line_end && *reason == ' ')
        reason++;
    size_t reason_len = (size_t)(line_end - reason);
    while (reason_len > 0 && reason[reason_len - 1] == ' ')
        reason_len--;
    if (reason_len >= sizeof res->reason)
        reason_len = sizeof res->reason - 1;
    memcpy(res->reason, reason, reason_len);
    res->reason[reason_len] = '\0';

    p = line_end + 2;
    while (1) {
        if (p + 1 >= end)
            return HTTP_PARSE_INCOMPLETE;
        if ((size_t)(p - buf) + 2 > cfg->max_header_bytes)
            return HTTP_BAD_GATEWAY;

        if (p[0] == '\r' && p[1] == '\n') {
            res->header_len = (size_t)(p - buf) + 2;
            break;
        }

        const char *he = find_crlf(p, end);
        if (he == NULL)
            return HTTP_PARSE_INCOMPLETE;
        if ((size_t)(he - buf) + 2 > cfg->max_header_bytes)
            return HTTP_BAD_GATEWAY;

        const char *colon = memchr(p, ':', (size_t)(he - p));
        if (colon == NULL)
            return HTTP_BAD_GATEWAY;

        const char *name = p;
        size_t name_len = (size_t)(colon - p);
        const char *value = colon + 1;
        size_t value_len = (size_t)(he - value);

        if (!is_token(name, name_len))
            return HTTP_BAD_GATEWAY;
        trim_ows(&value, &value_len);
        if (has_ctl(value, value_len))
            return HTTP_BAD_GATEWAY;

        if (res->header_count >= HTTP_MAX_HEADERS ||
            res->header_count >= cfg->max_headers)
            return HTTP_BAD_GATEWAY;

        upstream_header_t *h = &res->headers[res->header_count++];
        h->name = name;
        h->name_len = name_len;
        h->value = value;
        h->value_len = value_len;

        if (ci_equal(name, name_len, "Content-Length")) {
            http_status_t st = parse_content_length(res, value, value_len);
            if (st != HTTP_OK)
                return st;
        } else if (ci_equal(name, name_len, "Transfer-Encoding")) {
            if (res->chunked || !ci_equal(value, value_len, "chunked"))
                return HTTP_BAD_GATEWAY;
            res->chunked = 1;
        } else if (ci_equal(name, name_len, "Connection")) {
            if (connection_has_token(value, value_len, "close"))
                res->connection_close = 1;
            if (connection_has_token(value, value_len, "keep-alive"))
                res->connection_keep_alive = 1;
        }

        p = he + 2;
    }

    if (res->has_content_length && res->chunked)
        return HTTP_BAD_GATEWAY;

    return HTTP_OK;
}
