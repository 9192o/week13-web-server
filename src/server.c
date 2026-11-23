#include "server.h"
#include "body.h"
#include "http_parser.h"
#include "http_response.h"
#include "static.h"
#include "url.h"

#include <arpa/inet.h>
#include <errno.h>
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
 * deadline(now_ms 기준)까지 fd 가 읽을 수 있게 되길 기다린다.
 * 반환: 1 읽기 가능(데이터, EOF, 오류 모두 — read 가 알려줌), 0 시간 초과, -1 poll 오류.
 */
int wait_readable(int fd, long long deadline)
{
    for (;;) {
        long long left = deadline - now_ms();
        struct pollfd p;
        int r;

        if (left <= 0)
            return 0;
        p.fd = fd;
        p.events = POLLIN;
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
 * 반환: 보낸 본문 바이트 수, 생성기 버그나 쓰기 실패면 -1 (호출자는 응답 없이 닫음).
 */
static int send_error(int fd, int status, const char *extra)
{
    char head[HTTP_URI_MAX + 512], body[512];   /* Location 에 URI 가 통째로 들어갈 수 있음 */
    int hl, bl;

    bl = http_format_error_body(body, sizeof body, status);
    if (bl < 0) {
        fprintf(stderr, "BUG: error body overflow\n");
        return -1;
    }
    hl = http_format_head(head, sizeof head, status,
                          "text/html; charset=utf-8", bl, extra, time(NULL));
    if (hl < 0)
        return -1;
    if (write_all(fd, head, (size_t)hl) < 0 ||
        write_all(fd, body, (size_t)bl) < 0) {
        perror("write");
        return -1;
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
 * /cgi-bin 으로 온 POST: 411 → 413 → 100 Continue → 본문 읽기.
 * CGI 단위 전이라 본문을 다 읽으면 임시로 404.
 * 반환: 응답할 상태 코드, 또는 0 (응답 없이 close, *note 에 이유).
 */
static int handle_cgi_post(int fd, const char *buf, size_t len,
                           const struct http_request *req, const char **note)
{
    struct body_input in;
    const char *expect;
    char *body = NULL;
    size_t consumed;
    enum read_body_result r;

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
        if (write_all(fd, HTTP_100_CONTINUE, strlen(HTTP_100_CONTINUE)) < 0) {
            *note = "(send failed)";
            return 0;
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
    free(body);
    (void)consumed;            /* phase 1: 남는 바이트는 무시 (Connection: close) */

    switch (r) {
    case READ_BODY_OK:
        return 404;            /* CGI 단위 전까지 임시 */
    case READ_BODY_TIMEOUT:
        return 408;
    case READ_BODY_CLOSED:
        *note = "(client closed mid-body)";
        return 0;
    case READ_BODY_ERROR:
        break;
    }
    *note = "(body read error)";
    return 0;
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
    int body_len;
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
        if (url_is_cgi(decoded))
            status = req.method == HTTP_POST
                         ? handle_cgi_post(conn_fd, buf, len, &req, &note)
                         : 404;        /* CGI GET 은 CGI 단위에서 */
        else if (req.method == HTTP_POST)
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
        access_log(peer, reqline, 0, 0, note);
        close(conn_fd);
        return;
    }

    if (status == 200) {
        long long sent;
        int r = static_send(conn_fd, &target, &sent);

        if (r <= 0) {          /* 완료, 또는 헤더를 보낸 뒤 실패 (S10: EPIPE) */
            access_log(peer, reqline, 200, sent, r < 0 ? "(aborted)" : NULL);
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

    status = http_normalize_status(status);
    body_len = send_error(conn_fd, status, extra);
    if (body_len < 0) {
        access_log(peer, reqline, 0, 0, "(send failed)");
        close(conn_fd);
        return;
    }
    access_log(peer, reqline, status, body_len, NULL);
    if (status >= 400)
        lingering_close(conn_fd);
    else
        close(conn_fd);
}

void server_run(int listen_fd, const char *root)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        char ip[INET_ADDRSTRLEN];
        char peer_str[INET_ADDRSTRLEN + 8];
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
        snprintf(peer_str, sizeof peer_str, "%s:%u", ip, ntohs(peer.sin_port));

        server_handle(conn_fd, peer_str, root);
    }
}
