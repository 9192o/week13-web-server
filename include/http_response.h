#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include <time.h>

#define HTTP_SERVER_NAME "week13-webserver"

/* Expect: 100-continue 에 대한 중간 응답 (상태 줄 + 빈 줄) */
#define HTTP_100_CONTINUE "HTTP/1.1 100 Continue\r\n\r\n"

/* 상태 코드의 reason phrase. 표에 없으면 NULL. */
const char *http_reason(int status);

/*
 * 표에 있는 코드면 그대로, 없으면 stderr 에 "BUG: unknown status <코드>" 를
 * 찍고 500 을 돌려준다.
 */
int http_normalize_status(int status);

/*
 * 상태 라인 + 표준 헤더 + 빈 줄을 buf 에 쓴다. 본문은 쓰지 않는다.
 *   Date, Server, Content-Type, Content-Length, [extra], Connection: close
 * extra 는 NULL 이거나 "Name: value\r\n" 줄들(Location, Allow 등)로, 그대로 붙인다.
 * now 는 Date 헤더에 쓸 시각 (테스트에서 고정값을 넘길 수 있게).
 * 반환: 쓴 바이트 수. buf 가 부족하면 "BUG: response head overflow" 를 찍고 -1.
 */
int http_format_head(char *buf, size_t cap, int status,
                     const char *content_type, long long content_length,
                     const char *extra, time_t now);

/*
 * http_format_head 와 같지만 상태 코드를 500 으로 바꾸지 않는다 (CGI Status 용).
 * 문구는 표에 있으면 표의 문구, 없으면 빈 문구 ("HTTP/1.1 201 ").
 * 허용 범위 검사는 호출자 책임 (CGI 는 cgi_parse_output 이 함).
 */
int http_format_head_raw(char *buf, size_t cap, int status,
                         const char *content_type, long long content_length,
                         const char *extra, time_t now);

/*
 * 에러 페이지 HTML 본문을 buf 에 쓴다. 사용자 입력은 넣지 않는다.
 * 반환: 쓴 바이트 수. buf 가 부족하면 -1.
 */
int http_format_error_body(char *buf, size_t cap, int status);

#endif
