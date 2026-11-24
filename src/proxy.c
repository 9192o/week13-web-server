#include "proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * 백엔드 하나 "host:port" (s[0..len)) 를 해석한다. spec 은 에러 메시지용 규칙 원문.
 * 성공 0, 실패 -1.
 */
static int parse_backend(const char *spec, const char *s, size_t len,
                         struct proxy_backend *b, char *errbuf, size_t errlen)
{
    char item[PROXY_NAME_MAX], *colon, *end;
    struct addrinfo hints, *res = NULL;
    long port;
    int gr;

    if (len == 0) {
        snprintf(errbuf, errlen, "proxy rule %s: empty backend", spec);
        return -1;
    }
    if (len >= sizeof item) {
        snprintf(errbuf, errlen, "proxy rule %s: backend too long", spec);
        return -1;
    }
    memcpy(item, s, len);
    item[len] = '\0';

    colon = strrchr(item, ':');
    if (colon == NULL || colon == item) {
        snprintf(errbuf, errlen, "proxy rule %s: expected host:port, got %s", spec, item);
        return -1;
    }
    errno = 0;
    port = strtol(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' || port < 1 || port > 65535) {
        snprintf(errbuf, errlen, "proxy rule %s: invalid port in %s", spec, item);
        return -1;
    }
    snprintf(b->name, sizeof b->name, "%s", item);
    *colon = '\0';                      /* item = 호스트만 */

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;          /* IPv6 는 나중 단위 */
    hints.ai_socktype = SOCK_STREAM;
    gr = getaddrinfo(item, NULL, &hints, &res);
    if (gr != 0 || res == NULL) {
        snprintf(errbuf, errlen, "proxy rule %s: cannot resolve %s: %s", spec, item,
                 gr != 0 ? gai_strerror(gr) : "no address");
        if (res != NULL)
            freeaddrinfo(res);
        return -1;
    }
    memcpy(&b->addr, res->ai_addr, sizeof b->addr);
    b->addr.sin_port = htons((unsigned short)port);
    freeaddrinfo(res);
    return 0;
}

int proxy_parse_rule(const char *spec, struct proxy_route *out,
                     char *errbuf, size_t errlen)
{
    const char *eq = strchr(spec, '=');
    const char *p;
    size_t plen;

    if (eq == NULL) {
        snprintf(errbuf, errlen, "proxy rule %s: expected prefix=host:port[,host:port...]",
                 spec);
        return -1;
    }
    plen = (size_t)(eq - spec);
    if (plen == 0 || spec[0] != '/') {
        snprintf(errbuf, errlen, "proxy rule %s: prefix must start with '/'", spec);
        return -1;
    }
    if (plen >= PROXY_PREFIX_MAX) {
        snprintf(errbuf, errlen, "proxy rule %s: prefix too long (max %d)", spec,
                 PROXY_PREFIX_MAX - 1);
        return -1;
    }

    memset(out, 0, sizeof *out);
    memcpy(out->prefix, spec, plen);
    out->prefix[plen] = '\0';
    atomic_init(&out->next, 0);

    for (p = eq + 1; ; p++) {
        size_t n = strcspn(p, ",");

        if (out->nbackends == PROXY_MAX_BACKENDS) {
            snprintf(errbuf, errlen, "proxy rule %s: too many backends (max %d)", spec,
                     PROXY_MAX_BACKENDS);
            return -1;
        }
        if (parse_backend(spec, p, n, &out->backends[out->nbackends], errbuf, errlen) < 0)
            return -1;
        out->nbackends++;
        p += n;
        if (*p == '\0')
            break;
    }
    return 0;
}

struct proxy_route *proxy_match(struct proxy_table *t, const char *path)
{
    struct proxy_route *best = NULL;
    size_t best_len = 0;
    int i;

    for (i = 0; i < t->count; i++) {
        size_t n = strlen(t->routes[i].prefix);
        if (strncmp(path, t->routes[i].prefix, n) == 0 &&
            (best == NULL || n > best_len)) {
            best = &t->routes[i];
            best_len = n;
        }
    }
    return best;
}

const struct proxy_backend *proxy_pick(struct proxy_route *r)
{
    /* 순서만 맞으면 되고 다른 메모리와 동기화할 것이 없으므로 relaxed */
    unsigned long long k = atomic_fetch_add_explicit(&r->next, 1, memory_order_relaxed);

    return &r->backends[k % (unsigned long long)r->nbackends];
}

const struct proxy_backend *proxy_next(const struct proxy_route *r,
                                       const struct proxy_backend *b)
{
    int i = (int)(b - r->backends);

    return &r->backends[(i + 1) % r->nbackends];
}

const struct proxy_backend *proxy_find_self(const struct proxy_route *r,
                                            int listen_port)
{
    int i;

    for (i = 0; i < r->nbackends; i++) {
        const struct proxy_backend *b = &r->backends[i];
        unsigned long a = ntohl(b->addr.sin_addr.s_addr);

        if (ntohs(b->addr.sin_port) == listen_port && ((a >> 24) == 127 || a == 0))
            return b;
    }
    return NULL;
}

/* list(쉼표로 나뉜 토큰, 앞뒤 SP/HTAB 무시)에 name 이 있으면 1 (대소문자 무시) */
static int token_in_list(const char *list, const char *name)
{
    size_t nlen = strlen(name);
    const char *p = list;

    while (*p != '\0') {
        const char *s, *e;

        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        s = p;
        while (*p != '\0' && *p != ',')
            p++;
        e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        if ((size_t)(e - s) == nlen && strncasecmp(s, name, nlen) == 0)
            return 1;
    }
    return 0;
}

/*
 * 다음 홉으로 넘기면 안 되는 헤더인지: 고정 목록(RFC 9110 7.6.1) 또는
 * Connection 헤더에 이름이 적힌 헤더.
 */
static int is_hop_by_hop(const struct http_header *h, size_t n, const char *name)
{
    static const char *const fixed[] = {
        "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Upgrade"
    };
    size_t i;

    for (i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        if (strcasecmp(name, fixed[i]) == 0)
            return 1;
    for (i = 0; i < n; i++)
        if (strcasecmp(h[i].name, "Connection") == 0 && token_in_list(h[i].value, name))
            return 1;
    return 0;
}

/* out[*off..cap) 에 printf. 부족하면 -1 */
static int append(char *out, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*off >= cap)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(out + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *off)
        return -1;
    *off += (size_t)n;
    return 0;
}

int proxy_build_request(const struct http_request *req, char *out, size_t cap)
{
    size_t off = 0, i;

    if (append(out, cap, &off, "%s %s%s%s HTTP/1.0\r\n", req->method_str, req->path,
               req->query ? "?" : "", req->query ? req->query : "") < 0)
        return -1;
    for (i = 0; i < req->header_count; i++) {
        const struct http_header *h = &req->headers[i];
        if (is_hop_by_hop(req->headers, req->header_count, h->name) ||
            strcasecmp(h->name, "Content-Length") == 0)
            continue;
        if (append(out, cap, &off, "%s: %s\r\n", h->name, h->value) < 0)
            return -1;
    }
    if (append(out, cap, &off, "Connection: close\r\n\r\n") < 0)
        return -1;
    return (int)off;
}

/* 앞뒤 SP/HTAB 를 잘라 낸 시작 포인터 (끝은 NUL 로 자름) */
static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    *e = '\0';
    return s;
}

/* 1 이상 18 자리 이하 10진수 전체면 값, 아니면 -1 */
static long long parse_length(const char *s)
{
    long long v = 0;
    size_t i;

    if (s[0] == '\0' || strlen(s) > 18)
        return -1;
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* 헤더 이름에 쓸 수 없는 바이트 (공백, 제어 문자, 구분자 일부) */
static int bad_name_char(unsigned char c)
{
    return c <= 0x20 || c >= 0x7f || strchr("\"(),/:;<=>?@[\\]{}", c) != NULL;
}

const char *proxy_parse_response(char *buf, size_t len, struct proxy_response *r)
{
    char *line, *end, *head_end = NULL;
    long long cl = -1;
    size_t i;

    for (i = 0; i + 4 <= len; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
            head_end = buf + i;
            break;
        }
    }
    if (head_end == NULL)
        return "no header end";
    if (memchr(buf, 0, (size_t)(head_end - buf)) != NULL)
        return "NUL in header";
    memset(r, 0, sizeof *r);
    r->head_len = (size_t)(head_end - buf) + 4;
    head_end[2] = '\0';                 /* 마지막 헤더 줄의 \r\n 뒤를 끝으로 */

    /* 상태 줄: HTTP/1.x SSS[ reason] */
    line = buf;
    end = strstr(line, "\r\n");
    *end = '\0';
    if (strncmp(line, "HTTP/1.", 7) != 0 || line[7] < '0' || line[7] > '9' ||
        line[8] != ' ' ||
        line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' || (line[12] != ' ' && line[12] != '\0'))
        return "bad status line";
    r->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (r->status < 200)
        return "1xx response";
    r->reason = line[12] == ' ' ? line + 13 : line + 12;
    if (strpbrk(r->reason, "\r\n") != NULL)   /* 줄 안에 남은 CR/LF 는 응답 분할 위험 */
        return "bad status line";

    for (line = end + 2; *line != '\0'; line = end + 2) {
        char *colon, *name;

        end = strstr(line, "\r\n");
        *end = '\0';
        if (line[0] == ' ' || line[0] == '\t')
            return "obs-fold header";
        if (strpbrk(line, "\r\n") != NULL)
            return "bad header line";
        colon = strchr(line, ':');
        if (colon == NULL || colon == line)
            return "bad header line";
        *colon = '\0';
        name = line;
        for (i = 0; name[i] != '\0'; i++)
            if (bad_name_char((unsigned char)name[i]))
                return "bad header name";
        if (r->header_count == HTTP_MAX_HEADERS)
            return "too many headers";
        r->headers[r->header_count].name = name;
        r->headers[r->header_count].value = trim(colon + 1);
        r->header_count++;
    }

    for (i = 0; i < r->header_count; i++) {
        const struct http_header *h = &r->headers[i];

        if (strcasecmp(h->name, "Transfer-Encoding") == 0)
            return "transfer-encoding in HTTP/1.0 response";
        if (strcasecmp(h->name, "Content-Length") == 0) {
            long long v = parse_length(h->value);
            if (v < 0)
                return "bad content-length";
            if (cl >= 0 && cl != v)
                return "conflicting content-length";
            cl = v;
        }
    }
    r->body_len = (r->status == 204 || r->status == 304) ? 0 : cl;
    return NULL;
}

int proxy_format_response_head(const struct proxy_response *r, int keep_alive,
                               char *out, size_t cap)
{
    size_t off = 0, i;

    if (append(out, cap, &off, "HTTP/1.1 %d %s\r\n", r->status, r->reason) < 0)
        return -1;
    for (i = 0; i < r->header_count; i++) {
        const struct http_header *h = &r->headers[i];
        if (is_hop_by_hop(r->headers, r->header_count, h->name))
            continue;
        if (append(out, cap, &off, "%s: %s\r\n", h->name, h->value) < 0)
            return -1;
    }
    if (!keep_alive && append(out, cap, &off, "Connection: close\r\n") < 0)
        return -1;
    if (append(out, cap, &off, "\r\n") < 0)
        return -1;
    return (int)off;
}

int proxy_connect(const struct proxy_backend *b)
{
    int fd, e;

#ifdef SOCK_CLOEXEC
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
#else
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        e = errno;
        close(fd);
        errno = e;
        return -1;
    }
#endif
    /* 워커는 신호를 막아 두므로 EINTR 은 오지 않는다 (오면 실패로 처리) */
    if (connect(fd, (const struct sockaddr *)&b->addr, sizeof b->addr) < 0) {
        e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}
