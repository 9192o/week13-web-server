#include "proxy_wire.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} proxy_buf_t;

static int buf_reserve(proxy_buf_t *b, size_t extra) {
    if (b->len + extra <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra)
        cap *= 2;
    char *p = realloc(b->data, cap);
    if (p == NULL)
        return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

static int buf_append(proxy_buf_t *b, const char *s, size_t n) {
    if (buf_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 0;
}

static int buf_puts(proxy_buf_t *b, const char *s) {
    return buf_append(b, s, strlen(s));
}

static int buf_printf(proxy_buf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n < sizeof tmp)
        return buf_append(b, tmp, (size_t)n);

    char *big = malloc((size_t)n + 1);
    if (big == NULL)
        return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = buf_append(b, big, (size_t)n);
    free(big);
    return rc;
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

static int is_hop_by_hop(const char *name, size_t len) {
    static const char *names[] = {
        "Connection", "Keep-Alive", "Proxy-Authenticate",
        "Proxy-Authorization", "Proxy-Connection", "TE", "Trailer",
        "Transfer-Encoding", "Upgrade"
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (ci_equal(name, len, names[i]))
            return 1;
    }
    return 0;
}

static int connection_names(const http_request_t *req,
                            const char *name, size_t len) {
    for (size_t i = 0; i < req->header_count; i++) {
        if (!ci_equal(req->headers[i].name, req->headers[i].name_len,
                      "Connection"))
            continue;

        const char *p = req->headers[i].value;
        size_t n = req->headers[i].value_len;
        while (n > 0) {
            const char *comma = memchr(p, ',', n);
            size_t tok_len = comma ? (size_t)(comma - p) : n;
            const char *tok = p;

            while (tok_len > 0 && (tok[0] == ' ' || tok[0] == '\t')) {
                tok++;
                tok_len--;
            }
            while (tok_len > 0 && (tok[tok_len - 1] == ' ' ||
                                   tok[tok_len - 1] == '\t'))
                tok_len--;

            if (len > 0 && tok_len == len) {
                size_t j = 0;
                while (j < len &&
                       tolower((unsigned char)tok[j]) ==
                           tolower((unsigned char)name[j]))
                    j++;
                if (j == len)
                    return 1;
            }

            if (comma == NULL)
                break;
            size_t used = (size_t)(comma - p) + 1;
            p += used;
            n -= used;
        }
    }
    return 0;
}

static int response_connection_names(const proxy_conn_t *p,
                                     const char *name, size_t len) {
    for (size_t i = 0; i < p->resp.header_count; i++) {
        const upstream_header_t *h = &p->resp.headers[i];
        if (!ci_equal(h->name, h->name_len, "Connection"))
            continue;

        const char *s = h->value;
        size_t n = h->value_len;
        while (n > 0) {
            const char *comma = memchr(s, ',', n);
            size_t tok_len = comma ? (size_t)(comma - s) : n;
            const char *tok = s;

            while (tok_len > 0 && (tok[0] == ' ' || tok[0] == '\t')) {
                tok++;
                tok_len--;
            }
            while (tok_len > 0 && (tok[tok_len - 1] == ' ' ||
                                   tok[tok_len - 1] == '\t'))
                tok_len--;

            if (len > 0 && tok_len == len) {
                size_t j = 0;
                while (j < len &&
                       tolower((unsigned char)tok[j]) ==
                           tolower((unsigned char)name[j]))
                    j++;
                if (j == len)
                    return 1;
            }

            if (comma == NULL)
                break;
            size_t used = (size_t)(comma - s) + 1;
            s += used;
            n -= used;
        }
    }
    return 0;
}

void proxy_build_request(conn_t *c, const http_request_t *req,
                         proxy_conn_t *p) {
    proxy_buf_t b = {0};
    char target[HTTP_MAX_URI * 2 + 2];
    int tn = -1;

    if (p->route->strip_prefix) {
        const char *prefix = p->route->prefix;
        size_t plen = strlen(prefix);
        const char *rest = req->path;
        if (strncmp(req->path, prefix, plen) == 0)
            rest = req->path + plen;
        if (*rest == '\0')
            rest = "/";
        if (*rest == '/')
            tn = snprintf(target, sizeof target, "%s", rest);
        else
            tn = snprintf(target, sizeof target, "/%s", rest);
        if (tn >= 0 && (size_t)tn < sizeof target && req->query[0] != '\0') {
            size_t used = (size_t)tn;
            tn = snprintf(target + used, sizeof target - used,
                          "?%s", req->query);
            if (tn >= 0)
                tn += (int)used;
        }
    } else {
        tn = snprintf(target, sizeof target, "%s",
                      req->target[0] != '\0' ? req->target : req->path);
    }
    if (tn < 0 || (size_t)tn >= sizeof target)
        goto fail;

    if (buf_printf(&b, "%s %s HTTP/1.1\r\n",
                   req->method_str, target) < 0)
        goto fail;

    const char *host = NULL;
    size_t host_len = 0;
    const char *xff = NULL;
    size_t xff_len = 0;

    for (size_t i = 0; i < req->header_count; i++) {
        const http_header_t *h = &req->headers[i];
        if (ci_equal(h->name, h->name_len, "Host")) {
            host = h->value;
            host_len = h->value_len;
        } else if (ci_equal(h->name, h->name_len, "X-Forwarded-For")) {
            xff = h->value;
            xff_len = h->value_len;
        }
    }

    for (size_t i = 0; i < req->header_count; i++) {
        const http_header_t *h = &req->headers[i];
        if (is_hop_by_hop(h->name, h->name_len))
            continue;
        if (ci_equal(h->name, h->name_len, "Host") ||
            ci_equal(h->name, h->name_len, "Content-Length") ||
            ci_equal(h->name, h->name_len, "Transfer-Encoding") ||
            ci_equal(h->name, h->name_len, "Expect") ||
            ci_equal(h->name, h->name_len, "X-Forwarded-For") ||
            ci_equal(h->name, h->name_len, "X-Forwarded-Proto") ||
            ci_equal(h->name, h->name_len, "X-Forwarded-Host"))
            continue;
        if (connection_names(req, h->name, h->name_len))
            continue;
        if (buf_printf(&b, "%.*s: %.*s\r\n",
                       (int)h->name_len, h->name,
                       (int)h->value_len, h->value) < 0)
            goto fail;
    }

    if (host != NULL) {
        if (buf_printf(&b, "Host: %.*s\r\n",
                       (int)host_len, host) < 0)
            goto fail;
    } else {
        if (buf_printf(&b, "Host: %s:%d\r\n",
                       p->backend->host, p->backend->port) < 0)
            goto fail;
    }

    char ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &c->peer.sin_addr, ip, sizeof ip) == NULL)
        snprintf(ip, sizeof ip, "?");
    if (xff != NULL) {
        if (buf_printf(&b, "X-Forwarded-For: %.*s, %s\r\n",
                       (int)xff_len, xff, ip) < 0)
            goto fail;
    } else {
        if (buf_printf(&b, "X-Forwarded-For: %s\r\n", ip) < 0)
            goto fail;
    }
    if (buf_puts(&b, "X-Forwarded-Proto: http\r\n") < 0)
        goto fail;
    if (host != NULL) {
        if (buf_printf(&b, "X-Forwarded-Host: %.*s\r\n",
                       (int)host_len, host) < 0)
            goto fail;
    }

    if (req->chunked) {
        if (buf_puts(&b, "Transfer-Encoding: chunked\r\n") < 0)
            goto fail;
    } else if (req->has_content_length) {
        if (buf_printf(&b, "Content-Length: %zu\r\n",
                       req->content_length) < 0)
            goto fail;
    }

    if (buf_puts(&b, "Connection: keep-alive\r\n\r\n") < 0)
        goto fail;

    p->req_out = b.data;
    p->req_out_len = b.len;
    return;

fail:
    free(b.data);
}

void proxy_build_response(proxy_conn_t *p) {
    proxy_buf_t b = {0};
    const char *reason = p->resp.reason[0] != '\0'
                             ? p->resp.reason
                             : http_status_reason(p->status);

    if (buf_printf(&b, "HTTP/1.1 %d %s\r\n", p->status, reason) < 0)
        goto fail;

    for (size_t i = 0; i < p->resp.header_count; i++) {
        const upstream_header_t *h = &p->resp.headers[i];
        if (p->resp_body == BODY_CHUNKED &&
            ci_equal(h->name, h->name_len, "Trailer")) {
            if (buf_printf(&b, "%.*s: %.*s\r\n",
                           (int)h->name_len, h->name,
                           (int)h->value_len, h->value) < 0)
                goto fail;
            continue;
        }
        if (is_hop_by_hop(h->name, h->name_len))
            continue;
        if (response_connection_names(p, h->name, h->name_len))
            continue;
        if (ci_equal(h->name, h->name_len, "Content-Length") ||
            ci_equal(h->name, h->name_len, "Transfer-Encoding") ||
            ci_equal(h->name, h->name_len, "Trailer") ||
            ci_equal(h->name, h->name_len, "Connection") ||
            ci_equal(h->name, h->name_len, "Keep-Alive") ||
            ci_equal(h->name, h->name_len, "Proxy-Authenticate") ||
            ci_equal(h->name, h->name_len, "Proxy-Authorization") ||
            ci_equal(h->name, h->name_len, "TE") ||
            ci_equal(h->name, h->name_len, "Upgrade"))
            continue;
        if (buf_printf(&b, "%.*s: %.*s\r\n",
                       (int)h->name_len, h->name,
                       (int)h->value_len, h->value) < 0)
            goto fail;
    }

    if (p->resp_body == BODY_CONTENT_LENGTH) {
        if (buf_printf(&b, "Content-Length: %zu\r\n",
                       p->resp.content_length) < 0)
            goto fail;
    } else if (p->resp_body == BODY_CHUNKED) {
        if (buf_puts(&b, "Transfer-Encoding: chunked\r\n") < 0)
            goto fail;
    } else if (p->resp_body == BODY_NONE && p->status == 205) {
        if (buf_puts(&b, "Content-Length: 0\r\n") < 0)
            goto fail;
    } else if (p->resp_body == BODY_NONE && p->status != 204 &&
               p->resp.has_content_length) {
        if (buf_printf(&b, "Content-Length: %zu\r\n",
                       p->resp.content_length) < 0)
            goto fail;
    }

    if (p->close_client || p->resp_body == BODY_CLOSE) {
        if (buf_puts(&b, "Connection: close\r\n") < 0)
            goto fail;
    } else {
        if (buf_puts(&b, "Connection: keep-alive\r\n") < 0)
            goto fail;
    }

    if (buf_puts(&b, "\r\n") < 0)
        goto fail;

    p->resp_out = b.data;
    p->resp_out_len = b.len;
    return;

fail:
    free(b.data);
    p->resp_out = NULL;
    p->resp_out_len = 0;
}
