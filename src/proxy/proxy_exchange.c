#define _GNU_SOURCE
#include "proxy_exchange.h"
#include "connection.h"
#include "proxy_wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PX_BLOCK 8192
#define PX_CONNECT_MS 2000
#define PX_HEAD_MS 5000
#define PX_RELAY_MS 30000
#define PX_IDLE_MS 5000

enum px_state
{
    PX_CONNECT,
    PX_REQUEST,
    PX_HEAD,
    PX_BODY,
    PX_DONE
};
enum px_chunk_state
{
    PX_SIZE,
    PX_DATA,
    PX_CRLF,
    PX_TRAILERS
};

struct session_stream
{
    int fd;
    enum px_state state;
    enum px_chunk_state chunk;
    struct timespec deadline, cap;
    bool capped, started, is_head, keep_client, delivered;
    struct proxy_routes *routes;
    size_t first, next, selected;
    const struct http_request *request;
    unsigned int interim;
    struct proxy_head head;
    struct http_slice request_body;
    size_t request_pos;
    size_t remaining;
    char in[HTTP_REQUEST_HEAD_MAX];
    size_t in_len;
    /* Only one staged segment is sent at a time. */
    char out[HTTP_REQUEST_HEAD_MAX + 512];
    size_t out_len, out_pos;
    size_t payload_begin, payload_end;
};

static int set_deadline(struct session_stream *e, unsigned int ms)
{
    return connection_deadline_from_now(ms, e->capped ? &e->cap : NULL,
                                        &e->deadline);
}

static bool idle_usable(int fd)
{
    char byte;
    ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

static void close_idle(struct proxy_route *route)
{
    if (route->idle)
        close(route->idle - 1);
    route->idle = 0;
}

int proxy_expire_idle(struct proxy_routes *routes)
{
    int minimum = -1;
    for (size_t i = 0; i < routes->count; ++i)
    {
        struct proxy_route *route = &routes->entries[i];
        if (!route->idle)
            continue;
        int left = (int)connection_deadline_remaining_ms(&route->idle_deadline);
        if (left <= 0)
            close_idle(route);
        else if (minimum < 0 || left < minimum)
            minimum = left;
    }
    return minimum;
}

void proxy_close_idle(struct proxy_routes *routes)
{
    for (size_t i = 0; i < routes->count; ++i)
        close_idle(&routes->entries[i]);
}

static int connect_next(struct session_stream *e)
{
    while (e->next < e->routes->count &&
           connection_deadline_remaining_ms(&e->deadline) > 0)
    {
        size_t index = e->next;
        struct proxy_route *route = &e->routes->entries[index];
        e->next = route->next_peer == e->first ? e->routes->count : route->next_peer;
        e->out_len = proxy_request_head(e->request, route->host, e->out, sizeof(e->out));
        if (!e->out_len)
            return -1;

        int fd = -1;
        if (route->idle && connection_deadline_remaining_ms(&route->idle_deadline) > 0 &&
            idle_usable(route->idle - 1))
        {
            fd = route->idle - 1;
            route->idle = 0;
            e->state = PX_REQUEST;
        }
        else
        {
            close_idle(route);
            fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0)
                return -1;
            if (connect(fd, (const struct sockaddr *)&route->address, sizeof(route->address)) == 0)
                e->state = PX_REQUEST;
            else if (errno == EINPROGRESS)
                e->state = PX_CONNECT;
            else
            {
                close(fd);
                continue;
            }
        }
        e->fd = fd;
        e->selected = index;
        return e->state == PX_REQUEST ? set_deadline(e, PX_HEAD_MS) : 0;
    }
    return -1;
}

struct session_stream *proxy_exchange_create(struct proxy_routes *routes, size_t first,
                                             const struct http_request *req,
                                             bool is_head)
{
    struct session_stream *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->fd = -1;
    e->routes = routes;
    e->first = e->next = first;
    e->request = req;
    e->request_body = req->body;
    e->is_head = is_head;
    if (set_deadline(e, PX_CONNECT_MS) < 0 || connect_next(e) < 0)
        goto fail;
    return e;
fail:
    proxy_exchange_destroy(e);
    return NULL;
}

void proxy_exchange_destroy(struct session_stream *e)
{
    if (!e)
        return;
    if (e->fd >= 0)
    {
        struct proxy_route *route = &e->routes->entries[e->selected];
        /* The loop removes interest when the final response bytes are staged. */
        if (e->delivered && !e->capped && e->head.upstream_keep && !e->in_len &&
            !route->idle && idle_usable(e->fd) &&
            connection_deadline_from_now(PX_IDLE_MS, NULL, &route->idle_deadline) == 0)
            route->idle = e->fd + 1;
        else
            close(e->fd);
    }
    free(e);
}

int proxy_exchange_fd(const struct session_stream *e) { return e->fd; }
int proxy_exchange_status(const struct session_stream *e) { return e->head.status; }
bool proxy_exchange_started(const struct session_stream *e) { return e->started; }
bool proxy_exchange_keep_client(const struct session_stream *e) { return e->keep_client; }

bool proxy_exchange_has_output(const struct session_stream *e)
{
    return e->state >= PX_BODY && e->out_pos < e->out_len;
}

short proxy_exchange_interest(const struct session_stream *e)
{
    if (e->state <= PX_REQUEST)
        return POLLOUT;
    if (e->state != PX_DONE && !proxy_exchange_has_output(e))
        return POLLIN;
    return 0;
}

int64_t proxy_exchange_remaining_ms(const struct session_stream *e)
{
    return connection_deadline_remaining_ms(&e->deadline);
}

void proxy_exchange_cap_deadline(struct session_stream *e, const struct timespec *cap)
{
    e->capped = true;
    e->cap = *cap;
    if (e->deadline.tv_sec > cap->tv_sec ||
        (e->deadline.tv_sec == cap->tv_sec && e->deadline.tv_nsec > cap->tv_nsec))
        e->deadline = *cap;
}

/* Positive progress, zero would-block, negative failure. */
static ssize_t push(int fd, const char *data, size_t len)
{
    ssize_t n;
    do
    {
        n = send(fd, data, len, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return n ? n : -1;
}

static void consume(struct session_stream *e, size_t len)
{
    e->in_len -= len;
    memmove(e->in, e->in + len, e->in_len);
}

static void stage_payload(struct session_stream *e, size_t len, bool chunked)
{
    size_t begin = chunked ? (size_t)snprintf(e->out, 32, "%zx\r\n", len) : 0;
    memcpy(e->out + begin, e->in, len);
    e->payload_begin = begin;
    e->payload_end = begin + len;
    e->out_len = e->payload_end;
    if (chunked)
    {
        memcpy(e->out + e->out_len, "\r\n", 2);
        e->out_len += 2;
    }
    consume(e, len);
}

static void stage_end(struct session_stream *e)
{
    memcpy(e->out, "0\r\n\r\n", 5);
    e->out_len = 5;
    e->state = PX_DONE;
}

/* Consume a complete size line; leave any partial line in the input buffer. */
static int chunk_size(struct session_stream *e)
{
    size_t i = 0;
    size_t len = 0;
    while (i < e->in_len)
    {
        unsigned char c = (unsigned char)e->in[i];
        unsigned int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'f')
            d = (c | 32) - 'a' + 10;
        else
            break;
        if (len > (SIZE_MAX - d) / 16)
            return -1;
        len = len * 16 + d;
        ++i;
    }
    if (!i)
        return e->in_len ? -1 : 0;
    while (i < e->in_len && (e->in[i] == ' ' || e->in[i] == '\t'))
        ++i;
    if (i < e->in_len && e->in[i] == ';')
    {
        for (++i; i < e->in_len && e->in[i] != '\r'; ++i)
        {
            unsigned char c = (unsigned char)e->in[i];
            if ((c < 32 && c != '\t') || c == 127)
                return -1;
        }
    }
    if (i >= HTTP_CHUNK_LINE_MAX - 1)
        return -1;
    if (i == e->in_len)
        return 0;
    if (e->in[i] != '\r')
        return -1;
    if (i + 1 == e->in_len)
        return 0;
    if (e->in[i + 1] != '\n')
        return -1;
    consume(e, i + 2);
    e->remaining = len;
    e->chunk = len ? PX_DATA : PX_TRAILERS;
    return 1;
}

/* Returns 1 for local progress, 0 if new input is required, -1 for failure. */
static int stage_body(struct session_stream *e)
{
    if (e->head.frame == PROXY_CHUNKED)
    {
        if (e->chunk == PX_SIZE)
            return chunk_size(e);
        if (e->chunk == PX_CRLF)
        {
            if (e->in_len < 2)
                return 0;
            if (memcmp(e->in, "\r\n", 2) != 0)
                return -1;
            consume(e, 2);
            e->chunk = PX_SIZE;
            return 1;
        }
        if (e->chunk == PX_TRAILERS)
        {
            struct http_headers_result r = http_parse_headers(e->in, e->in_len, 0);
            if (r.status == HTTP_PARSE_ERROR)
                return -1;
            if (r.status == HTTP_PARSE_NEED_MORE)
                return 0;
            stage_end(e);
            return 1;
        }
    }
    if (!e->in_len)
        return 0;
    size_t len = e->in_len < PX_BLOCK ? e->in_len : PX_BLOCK;
    if (e->head.frame != PROXY_EOF && len > e->remaining)
        len = e->remaining;
    stage_payload(e, len, e->head.frame != PROXY_LENGTH);
    if (e->head.frame != PROXY_EOF)
    {
        e->remaining -= len;
        if (!e->remaining)
        {
            if (e->head.frame == PROXY_LENGTH)
                e->state = PX_DONE;
            else
                e->chunk = PX_CRLF;
        }
    }
    return 1;
}

int proxy_exchange_pump(struct session_stream *e, bool upstream_readable,
                        bool upstream_writable, int downstream_fd,
                        bool downstream_writable, bool keep_client, size_t *body_sent)
{
    *body_sent = 0;
    if (e->state == PX_CONNECT)
    {
        if (!upstream_writable)
            return 0;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(e->fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
        {
            /* Keep the failed fd open until its replacement has a different number. */
            int failed = e->fd;
            e->fd = -1;
            int rc = connect_next(e);
            close(failed);
            return rc < 0 ? 502 : 0;
        }
        if (set_deadline(e, PX_HEAD_MS) < 0)
            return 502;
        e->state = PX_REQUEST;
    }
    if (e->state == PX_REQUEST)
    {
        if (!upstream_writable)
            return 0;
        if (e->out_pos < e->out_len)
        {
            ssize_t n = push(e->fd, e->out + e->out_pos, e->out_len - e->out_pos);
            if (n < 0)
                return 502;
            e->out_pos += (size_t)n;
            if (e->out_pos < e->out_len)
                return 0;
        }
        if (e->request_pos < e->request_body.length)
        {
            size_t len = e->request_body.length - e->request_pos;
            ssize_t n = push(e->fd, e->request_body.data + e->request_pos,
                             len < PX_BLOCK ? len : PX_BLOCK);
            if (n < 0)
                return 502;
            e->request_pos += (size_t)n;
            if (e->request_pos < e->request_body.length)
                return 0;
        }
        e->out_pos = e->out_len = 0;
        e->state = PX_HEAD;
        if (set_deadline(e, PX_HEAD_MS) < 0)
            return 502;
    }

    for (;;)
    {
        if (proxy_exchange_has_output(e))
        {
            if (!downstream_writable)
                return 0;
            size_t before = e->out_pos;
            ssize_t n = push(downstream_fd, e->out + before, e->out_len - before);
            if (n < 0)
                return -1;
            e->started |= n != 0;
            e->out_pos += (size_t)n;
            size_t first = before > e->payload_begin ? before : e->payload_begin;
            size_t last = e->out_pos < e->payload_end ? e->out_pos : e->payload_end;
            *body_sent += last > first ? last - first : 0;
            if (e->out_pos < e->out_len)
                return 0;
            e->out_len = e->out_pos = e->payload_begin = e->payload_end = 0;
            downstream_writable = false;
        }
        if (e->state == PX_DONE)
        {
            e->delivered = true;
            return 1;
        }

        if (e->state == PX_HEAD)
        {
            size_t len;
            int rc = proxy_response_head(e->in, e->in_len, e->is_head, &e->head,
                                         e->out, sizeof(e->out), &len, keep_client);
            if (rc < 0)
                return 502;
            if (rc > 0)
            {
                consume(e, e->head.consumed);
                if (e->head.status < 200)
                {
                    if (e->head.status == 101 || ++e->interim > 16)
                        return 502;
                    continue;
                }
                if (set_deadline(e, PX_RELAY_MS) < 0)
                    return 502;
                e->keep_client = keep_client && e->head.frame == PROXY_LENGTH;
                e->out_len = len;
                e->remaining = e->head.length;
                e->state = e->is_head || e->head.frame == PROXY_NONE ||
                                   (e->head.frame == PROXY_LENGTH && !e->remaining)
                               ? PX_DONE
                               : PX_BODY;
                continue;
            }
        }
        else
        {
            int rc = stage_body(e);
            if (rc < 0)
                return 502;
            if (rc > 0)
                continue;
        }
        if (e->in_len == sizeof(e->in))
            return 502;
        if (!upstream_readable)
            return 0;
        ssize_t n;
        do
        {
            n = recv(e->fd, e->in + e->in_len, sizeof(e->in) - e->in_len, 0);
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : 502;
        if (n == 0)
        {
            if (e->state != PX_BODY || e->head.frame != PROXY_EOF)
                return 502;
            stage_end(e);
        }
        else
            e->in_len += (size_t)n;
        upstream_readable = false;
    }
}
