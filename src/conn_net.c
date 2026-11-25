#include "conn_common.h"

#include "logger.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int conn_common_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int conn_common_set_cloexec(int fd) {
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
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
