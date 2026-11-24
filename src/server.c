#include "server.h"
#include "access_log.h"
#include "cgi.h"
#include "http.h"
#include "static_file.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
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
    if (g_stop)
        _exit(1); /* second SIGINT/SIGTERM: force exit */
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

static int has_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return 1;
    }
    return 0;
}

static int request_is_head(const char *buf, size_t len) {
    return len >= 4 && memcmp(buf, "HEAD", 4) == 0 &&
           (len == 4 || buf[4] == ' ');
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

static void set_send_timeout(int fd, int seconds) {
    if (seconds <= 0)
        return;

    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static void handle_connection(int connfd, const struct sockaddr_in *peer,
                              const config_t *cfg) {
    char peer_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &peer->sin_addr, peer_ip, sizeof peer_ip) == NULL)
        snprintf(peer_ip, sizeof peer_ip, "?");
    log_error("connection from %s:%u", peer_ip, ntohs(peer->sin_port));

    access_log_t log;
    access_log_set_current(NULL);
    set_send_timeout(connfd, cfg->request_timeout_sec);

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
        int idle_phase = (len == 0 && requests > 0);
        uint64_t deadline = now_ms() +
            (uint64_t)(idle_phase ? cfg->keepalive_idle_sec
                                  : cfg->request_timeout_sec) * 1000;

        while (!has_header_end(buf, len) && len < max_len) {
            uint64_t now = now_ms();
            if (now >= deadline) {
                if (len == 0 && requests > 0)
                    return; /* idle keep-alive timeout */
                (void)http_response_send_error_ex(
                    connfd, HTTP_REQUEST_TIMEOUT, 1,
                    request_is_head(buf, len));
                return;
            }

            uint64_t remaining = deadline - now;
            int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;

            struct pollfd pfd;
            pfd.fd = connfd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int pr = poll(&pfd, 1, timeout);
            if (pr < 0) {
                if (errno == EINTR) {
                    if (g_stop)
                        return;
                    continue;
                }
                log_error("poll: %s", strerror(errno));
                return;
            }
            if (pr == 0) {
                if (len == 0 && requests > 0)
                    return; /* idle keep-alive timeout */
                (void)http_response_send_error_ex(
                    connfd, HTTP_REQUEST_TIMEOUT, 1,
                    request_is_head(buf, len));
                lingering_close(connfd);
                return;
            }

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
                    (void)http_response_send_error_ex(
                        connfd, HTTP_REQUEST_TIMEOUT, 1,
                        request_is_head(buf, len));
                    lingering_close(connfd);
                    return;
                }
                log_error("read: %s", strerror(errno));
                return;
            }
            if (n == 0)
                return; /* client closed */

            len += (size_t)n;
            if (idle_phase) {
                idle_phase = 0;
                deadline = now_ms() +
                    (uint64_t)cfg->request_timeout_sec * 1000;
            }
        }

        if (len == 0)
            return;

        if (!has_header_end(buf, len)) {
            int status = HTTP_BAD_REQUEST;
            if (len >= max_len) {
                if (!has_crlf(buf, len) ||
                    request_line_uri_too_long(buf, len, cfg->max_uri))
                    status = HTTP_URI_TOO_LONG;
                else
                    status = HTTP_HEADERS_TOO_LARGE;
            }
            (void)http_response_send_error_ex(
                connfd, status, 1, request_is_head(buf, len));
            lingering_close(connfd);
            return;
        }

        http_request_t req;
        http_request_init(&req);
        access_log_init(&log, peer_ip);
        access_log_set_current(&log);

        http_status_t st = http_request_parse_buffer(buf, len, cfg, &req);
        if (st == HTTP_PARSE_INCOMPLETE)
            st = HTTP_BAD_REQUEST;

        if (req.method_str[0] != '\0' || req.path[0] != '\0')
            access_log_set_request(&log, req.method_str, req.path);

        int is_cgi = cfg->cgi_dir[0] != '\0' &&
                     strncmp(req.path, "/cgi-bin/", 9) == 0;
        int need_body = is_cgi && req.method != HTTP_OPTIONS;
        size_t buffered = len > req.header_len ? len - req.header_len : 0;

        if (st == HTTP_OK && need_body) {
            if (req.expect_continue && buffered < req.content_length) {
                if (writen(connfd, "HTTP/1.1 100 Continue\r\n\r\n", 25) < 0) {
                    access_log_end(&log);
                    access_log_set_current(NULL);
                    http_request_free(&req);
                    return;
                }
            }
            st = http_request_read_body(connfd, buf, len, cfg, &req,
                                        deadline, &g_stop);
        }

        if (st != HTTP_OK) {
            if (!g_stop) {
                (void)http_response_send_error_ex(
                    connfd, (int)st, 1, req.method == HTTP_HEAD);
                lingering_close(connfd);
            }
            access_log_end(&log);
            access_log_set_current(NULL);
            http_request_free(&req);
            return;
        }

        requests++;
        int keep = req.keep_alive &&
                   (requests < cfg->keepalive_max_requests) &&
                   !g_stop;

        /*
         * Static/OPTIONS requests do not need the body. If it is already in
         * the header buffer we can consume it; otherwise close after the
         * response instead of reading a potentially huge upload.
         */
        int unread_body = 0;
        if (!need_body && req.content_length > 0) {
            if (buffered >= req.content_length)
                req.body_len = req.content_length;
            else {
                keep = 0;
                unread_body = 1;
            }
        }

        int close_after = !keep;

        handler_result_t handler_rc = HANDLER_OK;

        if (req.method == HTTP_OPTIONS) {
            if (http_response_send_options(connfd, close_after) < 0)
                handler_rc = HANDLER_ERROR;
        } else if (is_cgi) {
            handler_rc = cgi_handle(connfd, &req, cfg, peer_ip, close_after);
        } else {
            handler_rc = static_file_handle(connfd, &req, cfg, close_after);
        }

        if ((unread_body ||
             (handler_rc == HANDLER_CLOSE &&
              access_log_get_status(&log) >= 400)) &&
            handler_rc != HANDLER_ERROR)
            lingering_close(connfd);
        access_log_end(&log);
        access_log_set_current(NULL);

        /*
         * HANDLER_CLOSE: the handler sent a Connection: close response.
         * HANDLER_ERROR: the response is incomplete (write timeout, client
         * gone, file shrank). In both cases do not read another request.
         */
        if (handler_rc != HANDLER_OK)
            keep = 0;

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

        if (!keep || g_stop)
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
