#ifndef STATIC_H
#define STATIC_H

#include "http_parser.h"

#include <limits.h>

/* 큰 파일을 보낼 때 read → write 한 번에 옮기는 크기 */
#define STATIC_CHUNK (64 * 1024)

/*
 * 파일 경로(마지막 세그먼트의 확장자, 대소문자 무시)로 Content-Type 을 정한다.
 * 표에 없거나 확장자가 없으면 "application/octet-stream".
 */
const char *mime_lookup(const char *path);

struct static_target {
    char path[PATH_MAX];   /* realpath 로 확정한 실제 파일 경로 */
    long long size;        /* stat 시점의 크기 (전송 때 fstat 으로 다시 정함) */
    const char *mime;
};

/*
 * url_check_path 를 통과한 디코딩된 경로를 root 아래의 일반 파일로 해석한다.
 * (/cgi-bin 판단과 405 는 server_handle 이 먼저 한다.)
 * root 는 realpath 로 정규화된 절대 경로여야 한다 (config_parse 가 보장).
 *
 * 반환: 200 이면 *out 이 채워짐. 그 외에는 응답할 상태 코드.
 *   realpath/stat (404 없음, 403 권한·root 밖·일반 파일 아님, 500 기타)
 *   → 디렉터리: 끝 '/' 없으면 301, index.html 없으면 403
 */
int static_resolve(const char *root, const char *decoded,
                   struct static_target *out);

/*
 * 파일을 열어 200 헤더와 본문을 fd 로 보낸다. *sent 에 보낸 본문 바이트 수.
 * 반환:
 *    0  전송 완료
 *   -1  헤더를 보낸 뒤 실패 (EPIPE, 파일이 줄어듦 등) → 호출자는 그냥 close
 *   그 외  아직 아무것도 보내지 않음 → 그 상태 코드로 에러 응답 (open 실패 등)
 */
int static_send(int fd, const struct static_target *t, long long *sent);

#endif
