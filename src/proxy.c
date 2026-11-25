#include "proxy.h"

#include "chunked.h"
#include "event_backend.h"
#include "http_upstream.h"
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROXY_BUF_SIZE 65536
#define HEALTH_COOLDOWN_MS 1000

typedef enum {
    BODY_NONE,
    BODY_CONTENT_LENGTH,
    BODY_CHUNKED,
    BODY_CLOSE
} body_mode_t;

typedef struct proxy_idle {
    int fd;
    uint64_t idle_since_ms;
    struct proxy_idle *next;
} proxy_idle_t;

typedef struct {
    char host[MAX_BACKEND_HOST];
    int port;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint64_t down_until_ms;
    proxy_idle_t *idle_head;
    int idle_count;
} proxy_backend_t;

typedef struct {
    char prefix[MAX_ROUTE_PREFIX];
    route_type_t type;
    int strip_prefix;
    proxy_backend_t backends[MAX_BACKENDS];
    int backend_count;
    int rr_index;
} proxy_route_t;

typedef enum {
    PROXY_CONNECTING,
    PROXY_SENDING,
    PROXY_READING_HEADERS,
    PROXY_RELAY_BODY,
    PROXY_DONE
} proxy_state_t;

struct proxy_conn {
    ev_source_t source; /* must stay first */

    conn_t *client;
    int upstream_fd;
    int upstream_in_loop;
    proxy_state_t state;
    proxy_route_t *route;
    proxy_backend_t *backend;

    char *req_out;
    size_t req_out_len;
    size_t req_out_sent;

    char *resp_out;
    size_t resp_out_len;
    size_t resp_out_sent;

    char *resp_hdr;
    size_t resp_hdr_len;
    size_t resp_hdr_cap;
    upstream_response_t resp;

    body_mode_t req_body;
    size_t req_remaining;
    chunk_parser_t req_chunk;
    int req_body_done;

    body_mode_t resp_body;
    size_t resp_remaining;
    chunk_parser_t resp_chunk;
    int resp_body_done;

    char c2u[PROXY_BUF_SIZE];
    size_t c2u_len;
    size_t c2u_off;

    char u2c[PROXY_BUF_SIZE];
    size_t u2c_len;
    size_t u2c_off;

    int head_only;
    int client_keep_alive;
    int close_client;
    int response_started;
    int upstream_keep_alive;
    int upstream_eof;
    int continue_pending;
    size_t continue_sent;
    int dead;
    int status;
    size_t body_bytes;
    uint32_t tried_mask;

    uint64_t deadline_ms;
    struct proxy_conn *next;
    struct proxy_conn *next_dead;
};

static proxy_route_t g_routes[MAX_ROUTES];
static int g_route_count;
static const config_t *g_cfg;
static proxy_conn_t *g_active;
static proxy_conn_t *g_dead;
static int g_timeout_connect_ms;
static int g_timeout_response_ms;
static int g_timeout_body_ms;
static int g_pool_max_idle;
static int g_pool_idle_timeout_ms;
static int g_max_upstreams;
static int g_active_upstreams;

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

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int proxy_set_events(conn_t *c, int fd, uint32_t events, void *ptr,
                            int *in_loop) {
    if (events == 0) {
        if (*in_loop) {
            (void)ev_del(c->loop, fd);
            *in_loop = 0;
        }
        return 0;
    }
    if (*in_loop)
        return ev_mod(c->loop, fd, events, ptr);
    int rc = ev_add(c->loop, fd, events, ptr);
    if (rc == 0)
        *in_loop = 1;
    return rc;
}

static void proxy_list_add(proxy_conn_t *p) {
    p->next = g_active;
    g_active = p;
}

static void proxy_list_remove(proxy_conn_t *p) {
    proxy_conn_t **pp = &g_active;
    while (*pp != NULL) {
        if (*pp == p) {
            *pp = p->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void proxy_close_upstream(proxy_conn_t *p) {
    if (p->upstream_fd >= 0) {
        if (p->upstream_in_loop && p->client != NULL) {
            (void)ev_del(p->client->loop, p->upstream_fd);
            p->upstream_in_loop = 0;
        }
        close(p->upstream_fd);
        p->upstream_fd = -1;
    }
}

static void proxy_free_struct(proxy_conn_t *p) {
    if (p == NULL || p->dead)
        return;
    if (p->client != NULL && p->client->proxy == p)
        p->client->proxy = NULL;
    proxy_list_remove(p);
    if (g_active_upstreams > 0)
        g_active_upstreams--;
    p->dead = 1;
    p->next_dead = g_dead;
    g_dead = p;
}

void proxy_reap(void) {
    while (g_dead != NULL) {
        proxy_conn_t *p = g_dead;
        g_dead = p->next_dead;
        free(p->req_out);
        free(p->resp_out);
        free(p->resp_hdr);
        free(p);
    }
}

static void proxy_abort(proxy_conn_t *p) {
    if (p == NULL || p->dead)
        return;
    conn_t *c = p->client;
    if (p->backend != NULL)
        log_error("proxy: %s:%d aborted (state=%d)",
                  p->backend->host, p->backend->port, p->state);
    proxy_close_upstream(p);
    proxy_free_struct(p);
    if (c != NULL)
        server_conn_close(c);
}

static void proxy_fail(proxy_conn_t *p, int status) {
    if (p == NULL || p->dead)
        return;
    conn_t *c = p->client;
    int started = p->response_started;
    if (p->backend != NULL)
        log_error("proxy: %s:%d failed with %d (state=%d)",
                  p->backend->host, p->backend->port, status, p->state);
    proxy_close_upstream(p);

    if (!started && c != NULL) {
        int rc = server_conn_build_error(c, status, 1, p->head_only, 0);
        proxy_free_struct(p);
        if (rc == 0) {
            if (proxy_set_events(c, c->fd, EV_WRITE, c, &c->in_loop) < 0)
                server_conn_close(c);
            return;
        }
    }

    proxy_free_struct(p);
    if (c != NULL)
        server_conn_close(c);
}

static proxy_backend_t *select_backend(proxy_route_t *r, uint32_t skip_mask,
                                       int *out_index) {
    uint64_t now = now_ms();
    for (int i = 0; i < r->backend_count; i++) {
        int idx = (r->rr_index + i) % r->backend_count;
        if (skip_mask & (1u << idx))
            continue;
        proxy_backend_t *b = &r->backends[idx];
        if (b->down_until_ms <= now) {
            r->rr_index = (idx + 1) % r->backend_count;
            if (out_index != NULL)
                *out_index = idx;
            return b;
        }
    }
    return NULL;
}

static int pool_acquire(proxy_backend_t *b) {
    proxy_idle_t **pp = &b->idle_head;
    while (*pp != NULL) {
        proxy_idle_t *idle = *pp;
        char byte;
        ssize_t n = recv(idle->fd, &byte, 1, MSG_PEEK);
        if (n == 0 || n > 0 ||
            (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
             errno != EINTR)) {
            *pp = idle->next;
            b->idle_count--;
            close(idle->fd);
            free(idle);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;

        *pp = idle->next;
        b->idle_count--;
        int fd = idle->fd;
        free(idle);
        return fd;
    }
    return -1;
}

static void pool_release(proxy_backend_t *b, int fd) {
    if (g_pool_max_idle <= 0 || b->idle_count >= g_pool_max_idle) {
        close(fd);
        return;
    }

    proxy_idle_t *idle = malloc(sizeof *idle);
    if (idle == NULL) {
        close(fd);
        return;
    }
    idle->fd = fd;
    idle->idle_since_ms = now_ms();
    idle->next = b->idle_head;
    b->idle_head = idle;
    b->idle_count++;
}

static void pool_close_backend(proxy_backend_t *b) {
    proxy_idle_t *idle = b->idle_head;
    while (idle != NULL) {
        proxy_idle_t *next = idle->next;
        close(idle->fd);
        free(idle);
        idle = next;
    }
    b->idle_head = NULL;
    b->idle_count = 0;
}

static void proxy_build_request(conn_t *c, const http_request_t *req,
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

static void proxy_build_response(proxy_conn_t *p) {
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

static void proxy_update_events(proxy_conn_t *p);

static void proxy_flush_upstream(proxy_conn_t *p) {
    while (p->req_out_sent < p->req_out_len) {
        ssize_t n = write(p->upstream_fd, p->req_out + p->req_out_sent,
                          p->req_out_len - p->req_out_sent);
        if (n > 0) {
            p->req_out_sent += (size_t)n;
            p->deadline_ms = now_ms() +
                (uint64_t)g_cfg->request_timeout_sec * 1000;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            proxy_update_events(p);
            return;
        }
        proxy_fail(p, 502);
        return;
    }

    while (p->c2u_off < p->c2u_len) {
        ssize_t n = write(p->upstream_fd, p->c2u + p->c2u_off,
                          p->c2u_len - p->c2u_off);
        if (n > 0) {
            p->c2u_off += (size_t)n;
            p->deadline_ms = now_ms() +
                (uint64_t)g_cfg->request_timeout_sec * 1000;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            proxy_update_events(p);
            return;
        }
        proxy_fail(p, 502);
        return;
    }

    p->c2u_len = 0;
    p->c2u_off = 0;
    proxy_update_events(p);
}

static void proxy_read_client_body(proxy_conn_t *p) {
    if (p->state != PROXY_SENDING || p->req_body_done)
        return;

    conn_t *c = p->client;
    if (p->c2u_off > 0) {
        memmove(p->c2u, p->c2u + p->c2u_off, p->c2u_len - p->c2u_off);
        p->c2u_len -= p->c2u_off;
        p->c2u_off = 0;
    }
    size_t space = sizeof p->c2u - p->c2u_len;

    if (p->req_body == BODY_CONTENT_LENGTH) {
        while (p->req_remaining > 0 && space > 0) {
            if (c->in_len > 0) {
                size_t take = c->in_len;
                if (take > p->req_remaining)
                    take = p->req_remaining;
                if (take > space)
                    take = space;
                memcpy(p->c2u + p->c2u_len, c->in, take);
                p->c2u_len += take;
                p->req_remaining -= take;
                space -= take;
                memmove(c->in, c->in + take, c->in_len - take);
                c->in_len -= take;
                continue;
            }

            size_t want = p->req_remaining < space ? p->req_remaining : space;
            ssize_t n = read(c->fd, p->c2u + p->c2u_len, want);
            if (n > 0) {
                p->c2u_len += (size_t)n;
                p->req_remaining -= (size_t)n;
                space -= (size_t)n;
                continue;
            }
            if (n == 0) {
                proxy_abort(p);
                return;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            proxy_abort(p);
            return;
        }
        if (p->req_remaining == 0)
            p->req_body_done = 1;
    } else if (p->req_body == BODY_CHUNKED) {
        while (space > 0 && !p->req_body_done) {
            const char *src = NULL;
            size_t avail = 0;
            char tmp[4096];
            int from_client = 0;

            if (c->in_len > 0) {
                src = c->in;
                avail = c->in_len < space ? c->in_len : space;
            } else {
                size_t want = space < sizeof tmp ? space : sizeof tmp;
                ssize_t n = read(c->fd, tmp, want);
                if (n > 0) {
                    src = tmp;
                    avail = (size_t)n;
                    from_client = 1;
                } else if (n == 0) {
                    proxy_abort(p);
                    return;
                } else if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    proxy_abort(p);
                    return;
                }
            }

            size_t consumed = chunk_parser_feed(&p->req_chunk, src, avail);
            if (consumed > avail)
                consumed = avail;
            if (consumed > 0) {
                memcpy(p->c2u + p->c2u_len, src, consumed);
                p->c2u_len += consumed;
                space -= consumed;
            }
            if (from_client) {
                if (consumed < avail)
                    p->close_client = 1;
            } else if (consumed > 0) {
                memmove(c->in, c->in + consumed, c->in_len - consumed);
                c->in_len -= consumed;
            }
            if (chunk_parser_error(&p->req_chunk)) {
                proxy_fail(p, 400);
                return;
            }
            if (p->req_chunk.data_bytes > g_cfg->max_body) {
                proxy_fail(p, HTTP_PAYLOAD_TOO_LARGE);
                return;
            }
            if (chunk_parser_done(&p->req_chunk)) {
                p->req_body_done = 1;
                break;
            }
            if (consumed == 0)
                break;
        }
    }

    proxy_flush_upstream(p);
}

static void proxy_read_upstream_headers(proxy_conn_t *p) {
    if (p->resp_hdr == NULL) {
        size_t cap = g_cfg->max_header_bytes + 1;
        if (cap < 1024)
            cap = 1024;
        p->resp_hdr = malloc(cap);
        if (p->resp_hdr == NULL) {
            proxy_fail(p, 500);
            return;
        }
        p->resp_hdr_cap = cap;
    }

    while (1) {
        if (p->resp_hdr_len > 0) {
            p->resp_hdr[p->resp_hdr_len] = '\0';
            upstream_response_init(&p->resp);
            http_status_t st = upstream_response_parse(
                p->resp_hdr, p->resp_hdr_len, g_cfg, &p->resp);
            if (st == HTTP_OK) {
                if (p->resp.status >= 100 && p->resp.status < 200) {
                    size_t used = p->resp.header_len;
                    if (used > p->resp_hdr_len) {
                        proxy_fail(p, 502);
                        return;
                    }
                    memmove(p->resp_hdr, p->resp_hdr + used,
                            p->resp_hdr_len - used);
                    p->resp_hdr_len -= used;
                    /* Parse the remaining buffer before reading again. */
                    continue;
                }
                break; /* final response headers parsed */
            }
            if (st != HTTP_PARSE_INCOMPLETE) {
                proxy_fail(p, 502);
                return;
            }
            if (p->upstream_eof) {
                proxy_fail(p, 502);
                return;
            }
        } else if (p->upstream_eof) {
            proxy_fail(p, 502);
            return;
        }

        if (p->resp_hdr_len >= p->resp_hdr_cap - 1) {
            proxy_fail(p, 502);
            return;
        }

        ssize_t n = read(p->upstream_fd, p->resp_hdr + p->resp_hdr_len,
                         p->resp_hdr_cap - 1 - p->resp_hdr_len);
        if (n > 0) {
            p->resp_hdr_len += (size_t)n;
            continue;
        }
        if (n == 0) {
            p->upstream_eof = 1;
            continue;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        proxy_fail(p, 502);
        return;
    }

        p->status = p->resp.status;
        p->response_started = 1;
        p->resp_body_done = 0;

        if (p->head_only || p->status == 204 || p->status == 304 ||
            p->status == 205) {
            p->resp_body = BODY_NONE;
            p->resp_body_done = 1;
        } else if (p->resp.chunked) {
            p->resp_body = BODY_CHUNKED;
            chunk_parser_init(&p->resp_chunk);
        } else if (p->resp.has_content_length) {
            p->resp_body = BODY_CONTENT_LENGTH;
            p->resp_remaining = p->resp.content_length;
            if (p->resp_remaining == 0)
                p->resp_body_done = 1;
        } else {
            p->resp_body = BODY_CLOSE;
            p->close_client = 1;
        }

        if (!p->req_body_done) {
            p->req_body_done = 1;
            p->close_client = 1;
            p->upstream_keep_alive = 0;
            p->c2u_len = 0;
            p->c2u_off = 0;
        }

        p->client_keep_alive = p->client_keep_alive &&
                               !server_is_stopping();
        p->close_client = p->close_client || !p->client_keep_alive ||
                          p->resp_body == BODY_CLOSE;
        int http11 = p->resp.version_major == 1 &&
                     p->resp.version_minor >= 1;
        p->upstream_keep_alive = !p->resp.connection_close &&
                                 p->resp_body != BODY_CLOSE &&
                                 (http11 || p->resp.connection_keep_alive);

        proxy_build_response(p);
        if (p->resp_out == NULL) {
            proxy_fail(p, 500);
            return;
        }

        size_t extra = p->resp_hdr_len - p->resp.header_len;
        if (extra > 0) {
            if (p->resp_body == BODY_NONE || extra > sizeof p->u2c) {
                proxy_fail(p, 502);
                return;
            }

            size_t decoded_delta = 0;
            if (p->resp_body == BODY_CONTENT_LENGTH) {
                if (extra > p->resp_remaining) {
                    proxy_fail(p, 502);
                    return;
                }
                p->resp_remaining -= extra;
                if (p->resp_remaining == 0)
                    p->resp_body_done = 1;
            } else if (p->resp_body == BODY_CHUNKED) {
                size_t before = p->resp_chunk.data_bytes;
                size_t consumed = chunk_parser_feed(
                    &p->resp_chunk, p->resp_hdr + p->resp.header_len, extra);
                if (consumed != extra || chunk_parser_error(&p->resp_chunk)) {
                    proxy_fail(p, 502);
                    return;
                }
                decoded_delta = p->resp_chunk.data_bytes - before;
                if (chunk_parser_done(&p->resp_chunk))
                    p->resp_body_done = 1;
            }

            memcpy(p->u2c, p->resp_hdr + p->resp.header_len, extra);
            p->u2c_len = extra;
            p->body_bytes += p->resp_body == BODY_CHUNKED
                                 ? decoded_delta : extra;
        }

        if (p->upstream_eof && p->resp_body != BODY_CLOSE &&
            !p->resp_body_done) {
            proxy_fail(p, 502);
            return;
        }

        p->state = PROXY_RELAY_BODY;
        p->deadline_ms = now_ms() + (uint64_t)g_timeout_body_ms;
        proxy_update_events(p);
        return;
}

static void proxy_flush_client(proxy_conn_t *p);
static void proxy_flush_continue(proxy_conn_t *p);

static void proxy_read_upstream_body(proxy_conn_t *p) {
    if (p->resp_body_done) {
        proxy_flush_client(p);
        return;
    }

    if (p->u2c_off > 0) {
        memmove(p->u2c, p->u2c + p->u2c_off, p->u2c_len - p->u2c_off);
        p->u2c_len -= p->u2c_off;
        p->u2c_off = 0;
    }
    if (p->u2c_len == sizeof p->u2c) {
        proxy_flush_client(p);
        return;
    }
    size_t space = sizeof p->u2c - p->u2c_len;

    if (p->resp_body == BODY_CONTENT_LENGTH) {
        size_t want = p->resp_remaining < space ? p->resp_remaining : space;
        ssize_t n = read(p->upstream_fd, p->u2c + p->u2c_len, want);
        if (n > 0) {
            p->u2c_len += (size_t)n;
            p->resp_remaining -= (size_t)n;
            p->body_bytes += (size_t)n;
            p->deadline_ms = now_ms() + (uint64_t)g_timeout_body_ms;
            if (p->resp_remaining == 0)
                p->resp_body_done = 1;
        } else if (n == 0) {
            proxy_fail(p, 502);
            return;
        } else if (errno == EINTR) {
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else {
            proxy_fail(p, 502);
            return;
        }
    } else if (p->resp_body == BODY_CHUNKED) {
        char tmp[4096];
        size_t want = space < sizeof tmp ? space : sizeof tmp;
        ssize_t n = read(p->upstream_fd, tmp, want);
        if (n > 0) {
            size_t before = p->resp_chunk.data_bytes;
            size_t consumed = chunk_parser_feed(&p->resp_chunk, tmp, (size_t)n);
            if (consumed > 0) {
                memcpy(p->u2c + p->u2c_len, tmp, consumed);
                p->u2c_len += consumed;
                p->body_bytes += p->resp_chunk.data_bytes - before;
                p->deadline_ms = now_ms() + (uint64_t)g_timeout_body_ms;
            }
            if (consumed < (size_t)n) {
                p->upstream_keep_alive = 0;
                p->close_client = 1;
            }
            if (chunk_parser_error(&p->resp_chunk)) {
                proxy_fail(p, 502);
                return;
            }
            if (chunk_parser_done(&p->resp_chunk))
                p->resp_body_done = 1;
        } else if (n == 0) {
            proxy_fail(p, 502);
            return;
        } else if (errno == EINTR) {
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else {
            proxy_fail(p, 502);
            return;
        }
    } else if (p->resp_body == BODY_CLOSE) {
        ssize_t n = read(p->upstream_fd, p->u2c + p->u2c_len, space);
        if (n > 0) {
            p->u2c_len += (size_t)n;
            p->body_bytes += (size_t)n;
            p->deadline_ms = now_ms() + (uint64_t)g_timeout_body_ms;
        } else if (n == 0) {
            p->resp_body_done = 1;
        } else if (errno == EINTR) {
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else {
            proxy_fail(p, 502);
            return;
        }
    }

    proxy_flush_client(p);
}

static void proxy_drain_eof(proxy_conn_t *p) {
    while (!p->dead && !p->resp_body_done &&
           p->u2c_len < sizeof p->u2c) {
        size_t before = p->body_bytes;
        proxy_read_upstream_body(p);
        if (p->dead || p->resp_body_done)
            return;
        if (p->u2c_len == sizeof p->u2c)
            return;
        if (p->body_bytes == before)
            return;
    }
}

static void proxy_finish(proxy_conn_t *p) {
    conn_t *c = p->client;
    int keep = p->client_keep_alive && !p->close_client &&
               !server_is_stopping();

    server_conn_log_response(c, p->status, p->body_bytes);
    server_conn_log_end(c);

    if (p->upstream_keep_alive && p->upstream_fd >= 0) {
        if (p->upstream_in_loop) {
            (void)ev_del(c->loop, p->upstream_fd);
            p->upstream_in_loop = 0;
        }
        int fd = p->upstream_fd;
        p->upstream_fd = -1;
        pool_release(p->backend, fd);
    } else {
        proxy_close_upstream(p);
    }

    proxy_free_struct(p);

    if (keep) {
        if (c->in_len > 0) {
            server_conn_close(c);
            return;
        }
        c->state = ST_READ;
        c->close_after = 0;
        c->deadline_ms = now_ms() +
            (uint64_t)(c->in_len == 0 ? c->cfg->keepalive_idle_sec
                                      : c->cfg->request_timeout_sec) * 1000;
        if (proxy_set_events(c, c->fd, EV_READ, c, &c->in_loop) < 0)
            server_conn_close(c);
    } else {
        server_conn_close(c);
    }
}

static void proxy_flush_continue(proxy_conn_t *p) {
    static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
    size_t total = sizeof cont - 1;

    while (p->continue_sent < total) {
        ssize_t n = write(p->client->fd, cont + p->continue_sent,
                          total - p->continue_sent);
        if (n > 0) {
            p->continue_sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            proxy_update_events(p);
            return;
        }
        proxy_abort(p);
        return;
    }

    p->continue_pending = 0;
    proxy_update_events(p);
}

static void proxy_flush_client(proxy_conn_t *p) {
    conn_t *c = p->client;

    if (p->continue_pending) {
        proxy_flush_continue(p);
        if (p->dead || p->continue_pending)
            return;
    }

    while (p->resp_out_sent < p->resp_out_len) {
        ssize_t n = write(c->fd, p->resp_out + p->resp_out_sent,
                          p->resp_out_len - p->resp_out_sent);
        if (n > 0) {
            p->resp_out_sent += (size_t)n;
            p->deadline_ms = now_ms() + (uint64_t)g_timeout_body_ms;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            proxy_update_events(p);
            return;
        }
        proxy_abort(p);
        return;
    }

    while (p->u2c_off < p->u2c_len) {
        ssize_t n = write(c->fd, p->u2c + p->u2c_off, p->u2c_len - p->u2c_off);
        if (n > 0) {
            p->u2c_off += (size_t)n;
            p->deadline_ms = now_ms() + (uint64_t)g_timeout_body_ms;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            proxy_update_events(p);
            return;
        }
        proxy_abort(p);
        return;
    }

    p->u2c_len = 0;
    p->u2c_off = 0;

    if (p->upstream_eof && p->state == PROXY_RELAY_BODY &&
        !p->resp_body_done) {
        proxy_drain_eof(p);
        if (p->dead)
            return;
    }

    if (p->resp_out_sent == p->resp_out_len && p->u2c_len == 0 &&
        p->resp_body_done) {
        proxy_finish(p);
        return;
    }

    proxy_update_events(p);
}

static void proxy_update_events(proxy_conn_t *p) {
    if (p == NULL || p->dead || p->client == NULL || p->client->closed) {
        proxy_abort(p);
        return;
    }

    if (p->state == PROXY_SENDING &&
        p->req_out_sent == p->req_out_len &&
        p->c2u_len == 0 && p->req_body_done) {
        p->state = PROXY_READING_HEADERS;
        p->deadline_ms = now_ms() + (uint64_t)g_timeout_response_ms;
    }

    conn_t *c = p->client;
    uint32_t cev = 0;
    if (p->continue_pending)
        cev |= EV_WRITE;
    if (p->state == PROXY_SENDING && !p->req_body_done &&
        p->c2u_len < sizeof p->c2u)
        cev |= EV_READ;
    if (p->resp_out_sent < p->resp_out_len ||
        p->u2c_len > p->u2c_off)
        cev |= EV_WRITE;

    if (proxy_set_events(c, c->fd, cev, c, &c->in_loop) < 0) {
        proxy_abort(p);
        return;
    }

    uint32_t uev = 0;
    if (p->upstream_fd >= 0) {
        if (p->state == PROXY_CONNECTING) {
            uev = EV_WRITE;
        } else if (p->state == PROXY_SENDING) {
            if (p->req_out_sent < p->req_out_len ||
                p->c2u_len > p->c2u_off)
                uev = EV_WRITE;
        } else if (p->state == PROXY_READING_HEADERS) {
            uev = EV_READ;
        } else if (p->state == PROXY_RELAY_BODY) {
            if (!p->resp_body_done && p->u2c_len < sizeof p->u2c)
                uev |= EV_READ;
            if (p->c2u_len > p->c2u_off)
                uev |= EV_WRITE;
        }
        if (proxy_set_events(c, p->upstream_fd, uev, p,
                             &p->upstream_in_loop) < 0) {
            proxy_abort(p);
            return;
        }
    }
}

void proxy_kick(conn_t *c) {
    if (c == NULL || c->proxy == NULL)
        return;
    proxy_conn_t *p = c->proxy;
    if (p->state == PROXY_SENDING && !p->req_body_done &&
        c->in_len > 0) {
        proxy_read_client_body(p);
    } else {
        proxy_update_events(p);
    }
}

void proxy_mark_close(conn_t *c) {
    if (c != NULL && c->proxy != NULL)
        c->proxy->close_client = 1;
}

int proxy_init(const config_t *cfg) {
    if (cfg == NULL)
        return -1;

    memset(g_routes, 0, sizeof g_routes);
    g_route_count = 0;
    g_cfg = cfg;
    g_timeout_connect_ms = cfg->timeout_connect_sec * 1000;
    g_timeout_response_ms = cfg->timeout_response_sec * 1000;
    g_timeout_body_ms = cfg->timeout_body_sec * 1000;
    g_pool_max_idle = cfg->pool_max_idle;
    g_pool_idle_timeout_ms = cfg->pool_idle_timeout_sec * 1000;
    g_max_upstreams = cfg->max_upstreams;
    g_active_upstreams = 0;

    for (int i = 0; i < cfg->route_count; i++) {
        const route_spec_t *spec = &cfg->routes[i];
        proxy_route_t *r = &g_routes[i];
        snprintf(r->prefix, sizeof r->prefix, "%s", spec->prefix);
        r->type = spec->type;
        r->strip_prefix = spec->strip_prefix;
        r->backend_count = spec->backend_count;

        for (int j = 0; j < spec->backend_count; j++) {
            proxy_backend_t *b = &r->backends[j];
            snprintf(b->host, sizeof b->host, "%s", spec->backends[j].host);
            b->port = spec->backends[j].port;

            char portbuf[16];
            snprintf(portbuf, sizeof portbuf, "%d", b->port);
            struct addrinfo hints;
            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_NUMERICSERV;
            struct addrinfo *res = NULL;
            int rc = getaddrinfo(b->host, portbuf, &hints, &res);
            if (rc != 0 || res == NULL) {
                fprintf(stderr, "proxy: resolve %s:%d: %s\n",
                        b->host, b->port, gai_strerror(rc));
                proxy_cleanup();
                return -1;
            }
            memcpy(&b->addr, res->ai_addr, res->ai_addrlen);
            b->addr_len = res->ai_addrlen;
            freeaddrinfo(res);
        }
        g_route_count++;
    }

    return 0;
}

void proxy_cleanup(void) {
    for (int i = 0; i < g_route_count; i++) {
        for (int j = 0; j < g_routes[i].backend_count; j++)
            pool_close_backend(&g_routes[i].backends[j]);
    }
    g_route_count = 0;
    g_active = NULL;
    g_active_upstreams = 0;
    g_cfg = NULL;
    proxy_reap();
}

void proxy_sweep(void) {
    uint64_t now = now_ms();

    for (int i = 0; i < g_route_count; i++) {
        for (int j = 0; j < g_routes[i].backend_count; j++) {
            proxy_backend_t *b = &g_routes[i].backends[j];
            proxy_idle_t **pp = &b->idle_head;
            while (*pp != NULL) {
                proxy_idle_t *idle = *pp;
                if (now - idle->idle_since_ms >
                    (uint64_t)g_pool_idle_timeout_ms) {
                    *pp = idle->next;
                    b->idle_count--;
                    close(idle->fd);
                    free(idle);
                } else {
                    pp = &idle->next;
                }
            }
        }
    }

    proxy_conn_t *p = g_active;
    while (p != NULL) {
        proxy_conn_t *next = p->next;
        if (p->deadline_ms != 0 && now >= p->deadline_ms) {
            if (p->state == PROXY_CONNECTING && p->backend != NULL) {
                p->backend->down_until_ms =
                    now + HEALTH_COOLDOWN_MS;
                pool_close_backend(p->backend);
            }
            int status = (p->state == PROXY_SENDING && !p->req_body_done)
                             ? HTTP_REQUEST_TIMEOUT
                             : HTTP_GATEWAY_TIMEOUT;
            proxy_fail(p, status);
        }
        p = next;
    }
}

int proxy_next_timeout_ms(void) {
    uint64_t now = now_ms();
    uint64_t min = UINT64_MAX;

    for (proxy_conn_t *p = g_active; p != NULL; p = p->next) {
        if (p->deadline_ms != 0 && p->deadline_ms < min)
            min = p->deadline_ms;
    }

    for (int i = 0; i < g_route_count; i++) {
        for (int j = 0; j < g_routes[i].backend_count; j++) {
            proxy_backend_t *b = &g_routes[i].backends[j];
            for (proxy_idle_t *idle = b->idle_head; idle != NULL;
                 idle = idle->next) {
                uint64_t expire = idle->idle_since_ms +
                                  (uint64_t)g_pool_idle_timeout_ms;
                if (expire < min)
                    min = expire;
            }
        }
    }

    if (min == UINT64_MAX)
        return -1;
    if (min <= now)
        return 0;
    uint64_t d = min - now;
    return d > (uint64_t)0x7fffffff ? 0x7fffffff : (int)d;
}

static uint64_t proxy_request_deadline(const proxy_conn_t *p) {
    if (!p->req_body_done)
        return now_ms() + (uint64_t)g_cfg->request_timeout_sec * 1000;
    return now_ms() + (uint64_t)g_timeout_response_ms;
}

static int proxy_connect_backend(proxy_conn_t *p) {
    proxy_route_t *route = p->route;

    for (;;) {
        int idx = -1;
        proxy_backend_t *backend =
            select_backend(route, p->tried_mask, &idx);
        if (backend == NULL)
            return -1;

        p->backend = backend;
        p->tried_mask |= 1u << idx;

        int fd = pool_acquire(backend);
        if (fd >= 0) {
            p->upstream_fd = fd;
            p->state = PROXY_SENDING;
            p->deadline_ms = proxy_request_deadline(p);
            if (ev_add(p->client->loop, fd, EV_WRITE, p) < 0) {
                close(fd);
                p->upstream_fd = -1;
                return -1;
            }
            p->upstream_in_loop = 1;
            return 0;
        }

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        if (set_nonblocking(fd) < 0 || set_cloexec(fd) < 0) {
            close(fd);
            return -1;
        }

        int rc = connect(fd, (struct sockaddr *)&backend->addr,
                         backend->addr_len);
        if (rc == 0) {
            p->upstream_fd = fd;
            p->state = PROXY_SENDING;
            p->deadline_ms = proxy_request_deadline(p);
            if (ev_add(p->client->loop, fd, EV_WRITE, p) < 0) {
                close(fd);
                p->upstream_fd = -1;
                return -1;
            }
            p->upstream_in_loop = 1;
            return 0;
        }
        if (errno == EINPROGRESS) {
            p->upstream_fd = fd;
            p->state = PROXY_CONNECTING;
            p->deadline_ms = now_ms() + (uint64_t)g_timeout_connect_ms;
            if (ev_add(p->client->loop, fd, EV_WRITE, p) < 0) {
                close(fd);
                p->upstream_fd = -1;
                return -1;
            }
            p->upstream_in_loop = 1;
            return 0;
        }

        close(fd);
        backend->down_until_ms = now_ms() + HEALTH_COOLDOWN_MS;
        pool_close_backend(backend);
    }
}

int proxy_start(conn_t *c, int route_index, const http_request_t *req) {
    if (g_cfg == NULL || c == NULL || req == NULL ||
        route_index < 0 || route_index >= g_route_count)
        return -1;

    proxy_route_t *route = &g_routes[route_index];
    if (route->backend_count <= 0) {
        if (server_conn_build_error(c, HTTP_BAD_GATEWAY, 1,
                                    req->method == HTTP_HEAD, 0) < 0)
            return -1;
        if (ev_mod(c->loop, c->fd, EV_WRITE, c) < 0)
            return -1;
        return 0;
    }

    if (g_active_upstreams >= g_max_upstreams) {
        if (server_conn_build_error(c, HTTP_SERVICE_UNAVAILABLE, 1,
                                    req->method == HTTP_HEAD, 1) < 0)
            return -1;
        if (ev_mod(c->loop, c->fd, EV_WRITE, c) < 0)
            return -1;
        return 0;
    }

    proxy_conn_t *p = calloc(1, sizeof *p);
    if (p == NULL)
        return -1;

    p->source.kind = EV_SRC_UPSTREAM;
    p->client = c;
    p->upstream_fd = -1;
    p->route = route;
    p->backend = &route->backends[0];
    p->head_only = req->method == HTTP_HEAD;
    p->client_keep_alive = req->keep_alive &&
        (c->requests + 1 < g_cfg->keepalive_max_requests) &&
        !server_is_stopping();
    p->state = PROXY_CONNECTING;

    if (req->chunked) {
        p->req_body = BODY_CHUNKED;
        chunk_parser_init(&p->req_chunk);
    } else if (req->content_length > 0) {
        p->req_body = BODY_CONTENT_LENGTH;
        p->req_remaining = req->content_length;
    } else {
        p->req_body = BODY_NONE;
        p->req_body_done = 1;
    }

    proxy_build_request(c, req, p);
    if (p->req_out == NULL) {
        free(p);
        if (server_conn_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1,
                                    req->method == HTTP_HEAD, 0) < 0)
            return -1;
        if (ev_mod(c->loop, c->fd, EV_WRITE, c) < 0)
            return -1;
        return 0;
    }

    if (req->expect_continue) {
        p->continue_pending = 1;
        p->continue_sent = 0;
    }

    c->proxy = p;
    c->state = ST_PROXY;
    c->requests++;
    proxy_list_add(p);
    g_active_upstreams++;

    if (proxy_connect_backend(p) < 0) {
        proxy_fail(p, 502);
        return 0;
    }

    proxy_update_events(p);
    return 1;
}

void proxy_handle_upstream_event(proxy_conn_t *p, uint32_t events) {
    if (p == NULL || p->dead || p->upstream_fd < 0)
        return;

    if (p->state == PROXY_CONNECTING) {
        if (events & (EV_WRITE | EV_ERR)) {
            int err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(p->upstream_fd, SOL_SOCKET, SO_ERROR,
                           &err, &len) < 0 || err != 0) {
                p->backend->down_until_ms =
                    now_ms() + HEALTH_COOLDOWN_MS;
                pool_close_backend(p->backend);
                proxy_close_upstream(p);
                if (proxy_connect_backend(p) < 0) {
                    proxy_fail(p, 502);
                    return;
                }
                proxy_update_events(p);
                return;
            }
            p->state = PROXY_SENDING;
            p->deadline_ms = proxy_request_deadline(p);
            if (p->client->in_len > 0 && !p->req_body_done)
                proxy_read_client_body(p);
            else
                proxy_flush_upstream(p);
        }
        return;
    }

    if (events & EV_WRITE) {
        if (p->state == PROXY_SENDING || p->state == PROXY_RELAY_BODY)
            proxy_flush_upstream(p);
        if (p->dead)
            return;
    }

    int got_err = (events & EV_ERR) != 0;

    if ((events & EV_READ) || got_err) {
        if (p->state == PROXY_READING_HEADERS) {
            proxy_read_upstream_headers(p);
            if (p->dead)
                return;
        } else if (p->state == PROXY_RELAY_BODY) {
            if (got_err)
                proxy_drain_eof(p);
            else
                proxy_read_upstream_body(p);
            if (p->dead)
                return;
        } else if (p->state == PROXY_SENDING && got_err) {
            proxy_fail(p, 502);
            return;
        }
    }

    if (got_err && !p->dead) {
        p->upstream_eof = 1;
        if (p->state == PROXY_READING_HEADERS ||
            p->state == PROXY_SENDING) {
            proxy_fail(p, 502);
        } else if (p->state == PROXY_RELAY_BODY &&
                   !p->resp_body_done) {
            if (p->resp_body == BODY_CLOSE)
                proxy_drain_eof(p);
            else
                proxy_fail(p, 502);
        }
    }
}

void proxy_handle_client_event(conn_t *c, uint32_t events) {
    proxy_conn_t *p = c->proxy;
    if (p == NULL || p->dead)
        return;

    if (events & EV_ERR) {
        proxy_abort(p);
        return;
    }

    if (events & EV_READ) {
        if (p->state == PROXY_SENDING && !p->req_body_done) {
            proxy_read_client_body(p);
        } else {
            proxy_abort(p);
            return;
        }
    }

    if (c->proxy != NULL && (events & EV_WRITE)) {
        if (p->continue_pending)
            proxy_flush_continue(p);
        if (!p->dead && !p->continue_pending)
            proxy_flush_client(p);
    }
}

void proxy_detach(conn_t *c) {
    if (c == NULL || c->proxy == NULL)
        return;
    proxy_conn_t *p = c->proxy;
    proxy_close_upstream(p);
    proxy_free_struct(p);
}

void proxy_shutdown_all(void) {
    proxy_conn_t *p = g_active;
    while (p != NULL) {
        proxy_conn_t *next = p->next;
        conn_t *c = p->client;
        proxy_close_upstream(p);
        proxy_free_struct(p);
        if (c != NULL)
            server_conn_close(c);
        p = next;
    }

    for (int i = 0; i < g_route_count; i++) {
        for (int j = 0; j < g_routes[i].backend_count; j++)
            pool_close_backend(&g_routes[i].backends[j]);
    }
    proxy_reap();
}
