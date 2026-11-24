#include "http_parser.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* RFC 9110 tchar: 헤더 이름과 메서드에 쓸 수 있는 문자 */
static int is_tchar(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z'))
        return 1;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int is_token(const char *s, size_t n)
{
    size_t i;

    if (n == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (!is_tchar((unsigned char)s[i]))
            return 0;
    return 1;
}

/* buf[from..to) 에서 첫 CRLF 의 위치. 없으면 -1. */
static long find_crlf(const char *buf, size_t from, size_t to)
{
    size_t i;

    for (i = from; i + 1 < to; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return (long)i;
    return -1;
}

static int has_bare_cr_or_lf(const char *s, size_t n)
{
    return memchr(s, '\r', n) != NULL || memchr(s, '\n', n) != NULL;
}

/* 헤더 끝(CRLF CRLF) 다음 위치까지의 길이. 없으면 0. */
static size_t head_length(const char *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 4 <= len; i++)
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0)
            return i + 4;
    return 0;
}

/*
 * 요청 라인 [line, line+n) 에서 URI 부분의 길이를 잰다 (첫 SP 와 다음 SP 사이,
 * 다음 SP 가 없으면 줄 끝까지). 첫 SP 가 없으면 -1.
 */
static long uri_span(const char *line, size_t n, const char **uri_out,
                     const char **sp2_out)
{
    const char *sp1 = memchr(line, ' ', n);
    const char *end = line + n;
    const char *sp2;

    if (sp1 == NULL)
        return -1;
    sp2 = memchr(sp1 + 1, ' ', (size_t)(end - (sp1 + 1)));
    *uri_out = sp1 + 1;
    *sp2_out = sp2;
    return (long)((sp2 ? sp2 : end) - (sp1 + 1));
}

static int parse_request_line(char *line, size_t n, struct http_request *req)
{
    const char *uri, *sp2;
    char *method_end, *uri_end, *version, *q;
    long ulen = uri_span(line, n, &uri, &sp2);
    size_t i, mlen, vlen;

    /* 414: 길이 검사가 가장 먼저 */
    if (ulen > HTTP_URI_MAX)
        return 414;

    /* 400: 요청 라인 형식 — METHOD SP URI SP VERSION, 단일 SP, CRLF 만 */
    if (ulen < 0 || sp2 == NULL || has_bare_cr_or_lf(line, n))
        return 400;
    method_end = (char *)uri - 1;
    uri_end = (char *)sp2;
    version = uri_end + 1;
    mlen = (size_t)(method_end - line);
    vlen = (size_t)((line + n) - version);

    if (!is_token(line, mlen) || ulen == 0)
        return 400;
    if (vlen != 8 || memcmp(version, "HTTP/", 5) != 0 ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9')
        return 400;
    if (uri[0] != '/')                 /* origin-form 만 */
        return 400;
    for (i = 0; i < (size_t)ulen; i++) {
        unsigned char c = (unsigned char)uri[i];
        if (c < 0x21 || c > 0x7e)      /* 제어문자, 공백, 비 ASCII */
            return 400;
    }

    /* 501: 형식은 맞지만 지원하지 않는 메서드 (대소문자 구분) */
    if (mlen == 3 && memcmp(line, "GET", 3) == 0)
        req->method = HTTP_GET;
    else if (mlen == 4 && memcmp(line, "POST", 4) == 0)
        req->method = HTTP_POST;
    else
        return 501;

    /* 505: 형식은 맞지만 지원하지 않는 버전 */
    if (version[5] != '1' || (version[7] != '0' && version[7] != '1'))
        return 505;
    req->version_minor = version[7] - '0';

    /* 제자리 NUL 종료 */
    *method_end = '\0';
    *uri_end = '\0';
    line[n] = '\0';                    /* CR 자리 */
    req->method_str = line;
    req->path = uri;
    q = strchr((char *)uri, '?');
    if (q != NULL) {
        *q = '\0';
        req->query = q + 1;
    }
    return 0;
}

static int parse_header_line(char *line, size_t n, struct http_request *req)
{
    char *colon, *v, *vend;
    size_t i;

    if (n > 0 && (line[0] == ' ' || line[0] == '\t'))
        return 400;                    /* obs-fold */
    if (has_bare_cr_or_lf(line, n))
        return 400;
    colon = memchr(line, ':', n);
    if (colon == NULL || !is_token(line, (size_t)(colon - line)))
        return 400;                    /* 이름 비었음, 이름 뒤 공백 등 */
    if (req->header_count == HTTP_MAX_HEADERS)
        return 431;

    v = colon + 1;
    vend = line + n;
    while (v < vend && (*v == ' ' || *v == '\t'))
        v++;
    while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t'))
        vend--;
    for (i = 0; v + i < vend; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c != '\t' && (c < 0x20 || c == 0x7f))
            return 400;
    }

    *colon = '\0';
    *vend = '\0';
    req->headers[req->header_count].name = line;
    req->headers[req->header_count].value = v;
    req->header_count++;
    return 0;
}

/* 숫자로만 된 비어있지 않은 문자열을 long long 으로. 형식 오류나 오버플로면 -1. */
static long long parse_content_length(const char *s)
{
    const char *p;
    char *end;
    long long v;

    if (*s == '\0')
        return -1;
    for (p = s; *p; p++)               /* '+', '-', 공백 등은 strtoll 이 받아주므로 먼저 거름 */
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno == ERANGE || *end != '\0')
        return -1;
    return v;
}

static int check_headers(struct http_request *req)
{
    size_t i, host_count = 0;
    int has_te = 0, has_cl = 0;

    for (i = 0; i < req->header_count; i++) {
        const struct http_header *h = &req->headers[i];

        if (strcasecmp(h->name, "Content-Length") == 0) {
            long long v = parse_content_length(h->value);
            if (v < 0)
                return 400;
            if (has_cl && v != req->content_length)
                return 400;            /* 다른 값으로 중복 */
            req->content_length = v;   /* 같은 값 중복은 허용 */
            has_cl = 1;
        } else if (strcasecmp(h->name, "Transfer-Encoding") == 0) {
            has_te = 1;
        } else if (strcasecmp(h->name, "Host") == 0) {
            host_count++;
            req->host = h->value;
        } else if (strcasecmp(h->name, "Connection") == 0) {
            if (req->connection == NULL)
                req->connection = h->value;
        }
    }

    if (has_te)
        return has_cl ? 400 : 501;     /* CL+TE 동시는 smuggling 위험 → 400 */
    if (host_count > 1)
        return 400;
    if (req->version_minor == 1 && host_count == 0)
        return 400;
    return 0;
}

int http_parse_request(char *buf, size_t len, struct http_request *req)
{
    size_t head_len, pos;
    long eol;
    int rc;

    memset(req, 0, sizeof *req);
    req->content_length = -1;

    head_len = head_length(buf, len);
    if (head_len == 0)
        return 400;                    /* 헤더 끝이 없음 */
    req->head_len = head_len;

    /* 요청 라인: 첫 CRLF 까지 (head_len 범위 안에 반드시 있음) */
    eol = find_crlf(buf, 0, head_len);
    if (eol <= 0)
        return 400;                    /* 빈 요청 라인 */
    rc = parse_request_line(buf, (size_t)eol, req);
    if (rc != 0)
        return rc;

    /* 헤더 줄들: 마지막 빈 줄(head_len - 2 위치의 CRLF) 전까지 */
    pos = (size_t)eol + 2;
    while (pos < head_len - 2) {
        long le = find_crlf(buf, pos, head_len);
        rc = parse_header_line(buf + pos, (size_t)le - pos, req);
        if (rc != 0)
            return rc;
        pos = (size_t)le + 2;
    }

    return check_headers(req);
}

int http_classify_overflow(const char *buf, size_t len)
{
    const char *uri, *sp2;
    long eol = find_crlf(buf, 0, len);

    if (eol < 0)
        return 414;                    /* 요청 라인조차 끝나지 않음 */
    if (uri_span(buf, (size_t)eol, &uri, &sp2) > HTTP_URI_MAX)
        return 414;
    return 431;
}

const char *http_get_header(const struct http_request *req, const char *name)
{
    size_t i;

    for (i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    return NULL;
}

/* value 의 쉼표 구분 토큰 중 하나가 token 과 같으면 1 (OWS 무시, 대소문자 무시) */
static int has_token(const char *value, const char *token)
{
    size_t tlen = strlen(token);
    const char *p = value;

    while (*p != '\0') {
        const char *start, *end;

        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        start = p;
        while (*p != '\0' && *p != ',')
            p++;
        end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if ((size_t)(end - start) == tlen && strncasecmp(start, token, tlen) == 0)
            return 1;
    }
    return 0;
}

int http_wants_close(const struct http_request *req)
{
    size_t i;

    for (i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, "Connection") == 0 &&
            has_token(req->headers[i].value, "close"))
            return 1;
    return 0;
}
