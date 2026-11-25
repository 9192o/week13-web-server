#include "conn_common.h"

#include "http.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONN_FILE_CHUNK 65536

int conn_common_has_header_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return 1;
    }
    return 0;
}

int conn_common_request_is_head(const char *buf, size_t len) {
    return len >= 4 && memcmp(buf, "HEAD", 4) == 0 &&
           (len == 4 || buf[4] == ' ');
}

int conn_common_has_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return 1;
    }
    return 0;
}

int conn_common_request_line_uri_too_long(const char *buf, size_t len,
                                          size_t max_uri) {
    const char *line_end = NULL;
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            line_end = buf + i;
            break;
        }
    }
    if (line_end == NULL)
        return 0;

    const char *sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (sp1 == NULL)
        return 0;
    const char *sp2 = memchr(sp1 + 1, ' ',
                             (size_t)(line_end - (sp1 + 1)));
    if (sp2 == NULL)
        return 0;

    size_t target_len = (size_t)(sp2 - (sp1 + 1));
    return target_len > max_uri;
}

int conn_common_read_headers(conn_t *c, ev_loop_t *loop, const config_t *cfg,
                             conn_process_fn process, void *ctx) {
    if (c->state != ST_READ)
        return 0;

    while (!conn_common_has_header_end(c->in, c->in_len)) {
        if (c->in_len == sizeof c->in) {
            int status = HTTP_HEADERS_TOO_LARGE;
            if (!conn_common_has_crlf(c->in, c->in_len) ||
                conn_common_request_line_uri_too_long(c->in, c->in_len,
                                                      cfg->max_uri))
                status = HTTP_URI_TOO_LONG;

            conn_common_log_begin(c, "-", "-");
            if (conn_common_build_error(c, status, 1,
                                        conn_common_request_is_head(
                                            c->in, c->in_len),
                                        0, cfg->request_timeout_sec) < 0)
                return -1;
            c->requests++;
            if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
                return -1;
            return 0;
        }

        int idle = (c->in_len == 0 && c->requests > 0);
        ssize_t n = read(c->fd, c->in + c->in_len,
                         sizeof c->in - c->in_len);
        if (n > 0) {
            if (idle)
                c->deadline_ms = now_ms() +
                    (uint64_t)cfg->request_timeout_sec * 1000;
            c->in_len += (size_t)n;
            continue;
        }
        if (n == 0)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }

    if (process(c, loop, ctx, cfg) < 0)
        return -1;

    if (c->state == ST_WRITE) {
        if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
            return -1;
    }
    return 0;
}

int conn_common_on_writable(conn_t *c, ev_loop_t *loop, const config_t *cfg,
                            conn_process_fn process, void *ctx) {
    if (c->state != ST_WRITE)
        return 0;

    while (c->out_sent < c->out_len) {
        ssize_t n = write(c->fd, c->out + c->out_sent,
                          c->out_len - c->out_sent);
        if (n > 0) {
            c->out_sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    }

    if (c->file_remaining > 0) {
        if (c->file_buf == NULL) {
            c->file_buf = malloc(CONN_FILE_CHUNK);
            if (c->file_buf == NULL)
                return -1;
        }

        while (c->file_remaining > 0) {
            if (c->file_buf_sent == c->file_buf_len) {
                size_t want = c->file_remaining < CONN_FILE_CHUNK
                                  ? c->file_remaining : CONN_FILE_CHUNK;
                ssize_t r = read(c->file_fd, c->file_buf, want);
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    return -1;
                }
                if (r == 0)
                    return -1;
                c->file_buf_len = (size_t)r;
                c->file_buf_sent = 0;
            }

            while (c->file_buf_sent < c->file_buf_len) {
                ssize_t w = write(c->fd,
                                  c->file_buf + c->file_buf_sent,
                                  c->file_buf_len - c->file_buf_sent);
                if (w > 0) {
                    c->file_buf_sent += (size_t)w;
                    c->file_remaining -= (size_t)w;
                    continue;
                }
                if (w < 0 && errno == EINTR)
                    continue;
                if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return 0;
                return -1;
            }
        }
    }

    if (c->file_fd >= 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }

    conn_common_reset_response(c);
    conn_common_log_end(c);

    if (c->close_after)
        return -1;

    c->state = ST_READ;
    c->deadline_ms = now_ms() +
        (uint64_t)(c->in_len == 0 ? cfg->keepalive_idle_sec
                                  : cfg->request_timeout_sec) * 1000;

    if (ev_mod(loop, c->fd, EV_READ, c) < 0)
        return -1;

    if (c->in_len > 0 && conn_common_has_header_end(c->in, c->in_len)) {
        if (process(c, loop, ctx, cfg) < 0)
            return -1;
        if (c->state == ST_WRITE) {
            if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
                return -1;
        }
    }
    return 0;
}
