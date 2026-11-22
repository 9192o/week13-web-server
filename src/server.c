#include "server.h"
#include "cgi.h"
#include "http.h"
#include "static_file.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static int setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: accept/read can return EINTR */

    if (sigaction(SIGINT, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return -1;

    return 0;
}

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }

    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

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

static void set_recv_timeout(int fd, int seconds) {
    if (seconds <= 0)
        return;

    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static void handle_connection(int connfd, const struct sockaddr_in *peer,
                              const config_t *cfg) {
    char peer_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &peer->sin_addr, peer_ip, sizeof peer_ip) == NULL)
        snprintf(peer_ip, sizeof peer_ip, "?");
    log_error("connection from %s:%u", peer_ip, ntohs(peer->sin_port));

    char buf[16384];
    size_t max_len = cfg->max_header_bytes;
    if (max_len > sizeof buf)
        max_len = sizeof buf;
    if (max_len == 0)
        max_len = 1;

    size_t pending = 0;
    int requests = 0;

    for (;;) {
        if (g_stop)
            return;

        size_t len = pending;
        pending = 0;

        while (!has_header_end(buf, len) && len < max_len) {
            int timeout = (len == 0 && requests > 0)
                              ? cfg->keepalive_idle_sec
                              : cfg->request_timeout_sec;
            set_recv_timeout(connfd, timeout);

            ssize_t n = read(connfd, buf + len, max_len - len);
            if (n < 0) {
                if (errno == EINTR) {
                    if (g_stop)
                        return;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (len == 0 && requests > 0)
                        return; /* idle keep-alive timeout */
                    (void)http_response_send_error(
                        connfd, HTTP_REQUEST_TIMEOUT, 1);
                    return;
                }
                log_error("read: %s", strerror(errno));
                return;
            }
            if (n == 0)
                return; /* client closed */

            len += (size_t)n;
        }

        if (len == 0)
            return;

        if (!has_header_end(buf, len)) {
            int status = (len >= max_len) ? HTTP_HEADERS_TOO_LARGE
                                          : HTTP_BAD_REQUEST;
            (void)http_response_send_error(connfd, status, 1);
            return;
        }

        http_request_t req;
        http_request_init(&req);

        http_status_t st = http_request_parse_buffer(buf, len, cfg, &req);
        if (st == HTTP_PARSE_INCOMPLETE)
            st = HTTP_BAD_REQUEST;

        if (st == HTTP_OK)
            st = http_request_read_body(connfd, buf, len, cfg, &req, &g_stop);

        if (st != HTTP_OK) {
            if (!g_stop)
                (void)http_response_send_error(connfd, (int)st, 1);
            http_request_free(&req);
            return;
        }

        requests++;
        int keep = req.keep_alive &&
                   (requests < cfg->keepalive_max_requests);
        int close_after = !keep;

        if (req.method == HTTP_OPTIONS) {
            (void)http_response_send_options(connfd, close_after);
        } else if (cfg->cgi_dir[0] != '\0' &&
                   strncmp(req.path, "/cgi-bin/", 9) == 0) {
            (void)cgi_handle(connfd, &req, cfg, peer_ip, close_after);
        } else {
            (void)static_file_handle(connfd, &req, cfg, close_after);
        }

        size_t body_in_buf = 0;
        if (req.body_len > 0 && len > req.header_len) {
            size_t available = len - req.header_len;
            body_in_buf = available < req.body_len ? available : req.body_len;
        }

        size_t consumed = req.header_len + body_in_buf;
        if (consumed > len)
            consumed = len;

        size_t leftover = len - consumed;
        if (leftover > 0)
            memmove(buf, buf + consumed, leftover);
        pending = leftover;

        http_request_free(&req);

        if (!keep)
            return;
    }
}

int server_run(const config_t *cfg) {
    if (setup_signals() < 0) {
        log_error("signal setup failed: %s", strerror(errno));
        return -1;
    }

    int listenfd = create_listen_socket(cfg->port);
    if (listenfd < 0)
        return -1;

    log_error("listening on 0.0.0.0:%d (docroot: %s)", cfg->port, cfg->docroot);

    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof peer;
        int connfd = accept(listenfd, (struct sockaddr *)&peer, &peerlen);

        if (connfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE) {
                log_error("accept: %s", strerror(errno));
                sleep(1);
                continue;
            }
            log_error("accept: %s", strerror(errno));
            break;
        }

        handle_connection(connfd, &peer, cfg);
        close(connfd);
    }

    close(listenfd);
    log_error("server stopped");
    return 0;
}

void server_request_stop(void) {
    g_stop = 1;
}
