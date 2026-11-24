#include "server_ev.h"

#include "cgi.h"
#include "event_backend.h"
#include "http.h"
#include "static_file.h"
#include "task_pool.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define EV_MAX_CONNS 1024
#define EV_FILE_CHUNK 65536

static volatile sig_atomic_t g_stop = 0;
static uint64_t g_task_seq = 0;
static int g_signal_pipe[2] = {-1, -1};

static int set_nonblocking(int fd);
static int set_cloexec(int fd);

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_signal_pipe[1] >= 0) {
        char byte = 'x';
        ssize_t n = write(g_signal_pipe[1], &byte, 1);
        (void)n;
    }
}

static int setup_signals(void) {
    if (pipe(g_signal_pipe) < 0)
        return -1;
    if (set_nonblocking(g_signal_pipe[0]) < 0 ||
        set_nonblocking(g_signal_pipe[1]) < 0 ||
        set_cloexec(g_signal_pipe[0]) < 0 ||
        set_cloexec(g_signal_pipe[1]) < 0) {
        close(g_signal_pipe[0]);
        close(g_signal_pipe[1]);
        g_signal_pipe[0] = g_signal_pipe[1] = -1;
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: ev_wait can return EINTR */

    if (sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0 ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        close(g_signal_pipe[0]);
        close(g_signal_pipe[1]);
        g_signal_pipe[0] = g_signal_pipe[1] = -1;
        return -1;
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
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }

    (void)set_cloexec(fd);

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        log_error("setsockopt(SO_REUSEADDR): %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        log_error("bind(%d): %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        log_error("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (set_nonblocking(fd) < 0) {
        log_error("fcntl(O_NONBLOCK): %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int has_header_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return 1;
    }
    return 0;
}

static int request_is_head(const char *buf, size_t len) {
    return len >= 4 && memcmp(buf, "HEAD", 4) == 0 &&
           (len == 4 || buf[4] == ' ');
}

static int has_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return 1;
    }
    return 0;
}

static int request_line_uri_too_long(const char *buf, size_t len,
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

typedef enum {
    ST_READ,
    ST_READ_BODY,
    ST_WAIT_TASK,
    ST_WRITE
} conn_state_t;

typedef struct conn {
    int fd;
    struct sockaddr_in peer;
    conn_state_t state;
    int closed;

    char in[16384];
    size_t in_len;

    http_request_t req;

    char *out;
    size_t out_len;
    size_t out_sent;

    int file_fd;
    off_t file_offset;
    size_t file_remaining;
    char *file_buf;
    size_t file_buf_len;
    size_t file_buf_sent;

    int head_only;
    int close_after;
    int requests;
    uint64_t task_id;
    size_t body_received;
    uint64_t deadline_ms;

    struct conn *next;
} conn_t;

static conn_t *conn_new(int fd, const struct sockaddr_in *peer,
                        const config_t *cfg) {
    conn_t *c = calloc(1, sizeof *c);
    if (c == NULL)
        return NULL;

    c->fd = fd;
    c->peer = *peer;
    c->state = ST_READ;
    c->file_fd = -1;
    c->deadline_ms = now_ms() +
        (uint64_t)cfg->request_timeout_sec * 1000;
    http_request_init(&c->req);
    return c;
}

static void conn_free(conn_t *c) {
    if (c->file_fd >= 0)
        close(c->file_fd);
    free(c->file_buf);
    free(c->out);
    http_request_free(&c->req);
    free(c);
}

static void conn_close(ev_loop_t *loop, conn_t *c) {
    if (c->closed)
        return;
    c->closed = 1;
    if (c->fd >= 0) {
        (void)ev_del(loop, c->fd);
        close(c->fd);
        c->fd = -1;
    }
}

static void conn_sweep(conn_t **head, int *count) {
    conn_t **pp = head;
    while (*pp != NULL) {
        conn_t *c = *pp;
        if (!c->closed) {
            pp = &c->next;
            continue;
        }
        *pp = c->next;
        conn_free(c);
        (*count)--;
    }
}

static void conn_reset_response(conn_t *c) {
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

static int conn_build_error(conn_t *c, int status, int close_connection,
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

    conn_reset_response(c);
    c->out = out;
    c->out_len = out_len;
    c->out_sent = 0;
    c->head_only = head;
    c->close_after = close_connection;
    c->state = ST_WRITE;
    c->deadline_ms = now_ms() + (uint64_t)timeout_sec * 1000;
    return 0;
}

static int conn_build_options(conn_t *c, int close_connection,
                              int timeout_sec) {
    http_response_t res;
    http_response_init(&res);
    res.status = 200;
    res.close_connection = close_connection;

    if (http_response_add_header(&res, "Allow",
                                 "GET, POST, HEAD, OPTIONS") < 0 ||
        http_response_add_header(&res, "Content-Length", "0") < 0 ||
        http_response_add_header(&res, "Connection",
                                 close_connection ? "close" : "keep-alive") < 0)
        return -1;

    size_t cap = HTTP_MAX_RESPONSE_HEADERS + 128;
    char *out = malloc(cap);
    if (out == NULL)
        return -1;

    size_t out_len = 0;
    if (http_response_format(&res, out, cap, &out_len) < 0) {
        free(out);
        return -1;
    }

    conn_reset_response(c);
    c->out = out;
    c->out_len = out_len;
    c->out_sent = 0;
    c->head_only = 0;
    c->close_after = close_connection;
    c->state = ST_WRITE;
    c->deadline_ms = now_ms() + (uint64_t)timeout_sec * 1000;
    return 0;
}

static int conn_build_static(conn_t *c, static_result_t *r,
                             int timeout_sec) {
    size_t cap = HTTP_MAX_RESPONSE_HEADERS + 128 + r->body_len;
    char *out = malloc(cap);
    if (out == NULL) {
        if (r->file_fd >= 0)
            close(r->file_fd);
        return -1;
    }

    size_t hdr_len = 0;
    if (http_response_format(&r->res, out, cap, &hdr_len) < 0) {
        free(out);
        if (r->file_fd >= 0)
            close(r->file_fd);
        return -1;
    }

    size_t out_len = hdr_len;
    if (!r->head_only && r->body_len > 0) {
        memcpy(out + out_len, r->body, r->body_len);
        out_len += r->body_len;
    }

    conn_reset_response(c);
    c->out = out;
    c->out_len = out_len;
    c->out_sent = 0;
    c->head_only = r->head_only;
    c->close_after = r->res.close_connection;
    c->state = ST_WRITE;
    c->deadline_ms = now_ms() + (uint64_t)timeout_sec * 1000;

    if (r->file_fd >= 0) {
        if (r->head_only) {
            close(r->file_fd);
        } else {
            if (lseek(r->file_fd, r->file_offset, SEEK_SET) < 0) {
                close(r->file_fd);
                free(c->out);
                c->out = NULL;
                c->out_len = 0;
                return -1;
            }
            c->file_fd = r->file_fd;
            c->file_offset = r->file_offset;
            c->file_remaining = r->content_length;
        }
    }
    return 0;
}

static void *cgi_task_fn(void *arg) {
    cgi_job_t *job = arg;
    cgi_result_t *res = cgi_job_execute(job);
    cgi_job_free(job);
    return res;
}

static conn_t *find_by_task(conn_t *head, uint64_t id) {
    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->task_id == id)
            return c;
    }
    return NULL;
}

static int submit_cgi(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
                      const config_t *cfg, int close_after) {
    char remote[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &c->peer.sin_addr, remote, sizeof remote) == NULL)
        snprintf(remote, sizeof remote, "?");

    cgi_job_t *job = cgi_job_create(&c->req, cfg, remote, close_after);
    if (job == NULL) {
        if (conn_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1,
                             c->head_only, 0,
                             cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 0;
    }

    uint64_t id = ++g_task_seq;
    if (task_pool_submit(pool, id, cgi_task_fn, job) < 0) {
        int err = errno;
        cgi_job_free(job);

        int status = err == EAGAIN ? HTTP_SERVICE_UNAVAILABLE
                                   : HTTP_INTERNAL_SERVER_ERROR;
        int retry_after = err == EAGAIN;
        if (conn_build_error(c, status, 1, c->head_only, retry_after,
                             cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 0;
    }

    c->task_id = id;
    c->state = ST_WAIT_TASK;
    c->head_only = c->req.method == HTTP_HEAD;
    c->requests++;
    c->deadline_ms = now_ms() +
        (uint64_t)(cfg->request_timeout_sec + 5) * 1000;
    http_request_free(&c->req);

    if (ev_del(loop, c->fd) < 0)
        return -1;
    return 0;
}

static void drain_task_completions(ev_loop_t *loop, task_pool_t *pool,
                                   conn_t *head, const config_t *cfg) {
    int fd = task_pool_completion_fd(pool);
    char buf[256];
    while (read(fd, buf, sizeof buf) > 0)
        ;

    uint64_t id = 0;
    void *data = NULL;
    while (task_pool_pop_result(pool, &id, &data) == 1) {
        cgi_result_t *res = data;
        conn_t *c = find_by_task(head, id);

        if (c != NULL && !c->closed && c->state == ST_WAIT_TASK) {
            if (res == NULL || res->error || res->response == NULL) {
                if (conn_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1,
                                     c->head_only, 0,
                                     cfg->request_timeout_sec) < 0) {
                    conn_close(loop, c);
                } else if (ev_add(loop, c->fd, EV_WRITE, c) < 0) {
                    conn_close(loop, c);
                }
            } else {
                free(c->out);
                c->out = res->response;
                res->response = NULL;
                c->out_len = res->response_len;
                c->out_sent = 0;
                c->close_after = res->close_connection;
                c->file_fd = -1;
                c->file_remaining = 0;
                c->state = ST_WRITE;
                c->deadline_ms = now_ms() +
                    (uint64_t)cfg->request_timeout_sec * 1000;
                if (ev_add(loop, c->fd, EV_WRITE, c) < 0)
                    conn_close(loop, c);
            }
        }

        if (res != NULL)
            cgi_result_free(res);
    }
}

static void free_pending_results(task_pool_t *pool) {
    uint64_t id = 0;
    void *data = NULL;
    while (task_pool_pop_result(pool, &id, &data) == 1)
        cgi_result_free(data);
}

static int process_buffer(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
                          const config_t *cfg) {
    c->body_received = 0;
    http_request_init(&c->req);

    http_status_t st = http_request_parse_buffer(c->in, c->in_len, cfg, &c->req);
    if (st == HTTP_PARSE_INCOMPLETE)
        st = HTTP_BAD_REQUEST;

    int head = (c->req.method == HTTP_HEAD) ||
               request_is_head(c->in, c->in_len);

    if (st != HTTP_OK) {
        if (conn_build_error(c, st, 1, head, 0,
                             cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 1;
    }


    int keep = c->req.keep_alive &&
               (c->requests + 1 < cfg->keepalive_max_requests) &&
               !g_stop;
    int close_after = !keep;
    c->head_only = head;

    size_t buffered = c->in_len > c->req.header_len
                          ? c->in_len - c->req.header_len : 0;
    int is_cgi = cfg->cgi_dir[0] != '\0' &&
                 strncmp(c->req.path, "/cgi-bin/", 9) == 0;

    if (is_cgi && c->req.content_length > 0 &&
        buffered < c->req.content_length) {
        c->req.body = malloc(c->req.content_length);
        if (c->req.body == NULL) {
            if (conn_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1, head, 0,
                                 cfg->request_timeout_sec) < 0) {
                http_request_free(&c->req);
                return -1;
            }
            c->requests++;
            http_request_free(&c->req);
            return 1;
        }

        if (buffered > 0)
            memcpy(c->req.body, c->in + c->req.header_len, buffered);
        c->body_received = buffered;

        size_t consumed = c->req.header_len + buffered;
        if (consumed > c->in_len)
            consumed = c->in_len;
        size_t leftover = c->in_len - consumed;
        if (leftover > 0)
            memmove(c->in, c->in + consumed, leftover);
        c->in_len = leftover;

        c->state = ST_READ_BODY;
        c->deadline_ms = now_ms() +
            (uint64_t)cfg->request_timeout_sec * 1000;

        if (c->req.expect_continue) {
            if (writen(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25) < 0) {
                http_request_free(&c->req);
                return -1;
            }
        }
        return 1;
    }

    if (is_cgi && c->req.content_length > 0) {
        c->req.body = malloc(c->req.content_length);
        if (c->req.body == NULL) {
            if (conn_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1, head, 0,
                                 cfg->request_timeout_sec) < 0) {
                http_request_free(&c->req);
                return -1;
            }
            c->requests++;
            http_request_free(&c->req);
            return 1;
        }
        memcpy(c->req.body, c->in + c->req.header_len,
               c->req.content_length);
        c->req.body_len = c->req.content_length;
    }

    if (is_cgi) {
        size_t body_in_buf = buffered < c->req.content_length
                                 ? buffered : c->req.content_length;
        size_t consumed = c->req.header_len + body_in_buf;
        if (consumed > c->in_len)
            consumed = c->in_len;
        size_t leftover = c->in_len - consumed;
        if (leftover > 0)
            memmove(c->in, c->in + consumed, leftover);
        c->in_len = leftover;

        return submit_cgi(c, loop, pool, cfg, close_after);
    }

    if (c->req.content_length > 0 &&
        buffered < c->req.content_length)
        close_after = 1;

    if (c->req.method == HTTP_OPTIONS) {
        if (conn_build_options(c, close_after,
                               cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
    } else {
        static_result_t r;
        handler_result_t rc = static_file_prepare(&c->req, cfg,
                                                  close_after, &r);
        if (rc == HANDLER_ERROR) {
            http_request_free(&c->req);
            return -1;
        }
        if (conn_build_static(c, &r, cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
    }

    c->requests++;

    size_t body_in_buf = 0;
    if (c->req.content_length > 0 && buffered > 0) {
        body_in_buf = buffered < c->req.content_length
                          ? buffered : c->req.content_length;
    }
    size_t consumed = c->req.header_len + body_in_buf;
    if (consumed > c->in_len)
        consumed = c->in_len;

    size_t leftover = c->in_len - consumed;
    if (leftover > 0)
        memmove(c->in, c->in + consumed, leftover);
    c->in_len = leftover;

    http_request_free(&c->req);
    return 1;
}

static int on_readable(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
                       const config_t *cfg) {
    if (c->state == ST_READ_BODY) {
        while (c->body_received < c->req.content_length) {
            size_t want = c->req.content_length - c->body_received;
            ssize_t n = read(c->fd, c->req.body + c->body_received, want);
            if (n > 0) {
                c->body_received += (size_t)n;
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

        c->req.body_len = c->req.content_length;
        int keep = c->req.keep_alive &&
                   (c->requests + 1 < cfg->keepalive_max_requests) &&
                   !g_stop;
        if (submit_cgi(c, loop, pool, cfg, !keep) < 0)
            return -1;
        if (c->state == ST_WRITE) {
            if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
                return -1;
        }
        return 0;
    }

    if (c->state != ST_READ)
        return 0;

    while (!has_header_end(c->in, c->in_len)) {
        if (c->in_len == sizeof c->in) {
            int status = HTTP_HEADERS_TOO_LARGE;
            if (!has_crlf(c->in, c->in_len) ||
                request_line_uri_too_long(c->in, c->in_len, cfg->max_uri))
                status = HTTP_URI_TOO_LONG;

            if (conn_build_error(c, status, 1,
                                 request_is_head(c->in, c->in_len), 0,
                                 cfg->request_timeout_sec) < 0)
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

    if (process_buffer(c, loop, pool, cfg) < 0)
        return -1;

    if (c->state == ST_WRITE) {
        if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
            return -1;
    }
    return 0;
}

static int on_writable(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
                       const config_t *cfg) {
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
            c->file_buf = malloc(EV_FILE_CHUNK);
            if (c->file_buf == NULL)
                return -1;
        }

        while (c->file_remaining > 0) {
            if (c->file_buf_sent == c->file_buf_len) {
                size_t want = c->file_remaining < EV_FILE_CHUNK
                                  ? c->file_remaining : EV_FILE_CHUNK;
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

    free(c->out);
    c->out = NULL;
    c->out_len = 0;
    c->out_sent = 0;
    c->file_buf_len = 0;
    c->file_buf_sent = 0;

    if (c->close_after)
        return -1;

    c->state = ST_READ;
    c->deadline_ms = now_ms() +
        (uint64_t)(c->in_len == 0 ? cfg->keepalive_idle_sec
                                  : cfg->request_timeout_sec) * 1000;

    if (ev_mod(loop, c->fd, EV_READ, c) < 0)
        return -1;

    if (c->in_len > 0 && has_header_end(c->in, c->in_len)) {
        if (process_buffer(c, loop, pool, cfg) < 0)
            return -1;
        if (c->state == ST_WRITE) {
            if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
                return -1;
        }
    }
    return 0;
}

static void accept_new(int listenfd, ev_loop_t *loop, conn_t **head,
                       int *count, const config_t *cfg) {
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof peer;
        int fd = accept(listenfd, (struct sockaddr *)&peer, &peerlen);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EMFILE) {
                log_error("accept: %s", strerror(errno));
                break;
            }
            log_error("accept: %s", strerror(errno));
            break;
        }

        if (*count >= EV_MAX_CONNS) {
            close(fd);
            continue;
        }
        if (set_nonblocking(fd) < 0 || set_cloexec(fd) < 0) {
            close(fd);
            continue;
        }

        conn_t *c = conn_new(fd, &peer, cfg);
        if (c == NULL) {
            close(fd);
            continue;
        }
        if (ev_add(loop, fd, EV_READ, c) < 0) {
            close(fd);
            conn_free(c);
            continue;
        }

        c->next = *head;
        *head = c;
        (*count)++;
    }
}

static int next_timeout(conn_t *head) {
    if (head == NULL)
        return 1000;

    uint64_t now = now_ms();
    uint64_t min = UINT64_MAX;
    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->deadline_ms < min)
            min = c->deadline_ms;
    }
    if (min == UINT64_MAX)
        return 1000;
    if (min <= now)
        return 0;

    uint64_t d = min - now;
    return d > (uint64_t)INT_MAX ? INT_MAX : (int)d;
}

static void expire_timers(ev_loop_t *loop, conn_t *head,
                          const config_t *cfg) {
    uint64_t now = now_ms();
    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->closed || now < c->deadline_ms)
            continue;

        if (c->state == ST_READ && c->in_len == 0 && c->requests > 0) {
            conn_close(loop, c);
            continue;
        }

        if (c->state == ST_READ || c->state == ST_READ_BODY) {
            if (conn_build_error(c, HTTP_REQUEST_TIMEOUT, 1,
                                 request_is_head(c->in, c->in_len), 0,
                                 cfg->request_timeout_sec) < 0) {
                conn_close(loop, c);
                continue;
            }
            c->requests++;
            if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
                conn_close(loop, c);
            continue;
        }

        conn_close(loop, c);
    }
}

int server_ev_run(const config_t *cfg) {
    g_stop = 0;
    g_task_seq = 0;

    /*
     * macOS keeps one signal mask for the whole process, and fork() blocks
     * signals around the fork. A worker's fork can therefore leave SIGINT
     * blocked in the main thread; unblock the shutdown signals before every
     * wait so they are always deliverable.
     */
    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);

    if (setup_signals() < 0) {
        log_error("signal setup failed: %s", strerror(errno));
        return -1;
    }

    int listenfd = create_listen_socket(cfg->port);
    if (listenfd < 0)
        return -1;

    ev_loop_t *loop = ev_create_backend(getenv("EV_BACKEND"));
    if (loop == NULL) {
        close(listenfd);
        return -1;
    }
    if (ev_add(loop, listenfd, EV_READ, NULL) < 0) {
        close(listenfd);
        ev_destroy(loop);
        return -1;
    }

    task_pool_t *pool = task_pool_create(4, 64);
    if (pool == NULL) {
        close(listenfd);
        ev_destroy(loop);
        return -1;
    }
    int completion_fd = task_pool_completion_fd(pool);
    if (ev_add(loop, completion_fd, EV_READ, NULL) < 0) {
        task_pool_destroy(pool);
        close(listenfd);
        ev_destroy(loop);
        return -1;
    }
    if (ev_add(loop, g_signal_pipe[0], EV_READ, NULL) < 0) {
        task_pool_destroy(pool);
        close(listenfd);
        ev_destroy(loop);
        return -1;
    }

    log_error("listening on 0.0.0.0:%d (loop=%s, docroot: %s)",
              cfg->port, ev_loop_backend_name(loop), cfg->docroot);

    conn_t *head = NULL;
    int count = 0;

    while (!g_stop) {
        pthread_sigmask(SIG_UNBLOCK, &shutdown_signals, NULL);
        int timeout = next_timeout(head);
        ev_event_t events[64];
        int n = ev_wait(loop, events, 64, timeout);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_error("ev_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].fd == listenfd) {
                if (events[i].events & EV_READ)
                    accept_new(listenfd, loop, &head, &count, cfg);
                continue;
            }
            if (events[i].fd == completion_fd) {
                drain_task_completions(loop, pool, head, cfg);
                continue;
            }
            if (events[i].fd == g_signal_pipe[0]) {
                char buf[64];
                while (read(g_signal_pipe[0], buf, sizeof buf) > 0)
                    ;
                continue;
            }

            conn_t *c = events[i].ptr;
            if (c == NULL || c->closed)
                continue;

            int err = (events[i].events & EV_ERR) != 0;
            if (err)
                c->close_after = 1;

            if ((events[i].events & EV_READ) &&
                (c->state == ST_READ || c->state == ST_READ_BODY)) {
                if (on_readable(c, loop, pool, cfg) < 0) {
                    conn_close(loop, c);
                    continue;
                }
            }
            if (!c->closed && (events[i].events & EV_WRITE) &&
                c->state == ST_WRITE) {
                if (on_writable(c, loop, pool, cfg) < 0) {
                    conn_close(loop, c);
                    continue;
                }
            }

            /*
             * A hangup can arrive with readable data (half-close). Read and
             * queue the response first; only close immediately when there is
             * nothing left to write.
             */
            if (!c->closed && err &&
                (c->state == ST_READ || c->state == ST_READ_BODY))
                conn_close(loop, c);
        }

        conn_sweep(&head, &count);
        expire_timers(loop, head, cfg);
        conn_sweep(&head, &count);
    }

    while (head != NULL) {
        conn_close(loop, head);
        conn_sweep(&head, &count);
    }

    log_error("server_ev: shutting down pool");
    (void)ev_del(loop, completion_fd);
    (void)ev_del(loop, g_signal_pipe[0]);
    task_pool_shutdown(pool);
    free_pending_results(pool);
    task_pool_destroy(pool);

    (void)ev_del(loop, listenfd);
    close(listenfd);
    close(g_signal_pipe[0]);
    close(g_signal_pipe[1]);
    g_signal_pipe[0] = g_signal_pipe[1] = -1;
    ev_destroy(loop);
    log_error("server_ev stopped");
    return 0;
}
