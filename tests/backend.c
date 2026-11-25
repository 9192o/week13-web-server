#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HDR_MAX 16384
#define BODY_MAX (64 * 1024)

static int g_id;
static atomic_int g_connections;
static atomic_int g_requests;

static int ci_contains_str(const char *s, const char *needle) {
    size_t nlen = strlen(needle);
    for (; *s != '\0'; s++) {
        size_t i = 0;
        while (i < nlen &&
               tolower((unsigned char)s[i]) ==
                   tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return 1;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int read_all(int fd, char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf[n++] = c;
        if (n >= 2 && buf[n - 1] == '\n' && buf[n - 2] == '\r') {
            buf[n] = '\0';
            return (int)n;
        }
    }
    return -1;
}

static int find_header(const char *hdr, const char *name, char *out,
                       size_t out_cap) {
    size_t nlen = strlen(name);
    const char *p = strstr(hdr, "\r\n");
    if (p == NULL)
        return 0;
    p += 2;
    while (*p != '\0' && !(p[0] == '\r' && p[1] == '\n')) {
        const char *end = strstr(p, "\r\n");
        if (end == NULL)
            break;
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon != NULL) {
            size_t name_len = (size_t)(colon - p);
            if (name_len == nlen) {
                size_t i = 0;
                while (i < nlen &&
                       tolower((unsigned char)p[i]) ==
                           tolower((unsigned char)name[i]))
                    i++;
                if (i == nlen) {
                    const char *v = colon + 1;
                    while (v < end && (*v == ' ' || *v == '\t'))
                        v++;
                    size_t vlen = (size_t)(end - v);
                    if (vlen >= out_cap)
                        vlen = out_cap - 1;
                    memcpy(out, v, vlen);
                    out[vlen] = '\0';
                    return 1;
                }
            }
        }
        p = end + 2;
    }
    return 0;
}

static int read_chunked_body(int fd, char *body, size_t cap, size_t *out_len) {
    size_t total = 0;
    while (1) {
        char line[128];
        if (read_line(fd, line, sizeof line) < 0)
            return -1;
        char *semi = strchr(line, ';');
        if (semi != NULL)
            *semi = '\0';
        char *end = NULL;
        unsigned long long size = strtoull(line, &end, 16);
        if (end == line || (end != NULL && *end != '\0' && *end != '\r'))
            return -1;
        if (size == 0) {
            while (1) {
                if (read_line(fd, line, sizeof line) < 0)
                    return -1;
                if (strcmp(line, "\r\n") == 0)
                    break;
            }
            break;
        }
        if (total + (size_t)size > cap)
            return -1;
        if (read_all(fd, body + total, (size_t)size) < 0)
            return -1;
        total += (size_t)size;
        char crlf[2];
        if (read_all(fd, crlf, 2) < 0 || memcmp(crlf, "\r\n", 2) != 0)
            return -1;
    }
    *out_len = total;
    return 0;
}

static int read_request(int fd, char *hdr, size_t hdr_cap, char *body,
                        size_t body_cap, size_t *body_len, int *keep_alive,
                        char *method, size_t method_cap, char *target,
                        size_t target_cap) {
    size_t n = 0;
    while (n + 1 < hdr_cap) {
        ssize_t r = recv(fd, hdr + n, 1, 0);
        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        n++;
        if (n >= 4 && memcmp(hdr + n - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    if (n < 4 || memcmp(hdr + n - 4, "\r\n\r\n", 4) != 0)
        return -1;
    hdr[n] = '\0';

    char *line_end = strstr(hdr, "\r\n");
    if (line_end == NULL)
        return -1;

    char line[2048];
    size_t line_len = (size_t)(line_end - hdr);
    if (line_len >= sizeof line)
        return -1;
    memcpy(line, hdr, line_len);
    line[line_len] = '\0';

    char *sp1 = strchr(line, ' ');
    if (sp1 == NULL)
        return -1;
    *sp1 = '\0';
    char *tgt = sp1 + 1;
    char *sp2 = strchr(tgt, ' ');
    if (sp2 == NULL)
        return -1;
    *sp2 = '\0';
    const char *version = sp2 + 1;

    snprintf(method, method_cap, "%s", line);
    snprintf(target, target_cap, "%s", tgt);

    *keep_alive = strcmp(version, "HTTP/1.1") == 0;
    char conn[128];
    if (find_header(hdr, "Connection", conn, sizeof conn)) {
        if (ci_contains_str(conn, "close"))
            *keep_alive = 0;
        else if (ci_contains_str(conn, "keep-alive"))
            *keep_alive = 1;
    }

    *body_len = 0;
    char cl[64];
    if (find_header(hdr, "Content-Length", cl, sizeof cl)) {
        size_t want = (size_t)strtoull(cl, NULL, 10);
        if (want > body_cap)
            return -1;
        if (want > 0 && read_all(fd, body, want) < 0)
            return -1;
        *body_len = want;
        return 0;
    }

    char te[128];
    if (find_header(hdr, "Transfer-Encoding", te, sizeof te) &&
        ci_contains_str(te, "chunked")) {
        return read_chunked_body(fd, body, body_cap, body_len);
    }

    return 0;
}

static const char *reason_for(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    default: return "Status";
    }
}

static int send_response(int fd, int status, const char *content_type,
                         const char *body, size_t body_len, int keep_alive,
                         const char *extra) {
    char hdr[1024];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "\r\n",
                     status, reason_for(status),
                     content_type ? content_type : "text/plain",
                     body_len, keep_alive ? "keep-alive" : "close",
                     extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof hdr)
        return -1;
    if (send_all(fd, hdr, (size_t)n) < 0)
        return -1;
    if (body_len > 0 && send_all(fd, body, body_len) < 0)
        return -1;
    return 0;
}

static int send_chunked(int fd, int keep_alive) {
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    if (send_all(fd, hdr, strlen(hdr)) < 0)
        return -1;
    const char *chunks =
        "5\r\nhello\r\n"
        "6\r\n world\r\n"
        "1\r\n!\r\n"
        "0\r\n\r\n";
    (void)keep_alive;
    return send_all(fd, chunks, strlen(chunks));
}

static const unsigned char gzip_body[] = {
    0x1f, 0x8b, 0x08, 0x00, 0xcf, 0x96, 0x05, 0x6b, 0x00, 0x03,
    0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x48, 0xaf, 0xca, 0x2c,
    0xe0, 0x02, 0x00, 0x39, 0x7c, 0x63, 0x56, 0x0b, 0x00, 0x00,
    0x00
};

static int handle_one(int fd) {
    char hdr[HDR_MAX];
    char body[BODY_MAX];
    char method[16];
    char target[2048];
    size_t body_len = 0;
    int keep_alive = 0;

    if (read_request(fd, hdr, sizeof hdr, body, sizeof body, &body_len,
                     &keep_alive, method, sizeof method,
                     target, sizeof target) < 0)
        return -1;

    atomic_fetch_add(&g_requests, 1);

    char path[2048];
    char *query = NULL;
    snprintf(path, sizeof path, "%s", target);
    char *q = strchr(path, '?');
    if (q != NULL) {
        *q = '\0';
        query = q + 1;
    }

    /* Test backend accepts both /api/... and /... forms. */
    if (strncmp(path, "/api", 4) == 0 &&
        (path[4] == '\0' || path[4] == '/')) {
        size_t rest = strlen(path + 4);
        memmove(path, path + 4, rest + 1);
        if (path[0] == '\0')
            snprintf(path, sizeof path, "/");
    }

    if (strcmp(path, "/close") == 0)
        return -1;

    if (strcmp(path, "/close-cl") == 0) {
        const char *r =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 11\r\n"
            "Connection: close\r\n"
            "\r\n"
            "closed-body";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/close-delimited") == 0) {
        const char *r =
            "HTTP/1.1 200 OK\r\n"
            "Connection: close\r\n"
            "\r\n"
            "close-delimited";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/close-chunked") == 0) {
        const char *r =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n"
            "\r\n"
            "5\r\nhello\r\n0\r\n\r\n";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/interim") == 0) {
        const char *r =
            "HTTP/1.1 103 Early Hints\r\n"
            "\r\n"
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "Connection: close\r\n"
            "\r\n"
            "ok";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/hopresp") == 0) {
        const char *r =
            "HTTP/1.1 200 OK\r\n"
            "Connection: X-Hop, close\r\n"
            "X-Hop: secret\r\n"
            "Content-Length: 2\r\n"
            "\r\n"
            "ok";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/drip") == 0) {
        const char *hdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 12\r\n"
            "Connection: close\r\n"
            "\r\n";
        if (send_all(fd, hdr, strlen(hdr)) < 0)
            return -1;
        for (int i = 0; i < 12; i++) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 400000000L;
            nanosleep(&ts, NULL);
            if (send_all(fd, "x", 1) < 0)
                return -1;
        }
        return -1;
    }

    if (strcmp(path, "/http10") == 0) {
        const char *r =
            "HTTP/1.0 200 OK\r\n"
            "Content-Length: 2\r\n"
            "\r\n"
            "ok";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/reset205") == 0) {
        const char *r =
            "HTTP/1.1 205 Reset Content\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/head") == 0) {
        const char *r =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "Connection: close\r\n"
            "\r\n";
        (void)send_all(fd, r, strlen(r));
        if (strcmp(method, "HEAD") != 0)
            (void)send_all(fd, "ok", 2);
        return -1;
    }

    if (strcmp(path, "/no-content204") == 0) {
        const char *r =
            "HTTP/1.1 204 No Content\r\n"
            "Connection: close\r\n"
            "\r\n";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/not-modified304") == 0) {
        const char *r =
            "HTTP/1.1 304 Not Modified\r\n"
            "Connection: close\r\n"
            "\r\n";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/empty200") == 0) {
        const char *r =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/chunked") == 0)
        return send_chunked(fd, keep_alive);

    if (strcmp(path, "/trailer") == 0) {
        const char *r =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Trailer: X-Sum\r\n"
            "Connection: close\r\n"
            "\r\n"
            "5\r\nhello\r\n"
            "0\r\nX-Sum: 42\r\n\r\n";
        (void)send_all(fd, r, strlen(r));
        return -1;
    }

    if (strcmp(path, "/gzip") == 0) {
        return send_response(fd, 200, "text/plain",
                             (const char *)gzip_body, sizeof gzip_body,
                             keep_alive,
                             "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
    }

    if (strcmp(path, "/health") == 0) {
        const char *ok = "ok\n";
        return send_response(fd, 200, "text/plain", ok, strlen(ok),
                             keep_alive, NULL);
    }

    if (strcmp(path, "/stats") == 0) {
        char out[128];
        int n = snprintf(out, sizeof out, "connections=%d requests=%d\n",
                         atomic_load(&g_connections),
                         atomic_load(&g_requests));
        return send_response(fd, 200, "text/plain", out, (size_t)n,
                             keep_alive, NULL);
    }

    if (strcmp(path, "/id") == 0) {
        char out[64];
        int n = snprintf(out, sizeof out, "id=%d\n", g_id);
        return send_response(fd, 200, "text/plain", out, (size_t)n,
                             keep_alive, NULL);
    }

    if (strcmp(path, "/slow") == 0) {
        long ms = 1000;
        if (query != NULL && strncmp(query, "ms=", 3) == 0)
            ms = strtol(query + 3, NULL, 10);
        if (ms > 0) {
            struct timespec ts;
            ts.tv_sec = ms / 1000;
            ts.tv_nsec = (ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
        return send_response(fd, 200, "text/plain", "slow\n", 5,
                             keep_alive, NULL);
    }

    if (strcmp(path, "/status") == 0) {
        int code = 200;
        if (query != NULL && strncmp(query, "code=", 5) == 0)
            code = (int)strtol(query + 5, NULL, 10);
        char out[64];
        int n = snprintf(out, sizeof out, "status=%d\n", code);
        return send_response(fd, code, "text/plain", out, (size_t)n,
                             keep_alive, NULL);
    }

    if (strcmp(path, "/big") == 0) {
        size_t len = 1024 * 1024;
        char *big = malloc(len);
        if (big == NULL)
            return -1;
        memset(big, 'x', len);
        int rc = send_response(fd, 200, "application/octet-stream",
                               big, len, keep_alive, NULL);
        free(big);
        return rc;
    }

    if (strcmp(path, "/echo") == 0) {
        char out[8192];
        char host[256] = "";
        char xff[512] = "";
        char conn[128] = "";
        char keepalive[128] = "";
        char xhop[128] = "";
        (void)find_header(hdr, "Host", host, sizeof host);
        (void)find_header(hdr, "X-Forwarded-For", xff, sizeof xff);
        (void)find_header(hdr, "Connection", conn, sizeof conn);
        (void)find_header(hdr, "Keep-Alive", keepalive, sizeof keepalive);
        (void)find_header(hdr, "X-Hop", xhop, sizeof xhop);
        int n = snprintf(out, sizeof out,
                         "id=%d\nMETHOD=%s\nPATH=%s\nQUERY=%s\n"
                         "HOST=%s\nXFF=%s\nCONN=%s\nKEEPALIVE=%s\nXHOP=%s\nBODY=",
                         g_id, method, path, query ? query : "",
                         host, xff, conn, keepalive, xhop);
        if (n < 0)
            return -1;
        size_t used = (size_t)n;
        if (used > sizeof out)
            used = sizeof out;
        size_t copy = body_len;
        if (used + copy >= sizeof out)
            copy = sizeof out - used - 1;
        memcpy(out + used, body, copy);
        used += copy;
        out[used++] = '\n';
        return send_response(fd, 200, "text/plain", out, used,
                             keep_alive, NULL);
    }

    {
        const char *nf = "not found\n";
        return send_response(fd, 404, "text/plain", nf, strlen(nf),
                             keep_alive, NULL);
    }
}

typedef struct {
    int fd;
} client_arg_t;

static void *client_thread(void *arg) {
    client_arg_t *client = arg;
    int fd = client->fd;
    free(client);

    for (;;) {
        if (handle_one(fd) < 0)
            break;
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port> [id]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    g_id = argc > 2 ? atoi(argv[2]) : 0;
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen");
        return 1;
    }

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int c = accept(fd, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        atomic_fetch_add(&g_connections, 1);

        client_arg_t *arg = malloc(sizeof *arg);
        if (arg == NULL) {
            close(c);
            continue;
        }
        arg->fd = c;
        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread, arg) == 0) {
            pthread_detach(thread);
        } else {
            free(arg);
            close(c);
        }
    }

    close(fd);
    return 0;
}
