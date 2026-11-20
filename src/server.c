#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int server_listen(int port)
{
    struct sockaddr_in addr;
    int fd, on = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 재시작 직후 TIME_WAIT 이 남아 있어도 같은 포트에 bind 할 수 있게 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}
//버퍼에서 \r\n\r\n 네 바이트를 찾아서, 그 바로 다음 위치(본문 시작)를 포인터로 돌려줌
const char *find_header_end(const char *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i + 4;
    }
    return NULL;
}

enum read_head_result read_request_head(int fd, char *buf, size_t cap,
                                        size_t *out_len)
{
    size_t total = 0;

    *out_len = 0;
    while (total < cap) {
        ssize_t n = read(fd, buf + total, cap - total);
        if (n < 0) {             // EINTR이면 재시도, 아니면 ERROR
            if (errno == EINTR)
                continue;
            return READ_HEAD_ERROR;
        }
        if (n == 0)               // 상대가 연결을 끊음(EOF)
            return READ_HEAD_CLOSED;

        /* 새로 받은 부분만 검사하되, 경계에 걸친 \r\n\r\n 을 위해 3바이트 겹침 */
        size_t from = total >= 3 ? total - 3 : 0;
        total += (size_t)n;
        *out_len = total;
        if (find_header_end(buf + from, total - from) != NULL)
            return READ_HEAD_OK;
    }
    return READ_HEAD_TOO_LARGE;
}

int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;   /* EPIPE 등: SIGPIPE 는 무시 중이라 여기로 온다 */
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

void server_handle(int conn_fd)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 6\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello\n";
    char buf[REQUEST_HEAD_MAX];
    size_t len;

    switch (read_request_head(conn_fd, buf, sizeof buf, &len)) {
    case READ_HEAD_OK:
        if (write_all(conn_fd, response, sizeof response - 1) < 0)
            perror("write");
        break;
    case READ_HEAD_CLOSED:
        fprintf(stderr, "client closed before end of headers (%zu bytes)\n",
                len);
        break;
    case READ_HEAD_ERROR:
        perror("read");
        break;
    case READ_HEAD_TOO_LARGE:
        fprintf(stderr, "request head exceeds %d bytes, closing\n",
                REQUEST_HEAD_MAX);
        break;
    }
    close(conn_fd);
}

void server_run(int listen_fd)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        char ip[INET_ADDRSTRLEN];
        int conn_fd;

        conn_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (conn_fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            continue;
        }

        if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip) == NULL)
            strcpy(ip, "?");
        fprintf(stderr, "accept %s:%u\n", ip, ntohs(peer.sin_port));

        server_handle(conn_fd);
    }
}
