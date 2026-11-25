#include "conn_common.h"

#include "access_log.h"
#include "http.h"
#include "logger.h"
#include "proxy.h"
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
#include <time.h>
#include <unistd.h>

#define EV_FILE_CHUNK 65536

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_signal_count = 0;
static int g_signal_pipe[2] = {-1, -1};

static void on_signal(int sig) {
    (void)sig;
    if (++g_signal_count >= 2)
        _exit(1);
    g_stop = 1;
    if (g_signal_pipe[1] >= 0) {
        char byte = 'x';
        ssize_t n = write(g_signal_pipe[1], &byte, 1);
        (void)n;
    }
}

int conn_common_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int conn_common_set_cloexec(int fd) {
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

int conn_common_setup_signals(void) {
    if (pipe(g_signal_pipe) < 0)
        return -1;
    if (conn_common_set_nonblocking(g_signal_pipe[0]) < 0 ||
        conn_common_set_nonblocking(g_signal_pipe[1]) < 0 ||
        conn_common_set_cloexec(g_signal_pipe[0]) < 0 ||
        conn_common_set_cloexec(g_signal_pipe[1]) < 0) {
        close(g_signal_pipe[0]);
        close(g_signal_pipe[1]);
        g_signal_pipe[0] = g_signal_pipe[1] = -1;
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0 ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        conn_common_close_signals();
        return -1;
    }
    return 0;
}

int conn_common_signal_fd(void) {
    return g_signal_pipe[0];
}

void conn_common_close_signals(void) {
    if (g_signal_pipe[0] >= 0)
        close(g_signal_pipe[0]);
    if (g_signal_pipe[1] >= 0)
        close(g_signal_pipe[1]);
    g_signal_pipe[0] = g_signal_pipe[1] = -1;
}

int conn_common_stopping(void) {
    return g_stop != 0;
}

int conn_common_create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }
    (void)conn_common_set_cloexec(fd);

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
    if (conn_common_set_nonblocking(fd) < 0) {
        log_error("fcntl(O_NONBLOCK): %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

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
    proxy_detach(c);
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
    proxy_detach(c);
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

int server_is_stopping(void) {
    return g_stop != 0;
}

void conn_common_reject_overloaded(int fd, const struct sockaddr_in *peer) {
    static const char response[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Retry-After: 1\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";

    char ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof ip) == NULL)
        snprintf(ip, sizeof ip, "?");
    logger_access(ip, "-", "-", 503, 0);

    if (conn_common_set_nonblocking(fd) < 0) {
        close(fd);
        return;
    }
    ssize_t n = send(fd, response, sizeof response - 1, 0);
    (void)n;
    close(fd);
}

void conn_common_accept_new(int listenfd, ev_loop_t *loop, conn_t **head,
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
            log_error("accept: %s", strerror(errno));
            break;
        }

        if (*count >= cfg->max_conns) {
            conn_common_reject_overloaded(fd, &peer);
            continue;
        }
        if (conn_common_set_nonblocking(fd) < 0 ||
            conn_common_set_cloexec(fd) < 0) {
            close(fd);
            continue;
        }

        conn_t *c = conn_common_new(fd, &peer, cfg, loop);
        if (c == NULL) {
            close(fd);
            continue;
        }
        if (ev_add(loop, fd, EV_READ, c) < 0) {
            close(fd);
            conn_common_free(c);
            continue;
        }
        c->in_loop = 1;

        c->next = *head;
        *head = c;
        (*count)++;
    }
}

int conn_common_next_timeout(conn_t *head) {
    uint64_t now = now_ms();
    uint64_t min = UINT64_MAX;
    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->state == ST_PROXY)
            continue;
        if (c->deadline_ms < min)
            min = c->deadline_ms;
    }

    int proxy_ms = proxy_next_timeout_ms();
    if (proxy_ms >= 0 && (uint64_t)proxy_ms < min)
        min = (uint64_t)proxy_ms;

    if (min == UINT64_MAX)
        return 1000;
    if (min <= now)
        return 0;

    uint64_t d = min - now;
    return d > (uint64_t)INT_MAX ? INT_MAX : (int)d;
}

void conn_common_expire_timers(ev_loop_t *loop, conn_t *head,
                               const config_t *cfg) {
    uint64_t now = now_ms();
    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->closed || now < c->deadline_ms)
            continue;
        if (c->state == ST_PROXY)
            continue;

        if (c->state == ST_READ && c->in_len == 0 && c->requests > 0) {
            conn_common_close(loop, c);
            continue;
        }

        if (c->state == ST_READ || c->state == ST_READ_BODY) {
            if (!c->log_has_request)
                conn_common_log_begin(c, "-", "-");
            if (conn_common_build_error(c, HTTP_REQUEST_TIMEOUT, 1,
                                        conn_common_request_is_head(
                                            c->in, c->in_len),
                                        0, cfg->request_timeout_sec) < 0) {
                conn_common_close(loop, c);
                continue;
            }
            c->requests++;
            if (ev_mod(loop, c->fd, EV_WRITE, c) < 0)
                conn_common_close(loop, c);
            continue;
        }

        conn_common_close(loop, c);
    }
}

static void *shutdown_watchdog(void *arg) {
    int timeout_ms = *(int *)arg;
    free(arg);
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
    _exit(0);
    return NULL;
}

void conn_common_start_shutdown_watchdog(int timeout_ms) {
    pthread_t thread;
    int *arg = malloc(sizeof *arg);
    if (arg == NULL)
        return;
    *arg = timeout_ms;
    if (pthread_create(&thread, NULL, shutdown_watchdog, arg) == 0)
        pthread_detach(thread);
    else
        free(arg);
}

void conn_common_begin_drain(ev_loop_t *loop, int *listenfd, conn_t *head) {
    if (*listenfd >= 0) {
        (void)ev_del(loop, *listenfd);
        close(*listenfd);
        *listenfd = -1;
    }

    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->closed)
            continue;
        if (c->state == ST_READ || c->state == ST_READ_BODY)
            conn_common_close(loop, c);
        else
            c->close_after = 1;
    }
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
