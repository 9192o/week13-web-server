#include "conn_internal.h"

#include "access_log.h"
#include "http.h"
#include "util.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const conn_common_hooks_t *g_hooks;

void conn_common_set_hooks(const conn_common_hooks_t *hooks) {
    g_hooks = hooks;
}

void conn_common_hook_detach(conn_t *c) {
    if (g_hooks != NULL && g_hooks->detach != NULL)
        g_hooks->detach(c);
}

int conn_common_hook_next_timeout_ms(void) {
    if (g_hooks == NULL || g_hooks->next_timeout_ms == NULL)
        return -1;
    return g_hooks->next_timeout_ms();
}

int conn_common_extra_connections(void) {
    if (g_hooks == NULL || g_hooks->extra_connections == NULL)
        return 0;
    return g_hooks->extra_connections();
}

conn_t *conn_common_new(int fd, const struct sockaddr_in *peer,
                        const config_t *cfg, ev_loop_t *loop) {
    conn_t *c = calloc(1, sizeof *c);
    if (c == NULL)
        return NULL;

    c->source.kind = EV_SRC_CLIENT;
    c->loop = loop;
    c->cfg = cfg;
    c->fd = fd;
    c->peer = *peer;
    c->state = ST_READ;
    c->file_fd = -1;
    c->deadline_ms = now_ms() +
        (uint64_t)cfg->request_timeout_sec * 1000;
    http_request_init(&c->req);

    char ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof ip) == NULL)
        snprintf(ip, sizeof ip, "?");
    access_log_init(&c->log, ip);
    return c;
}

void conn_common_log_begin(conn_t *c, const char *method, const char *path) {
    access_log_restart(&c->log);
    access_log_set_request(&c->log, method, path);
    c->log_has_request = 1;
}

void conn_common_log_response(conn_t *c, int status, size_t body_bytes) {
    access_log_set_status(&c->log, status);
    access_log_add_bytes(&c->log, body_bytes);
}

void conn_common_log_end(conn_t *c) {
    if (!c->log_has_request)
        return;
    if (access_log_get_status(&c->log) != 0)
        access_log_end(&c->log);
    c->log_has_request = 0;
}

void conn_common_free(conn_t *c) {
    conn_common_hook_detach(c);
    if (c->file_fd >= 0)
        close(c->file_fd);
    free(c->file_buf);
    free(c->out);
    http_request_free(&c->req);
    free(c);
}

void conn_common_close(ev_loop_t *loop, conn_t *c) {
    if (c->closed)
        return;
    conn_common_log_end(c);
    conn_common_hook_detach(c);
    c->closed = 1;
    if (c->fd >= 0) {
        if (c->in_loop) {
            (void)ev_del(loop, c->fd);
            c->in_loop = 0;
        }
        close(c->fd);
        c->fd = -1;
    }
}

void conn_common_sweep(conn_t **head, int *count) {
    conn_t **pp = head;
    while (*pp != NULL) {
        conn_t *c = *pp;
        if (!c->closed) {
            pp = &c->next;
            continue;
        }
        *pp = c->next;
        conn_common_free(c);
        (*count)--;
    }
}

void conn_common_reset_response(conn_t *c) {
    if (c->file_fd >= 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
    free(c->out);
    c->out = NULL;
    c->out_len = 0;
    c->out_sent = 0;
    c->file_remaining = 0;
    c->file_buf_len = 0;
    c->file_buf_sent = 0;
}

int conn_common_build_error(conn_t *c, int status, int close_connection,
                            int head, int retry_after, int timeout_sec) {
    http_response_t res;
    char body[512];
    size_t body_len = 0;

    if (http_response_build_error(status, close_connection, &res, body,
                                  sizeof body, &body_len) < 0)
        return -1;
    if (retry_after &&
        http_response_add_header(&res, "Retry-After", "1") < 0)
        return -1;

    size_t cap = HTTP_MAX_RESPONSE_HEADERS + 128 + body_len;
    char *out = malloc(cap);
    if (out == NULL)
        return -1;

    size_t hdr_len = 0;
    if (http_response_format(&res, out, cap, &hdr_len) < 0) {
        free(out);
        return -1;
    }

    size_t out_len = hdr_len;
    if (!head && body_len > 0) {
        memcpy(out + out_len, body, body_len);
        out_len += body_len;
    }

    conn_common_reset_response(c);
    c->out = out;
    c->out_len = out_len;
    c->out_sent = 0;
    c->head_only = head;
    c->close_after = close_connection;
    c->state = ST_WRITE;
    c->deadline_ms = now_ms() + (uint64_t)timeout_sec * 1000;
    conn_common_log_response(c, status, head ? 0 : body_len);
    return 0;
}

int server_conn_build_error(conn_t *c, int status, int close_connection,
                            int head, int retry_after) {
    if (c == NULL || c->cfg == NULL)
        return -1;
    return conn_common_build_error(c, status, close_connection, head,
                                   retry_after, c->cfg->request_timeout_sec);
}

void server_conn_close(conn_t *c) {
    if (c != NULL)
        conn_common_close(c->loop, c);
}

void server_conn_log_response(conn_t *c, int status, size_t body_bytes) {
    if (c != NULL)
        conn_common_log_response(c, status, body_bytes);
}

void server_conn_log_end(conn_t *c) {
    if (c != NULL)
        conn_common_log_end(c);
}
