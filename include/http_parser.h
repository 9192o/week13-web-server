#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#define HTTP_URI_MAX      8192   /* 넘으면 414 */
#define HTTP_MAX_HEADERS  64     /* 넘으면 431 */

enum http_method {
    HTTP_GET,
    HTTP_POST
};

struct http_header {
    const char *name;    /* 원문 대소문자 그대로, NUL 종료 */
    const char *value;   /* 앞뒤 공백(SP/HTAB) 제거, NUL 종료 */
};

/*
 * 모든 문자열 포인터는 http_parse_request 에 넘긴 버퍼 내부를 가리킨다.
 * 따라서 이 구조체는 그 버퍼보다 오래 쓰면 안 된다.
 */
struct http_request {
    enum http_method method;
    const char *method_str;   /* "GET" / "POST" */
    const char *path;         /* '?' 앞부분, 퍼센트 디코딩 안 된 원문 */
    const char *query;        /* '?' 뒷부분, 없으면 NULL ("?" 만 있으면 "") */
    int version_minor;        /* HTTP/1.0 → 0, HTTP/1.1 → 1 */

    struct http_header headers[HTTP_MAX_HEADERS];
    size_t header_count;

    long long content_length; /* Content-Length 없으면 -1 */
    const char *host;         /* 없으면 NULL (HTTP/1.0 만 가능) */
    const char *connection;   /* 없으면 NULL. 파싱만 하고 Phase 1 은 항상 close */

    size_t head_len;          /* 빈 줄(CRLF CRLF)까지의 바이트 수 = 본문 시작 오프셋 */
};

/*
 * buf[0..len) 에 담긴 요청 라인 + 헤더를 파싱한다. buf 는 제자리에서 수정된다
 * (구분자 자리에 NUL 을 씀). 헤더 끝 뒤에 붙어 온 본문 바이트는 건드리지 않는다.
 *
 * 반환: 성공 0, 실패 시 응답할 HTTP 상태 코드.
 * 검사 순서 (첫 오류에서 중단):
 *   414 URI 길이 → 400 요청 라인 형식 → 501 메서드 → 505 버전
 *   → 헤더 (431 개수 / 400 형식 / 400 CL+TE / 501 TE) → 400 Content-Length / Host
 */
int http_parse_request(char *buf, size_t len, struct http_request *req);

/*
 * 헤더 끝을 찾기 전에 버퍼가 가득 찼을 때 어떤 오류로 응답할지 정한다.
 * 요청 라인이 끝나지 않았거나 URI 가 상한을 넘으면 414, 아니면 431.
 */
int http_classify_overflow(const char *buf, size_t len);

/* 이름이 같은(대소문자 무시) 첫 번째 헤더의 값. 없으면 NULL. */
const char *http_get_header(const struct http_request *req, const char *name);

/*
 * Connection 헤더(여러 줄 가능)의 쉼표로 나뉜 토큰 중 "close" 가 있으면 1.
 * 토큰 앞뒤 공백(SP/HTAB)은 무시하고, 대소문자는 구분하지 않는다.
 */
int http_wants_close(const struct http_request *req);

#endif
