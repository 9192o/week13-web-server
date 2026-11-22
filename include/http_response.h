#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include <time.h>

#define HTTP_SERVER_NAME "week13-webserver"

/* 상태 코드의 reason phrase. 표에 없으면 NULL. */
const char *http_reason(int status);

/*
 * 표에 있는 코드면 그대로, 없으면 stderr 에 "BUG: unknown status <코드>" 를
 * 찍고 500 을 돌려준다.
 */
int http_normalize_status(int status);

/*
 * 상태 라인 + 표준 헤더 + 빈 줄을 buf 에 쓴다. 본문은 쓰지 않는다.
 *   Date, Server, Content-Type, Content-Length, Connection: close
 * now 는 Date 헤더에 쓸 시각 (테스트에서 고정값을 넘길 수 있게).
 * 반환: 쓴 바이트 수. buf 가 부족하면 "BUG: response head overflow" 를 찍고 -1.
 */
int http_format_head(char *buf, size_t cap, int status,
                     const char *content_type, long long content_length,
                     time_t now);

/*
 * 에러 페이지 HTML 본문을 buf 에 쓴다. 사용자 입력은 넣지 않는다.
 * 반환: 쓴 바이트 수. buf 가 부족하면 -1.
 */
int http_format_error_body(char *buf, size_t cap, int status);

#endif
