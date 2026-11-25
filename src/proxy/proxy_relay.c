#include "proxy_core.h"

#include "chunked.h"
#include "event_backend.h"
#include "http.h"
#include "http_upstream.h"
#include "proxy_wire.h"
#include "util.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void proxy_flush_upstream(proxy_conn_t *p) {
    while (p->req_out_sent < p->req_out_len) {
        ssize_t n = write(p->upstream_fd, p->req_out + p->req_out_sent,
                          p->req_out_len - p->req_out_sent);
        if (n > 0) {
            p->req_out_sent += (size_t)n;
            p->deadline_ms = now_ms() +
                (uint64_t)proxy_config()->request_timeout_sec * 1000;
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
                (uint64_t)proxy_config()->request_timeout_sec * 1000;
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

void proxy_read_client_body(proxy_conn_t *p) {
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
            if (p->req_chunk.data_bytes > proxy_config()->max_body) {
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

void proxy_read_upstream_headers(proxy_conn_t *p) {
    if (p->resp_hdr == NULL) {
        size_t cap = proxy_config()->max_header_bytes + 1;
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
                p->resp_hdr, p->resp_hdr_len, proxy_config(), &p->resp);
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
        p->deadline_ms = now_ms() + (uint64_t)(proxy_config()->timeout_body_sec * 1000);
        proxy_update_events(p);
        return;
}

void proxy_flush_client(proxy_conn_t *p);
void proxy_flush_continue(proxy_conn_t *p);

void proxy_read_upstream_body(proxy_conn_t *p) {
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
            p->deadline_ms = now_ms() + (uint64_t)(proxy_config()->timeout_body_sec * 1000);
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
                p->deadline_ms = now_ms() + (uint64_t)(proxy_config()->timeout_body_sec * 1000);
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
            p->deadline_ms = now_ms() + (uint64_t)(proxy_config()->timeout_body_sec * 1000);
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

void proxy_drain_eof(proxy_conn_t *p) {
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

void proxy_flush_continue(proxy_conn_t *p) {
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

void proxy_flush_client(proxy_conn_t *p) {
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
            p->deadline_ms = now_ms() + (uint64_t)(proxy_config()->timeout_body_sec * 1000);
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
            p->deadline_ms = now_ms() + (uint64_t)(proxy_config()->timeout_body_sec * 1000);
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

