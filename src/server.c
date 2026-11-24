#include "server.h"
#include "body.h"
#include "cgi.h"
#include "http_parser.h"
#include "http_response.h"
#include "pool.h"
#include "queue.h"
#include "static.h"
#include "url.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
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
    /* CGI 자식이 listen 소켓을 물려받지 않게 */
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        perror("fcntl(FD_CLOEXEC)");
        close(fd);
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

long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * deadline(now_ms 기준)까지 fd 에 events 가 오길 기다린다.
 * 반환: 1 준비됨(오류·끊김 포함 — 이어지는 read/send 가 알려줌), 0 시간 초과, -1 poll 오류.
 */
static int wait_event(int fd, short events, long long deadline)
{
    for (;;) {
        long long left = deadline - now_ms();
        struct pollfd p;
        int r;

        if (left <= 0)
            return 0;
        p.fd = fd;
        p.events = events;
        p.revents = 0;
        r = poll(&p, 1, (int)left);
        if (r < 0) {
            if (errno == EINTR)
                continue;      /* 남은 시간을 다시 계산해서 재시도 */
            return -1;
        }
        return r;
    }
}

/* 반환: 1 읽기 가능(데이터, EOF, 오류 모두 — read 가 알려줌), 0 시간 초과, -1 poll 오류. */
int wait_readable(int fd, long long deadline)
{
    return wait_event(fd, POLLIN, deadline);
}

enum read_head_result read_request_head(int fd, char *buf, size_t cap,
                                        int timeout_ms, size_t *out_len)
{
    long long deadline = now_ms() + timeout_ms;
    size_t total = 0;

    *out_len = 0;
    while (total < cap) {
        int ready = wait_readable(fd, deadline);
        if (ready == 0)
            return READ_HEAD_TIMEOUT;
        if (ready < 0)
            return READ_HEAD_ERROR;

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

enum send_result send_all(int fd, const void *buf, size_t len, long long deadline)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n;

        /* 매번 확인: 버퍼가 조금씩 계속 비워져도 응답 전체 시간은 넘기지 않는다 */
        if (now_ms() >= deadline)
            return SEND_TIMEOUT;
        n = send(fd, p, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 소켓 버퍼가 가득 참: 자리가 날 때까지 남은 시간만큼만 기다림 */
                if (wait_event(fd, POLLOUT, deadline) < 0)
                    return SEND_ERROR;
                continue;      /* 시간 초과였다면 위의 deadline 검사에서 걸림 */
            }
            return SEND_ERROR; /* EPIPE 등 */
        }
        p += n;
        len -= (size_t)n;
    }
    return SEND_OK;
}

/* send_all 실패를 접근 로그 비고로 */
static const char *send_fail_note(int r)
{
    return r == SEND_TIMEOUT ? "(send timeout)" : "(send failed)";
}

/*
 * 에러 응답 뒤 RST 로 응답이 날아가지 않도록: 쓰기 방향만 닫고(FIN),
 * 클라이언트가 아직 보내는 데이터를 한도(바이트/시간)까지 읽어 버린 뒤 close.
 */
static void lingering_close(int fd)
{
    long long deadline = now_ms() + LINGER_TIMEOUT_MS;
    char junk[4096];
    size_t total = 0;

    if (shutdown(fd, SHUT_WR) == 0) {
        while (total < LINGER_MAX_BYTES && wait_readable(fd, deadline) == 1) {
            ssize_t n = read(fd, junk, sizeof junk);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            total += (size_t)n;
        }
    }
    close(fd);
}

/*
 * 에러 페이지(헤더 + HTML 본문)를 보낸다.
 * extra 는 NULL 또는 추가 헤더 줄들 (301 의 Location, 405 의 Allow).
 * 반환: 보낸 본문 바이트 수. 실패면 음수 (호출자는 그냥 닫음):
 *   SEND_TIMEOUT 시간 초과, -1 쓰기 실패나 생성기 버그.
 */
static int send_error(int fd, int status, const char *extra)
{
    char head[HTTP_URI_MAX + 512], body[512];   /* Location 에 URI 가 통째로 들어갈 수 있음 */
    long long deadline;
    int hl, bl, r;

    bl = http_format_error_body(body, sizeof body, status);
    if (bl < 0) {
        fprintf(stderr, "BUG: error body overflow\n");
        return -1;
    }
    hl = http_format_head(head, sizeof head, status,
                          "text/html; charset=utf-8", bl, extra, time(NULL));
    if (hl < 0)
        return -1;
    deadline = now_ms() + SEND_TIMEOUT_MS;
    r = send_all(fd, head, (size_t)hl, deadline);
    if (r == SEND_OK)
        r = send_all(fd, body, (size_t)bl, deadline);
    if (r != SEND_OK) {
        if (r == SEND_ERROR)
            perror("send");
        return r;
    }
    return bl;
}

/*
 * 로그용으로 원문 요청의 첫 줄을 복사한다 (파싱이 버퍼를 바꾸기 전에 호출).
 * CR/LF 앞까지 최대 REQLINE_LOG_MAX 바이트, 출력 불가 문자는 '?', 넘치면 "...".
 * 받은 바이트가 없으면 "-".
 */
static void copy_reqline(const char *buf, size_t len, char *out, size_t cap)
{
    size_t i, o = 0;

    if (len == 0) {
        snprintf(out, cap, "-");
        return;
    }
    for (i = 0; i < len && buf[i] != '\r' && buf[i] != '\n'; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (o == REQLINE_LOG_MAX) {
            memcpy(out + o, "...", 3);
            o += 3;
            break;
        }
        /* '"' 도 바꿔서 로그의 "요청 라인" 따옴표가 깨지지 않게 */
        out[o++] = (c >= 0x20 && c <= 0x7e && c != '"') ? (char)c : '?';
    }
    out[o] = '\0';
    (void)cap;   /* 호출자가 REQLINE_LOG_MAX + 4 이상을 보장 */
}

/* 접근 로그 한 줄: ip:port "요청 라인" 상태 본문바이트 [비고] */
static void access_log(const char *peer, const char *reqline, int status,
                       long long body_len, const char *note)
{
    if (status > 0)
        fprintf(stderr, "%s \"%s\" %d %lld%s%s\n", peer, reqline, status,
                body_len, note ? " " : "", note ? note : "");
    else
        fprintf(stderr, "%s \"%s\" - 0%s%s\n", peer, reqline,
                note ? " " : "", note ? note : "");
}

/*
 * 응답을 보내고 연결을 닫는다. status <= 0 이면 응답 없이 close (로그에 note).
 * 400 이상이면 lingering close.
 */
static void finish_error(int fd, const char *peer, const char *reqline,
                         int status, const char *extra, const char *note)
{
    int body_len;

    if (status <= 0) {
        access_log(peer, reqline, 0, 0, note);
        close(fd);
        return;
    }
    status = http_normalize_status(status);
    body_len = send_error(fd, status, extra);
    if (body_len < 0) {
        access_log(peer, reqline, 0, 0, send_fail_note(body_len));
        close(fd);
        return;
    }
    access_log(peer, reqline, status, body_len, note);
    if (status >= 400)
        lingering_close(fd);
    else
        close(fd);
}

/*
 * /cgi-bin 으로 온 POST 의 본문: 411 → 413 → 100 Continue → 본문 읽기.
 * 반환: 0 성공 (*body 에 malloc 한 본문, 길이 0 이면 NULL),
 *       -1 응답 없이 close (*note 에 이유), 그 외 응답할 상태 코드.
 */
static int read_cgi_body(int fd, const char *buf, size_t len,
                         const struct http_request *req, const char **note,
                         char **out_body)
{
    struct body_input in;
    const char *expect;
    char *body = NULL;
    size_t consumed;
    enum read_body_result r;

    *out_body = NULL;
    if (req->content_length < 0)
        return 411;
    if (req->content_length > REQUEST_BODY_MAX)
        return 413;

    in.buf = buf;
    in.buffered_len = len;
    in.head_len = req->head_len;
    in.content_length = req->content_length;

    expect = http_get_header(req, "Expect");
    if (req->version_minor == 1 && expect != NULL &&
        strcasecmp(expect, "100-continue") == 0 &&
        (long long)(len - req->head_len) < req->content_length) {
        int r = send_all(fd, HTTP_100_CONTINUE, strlen(HTTP_100_CONTINUE),
                         now_ms() + SEND_TIMEOUT_MS);
        if (r != SEND_OK) {
            *note = send_fail_note(r);
            return -1;
        }
    }

    if (req->content_length > 0) {
        body = malloc((size_t)req->content_length);
        if (body == NULL) {
            fprintf(stderr, "malloc(%lld) for request body: %s\n",
                    req->content_length, strerror(errno));
            return 500;
        }
    }

    r = read_request_body(fd, &in, BODY_TIMEOUT_MS, body, &consumed);
    (void)consumed;            /* phase 1: 남는 바이트는 무시 (Connection: close) */
    if (r == READ_BODY_OK) {
        *out_body = body;
        return 0;
    }
    free(body);                /* S9: 실패하면 버퍼를 바로 해제 */

    switch (r) {
    case READ_BODY_TIMEOUT:
        return 408;
    case READ_BODY_CLOSED:
        *note = "(client closed mid-body)";
        return -1;
    case READ_BODY_OK:
    case READ_BODY_ERROR:
        break;
    }
    *note = "(body read error)";
    return -1;
}

/* getpeername/getsockname 결과를 "ip" 와 포트로 */
static void sock_addr(int fd, int peer, char *ip, size_t ip_cap, int *port)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int r = peer ? getpeername(fd, (struct sockaddr *)&a, &al)
                 : getsockname(fd, (struct sockaddr *)&a, &al);

    if (r < 0 || inet_ntop(AF_INET, &a.sin_addr, ip, (socklen_t)ip_cap) == NULL) {
        snprintf(ip, ip_cap, "0.0.0.0");
        *port = 0;
        return;
    }
    *port = ntohs(a.sin_port);
}

/* SERVER_NAME: Host 헤더에서 포트를 뗀 값, 없으면 서버 쪽 IP */
static void server_name_of(const struct http_request *req, const char *local_ip,
                           char *out, size_t cap)
{
    const char *host = req->host;
    size_t n;

    if (host == NULL || host[0] == '\0') {
        snprintf(out, cap, "%s", local_ip);
        return;
    }
    if (host[0] == '[') {                   /* [IPv6]:port */
        const char *rb = strchr(host, ']');
        n = rb ? (size_t)(rb - host) + 1 : strlen(host);
    } else {
        n = strcspn(host, ":");
    }
    if (n >= cap)
        n = cap - 1;
    memcpy(out, host, n);
    out[n] = '\0';
}

static void cgi_fail_note(const struct cgi_result *res, char *out, size_t cap)
{
    switch (res->fail) {
    case CGI_FAIL_SIGNAL:
        snprintf(out, cap, "(cgi signal %d)", res->detail);
        break;
    case CGI_FAIL_EXIT:
        snprintf(out, cap, "(cgi exit %d)", res->detail);
        break;
    case CGI_FAIL_TIMEOUT:
        snprintf(out, cap, "(cgi timeout)");
        break;
    case CGI_FAIL_TOO_LARGE:
        snprintf(out, cap, "(cgi output too large)");
        break;
    case CGI_FAIL_BAD_HEADER:
        snprintf(out, cap, "(cgi bad header)");
        break;
    case CGI_FAIL_INTERNAL:
    case CGI_OK:
        snprintf(out, cap, "(cgi internal error)");
        break;
    }
}

/*
 * /cgi-bin 요청 전체 처리: 스크립트 찾기 → (POST) 본문 → 실행 → 응답 → close.
 */
static void serve_cgi(int fd, const char *peer, const char *reqline,
                      const char *buf, size_t len,
                      const struct http_request *req, const char *root,
                      const char *decoded)
{
    char script[PATH_MAX];
    char local_ip[INET_ADDRSTRLEN], remote_ip[INET_ADDRSTRLEN];
    char server_name[256], protocol[16], note_buf[64];
    char head[1024];
    const char *note = NULL;
    char *body = NULL;
    struct cgi_request creq;
    struct cgi_result res;
    size_t out_len;
    long long deadline;
    int status, hl, sr;

    status = cgi_resolve(root, decoded, script);
    if (status == 0 && req->method == HTTP_POST)
        status = read_cgi_body(fd, buf, len, req, &note, &body);
    if (status != 0) {
        finish_error(fd, peer, reqline, status, NULL, note);
        return;
    }

    memset(&creq, 0, sizeof creq);
    sock_addr(fd, 1, remote_ip, sizeof remote_ip, &creq.remote_port);
    sock_addr(fd, 0, local_ip, sizeof local_ip, &creq.server_port);
    server_name_of(req, local_ip, server_name, sizeof server_name);
    snprintf(protocol, sizeof protocol, "HTTP/1.%d", req->version_minor);
    creq.script_path = script;
    creq.script_name = decoded;
    creq.method = req->method_str;
    creq.query = req->query;
    creq.protocol = protocol;
    creq.remote_addr = remote_ip;
    creq.server_name = server_name;
    creq.content_length = -1;
    if (req->method == HTTP_POST) {
        creq.content_type = http_get_header(req, "Content-Type");
        creq.content_length = req->content_length;
        creq.body = body;
        creq.body_len = (size_t)req->content_length;
    }

    cgi_run(&creq, CGI_TIMEOUT_MS, &res);
    free(body);
    if (res.fail != CGI_OK) {
        cgi_fail_note(&res, note_buf, sizeof note_buf);
        finish_error(fd, peer, reqline, 500, NULL, note_buf);   /* S11 */
        return;
    }

    /* 상태 줄과 헤더는 서버가 쓴다 (문구는 서버 표, 없으면 빈 문구) */
    out_len = res.output_len - res.body_off;
    hl = http_format_head_raw(head, sizeof head, res.status, res.content_type,
                              (long long)out_len, NULL, time(NULL));
    if (hl < 0) {
        cgi_result_free(&res);
        finish_error(fd, peer, reqline, 500, NULL, "(cgi head overflow)");
        return;
    }
    deadline = now_ms() + SEND_TIMEOUT_MS;   /* CGI 실행 시간과 별개로 전송부터 */
    sr = send_all(fd, head, (size_t)hl, deadline);
    if (sr == SEND_OK)
        sr = send_all(fd, res.output + res.body_off, out_len, deadline);
    if (sr != SEND_OK) {
        access_log(peer, reqline, 0, 0, send_fail_note(sr));
        cgi_result_free(&res);
        close(fd);
        return;
    }
    access_log(peer, reqline, res.status, (long long)out_len, NULL);
    cgi_result_free(&res);
    if (res.status >= 400)
        lingering_close(fd);
    else
        close(fd);
}

void server_handle(int conn_fd, const char *peer, const char *root)
{
    char buf[REQUEST_HEAD_MAX];
    char reqline[REQLINE_LOG_MAX + 4];
    char decoded[HTTP_URI_MAX + 1];
    char location[HTTP_URI_MAX + 64];
    const char *extra = NULL;
    struct http_request req;
    struct static_target target;
    enum read_head_result rr;
    const char *note = NULL;
    int status = 0;            /* 0 이면 응답 없이 닫는다 */
    size_t len;

    rr = read_request_head(conn_fd, buf, sizeof buf, HEADER_TIMEOUT_MS, &len);
    copy_reqline(buf, len, reqline, sizeof reqline);   /* 파서가 buf 를 바꾸기 전에 */

    switch (rr) {
    case READ_HEAD_OK:
        status = http_parse_request(buf, len, &req);
        if (status == 0)
            status = url_check_path(req.path, decoded, sizeof decoded);
        if (status != 0)
            break;
        if (url_is_cgi(decoded)) {
            serve_cgi(conn_fd, peer, reqline, buf, len, &req, root, decoded);
            return;
        }
        if (req.method == HTTP_POST)
            status = 405;
        else
            status = static_resolve(root, decoded, &target);
        break;
    case READ_HEAD_TOO_LARGE:
        status = http_classify_overflow(buf, len);
        break;
    case READ_HEAD_TIMEOUT:
        if (len > 0)
            status = 408;
        else
            note = "(timeout, no data)";
        break;
    case READ_HEAD_CLOSED:
        note = "(client closed)";
        break;
    case READ_HEAD_ERROR:
        note = "(read error)";
        break;
    }

    if (status == 0) {
        finish_error(conn_fd, peer, reqline, 0, NULL, note);
        return;
    }

    if (status == 200) {
        long long sent;
        int r = static_send(conn_fd, &target, &sent);

        if (r <= 0) {          /* 완료, 또는 헤더를 보낸 뒤 실패 (S10: EPIPE, 시간 초과) */
            access_log(peer, reqline, 200, sent,
                       r == SEND_TIMEOUT ? "(send timeout)" :
                       r < 0             ? "(aborted)" : NULL);
            close(conn_fd);
            return;
        }
        status = r;            /* 아직 아무것도 안 보냄: 에러 응답으로 */
    }

    if (status == 301) {       /* 디렉터리: 원문 경로 + '/' (+ 쿼리 그대로) */
        snprintf(location, sizeof location, "Location: %s/%s%s\r\n", req.path,
                 req.query ? "?" : "", req.query ? req.query : "");
        extra = location;
    } else if (status == 405) {
        extra = "Allow: GET\r\n";
    }

    finish_error(conn_fd, peer, reqline, status, extra, NULL);
}

volatile sig_atomic_t server_stop = 0;

/*
 * 큐가 가득 찼을 때 accept 스레드에서 503 을 보내고 닫는다.
 * accept 스레드가 막히면 안 되므로 어느 단계도 기다리지 않는다:
 * 503 을 MSG_DONTWAIT 로 한 번 send (수백 바이트라 새 소켓 버퍼에 다 들어감) →
 * 쓰기 방향 닫기(FIN) → 이미 도착한 요청 바이트만 비우기 (안 읽은 데이터가 남은 채
 * close 하면 RST 가 나가 클라이언트가 503 을 못 볼 수 있음) → close.
 */
static void reject_busy(int fd, const char *peer)
{
    char resp[1024], junk[4096];
    int hl, bl;
    ssize_t n;

    bl = http_format_error_body(resp + 512, 512, 503);
    hl = bl < 0 ? -1 : http_format_head(resp, 512, 503, "text/html; charset=utf-8",
                                        bl, NULL, time(NULL));
    if (hl < 0) {
        access_log(peer, "-", 0, 0, "(queue full, format failed)");
        close(fd);
        return;
    }
    memmove(resp + hl, resp + 512, (size_t)bl);   /* 헤더 바로 뒤로 본문을 붙임 */

    do {
        n = send(fd, resp, (size_t)(hl + bl), MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n != hl + bl) {
        access_log(peer, "-", 0, 0, "(queue full, send failed)");
        close(fd);
        return;
    }
    access_log(peer, "-", 503, bl, "(queue full)");

    shutdown(fd, SHUT_WR);
    while ((n = recv(fd, junk, sizeof junk, MSG_DONTWAIT)) > 0 ||
           (n < 0 && errno == EINTR))
        ;
    close(fd);
}

/* 워커 스레드에서 연결 하나 처리 */
static void handle_item(const struct conn_item *item, void *root)
{
    server_handle(item->fd, item->peer, root);
}

int server_run(int listen_fd, const char *root, int workers, int queue_cap)
{
    struct conn_queue q;
    struct worker_pool pool;
    int dropped;

    if (queue_init(&q, queue_cap) < 0) {
        fprintf(stderr, "queue_init(%d): failed\n", queue_cap);
        return -1;
    }
    if (pool_start(&pool, workers, &q, handle_item, (void *)root) < 0) {
        queue_destroy(&q);
        return -1;
    }
    fprintf(stderr, "workers=%d queue=%d\n", workers, queue_cap);

    /* 종료 신호는 이 스레드만 받는다 (워커는 막아 둠): accept 가 EINTR 로 깨어남 */
    while (!server_stop) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        char ip[INET_ADDRSTRLEN];
        struct conn_item item;
        enum queue_result qr;
        int conn_fd;

        conn_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (conn_fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;          /* EINTR: 종료 신호면 while 조건에서 빠져나감 */
            perror("accept");
            continue;
        }
        /* CGI 자식이 연결 소켓을 물려받지 않게 (실패해도 처리는 계속) */
        if (fcntl(conn_fd, F_SETFD, FD_CLOEXEC) < 0)
            perror("fcntl(FD_CLOEXEC)");

        if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip) == NULL)
            strcpy(ip, "?");
        item.fd = conn_fd;
        snprintf(item.peer, sizeof item.peer, "%s:%u", ip, ntohs(peer.sin_port));

        /* 워커도 큐도 가득: 기다리지 않고 바로 503 (S3) */
        qr = queue_push(&q, &item, 0);
        if (qr == QUEUE_TIMEOUT)
            reject_busy(conn_fd, item.peer);
        else if (qr != QUEUE_OK)
            close(conn_fd);
    }

    pool_stop(&pool);              /* 큐를 닫고, 처리 중인 연결이 끝나길 기다림 */
    dropped = queue_drain(&q);
    if (dropped > 0)
        fprintf(stderr, "closed %d queued connection(s) without response\n", dropped);
    queue_destroy(&q);
    return 0;
}
