#ifndef URL_H
#define URL_H

#include <stddef.h>

/*
 * 퍼센트 디코딩. in 의 %XX(16진수, 대소문자 무관)를 바이트로 바꿔 out 에 NUL 종료로 쓴다.
 * '+' 는 경로 규칙대로 그대로 둔다. 결과는 입력보다 길어지지 않으므로
 * cap >= strlen(in) + 1 이면 항상 충분하다.
 * 반환: 0 성공, -1 잘못된 % (16진수 아님, 두 자리 미만), %00, cap 부족.
 */
int url_decode(const char *in, char *out, size_t cap);

enum url_path_check {
    URL_PATH_OK,
    URL_PATH_DOTDOT,   /* ".." 세그먼트가 있음 → 400 */
    URL_PATH_DOTFILE   /* '.' 으로 시작하는 세그먼트가 있음 ("." 포함) → 404 */
};

/*
 * 디코딩된 경로를 '/' 로 나눠 세그먼트를 검사한다.
 * ".." 이 어디든 있으면 DOTFILE 보다 우선해서 DOTDOT 을 돌려준다.
 */
enum url_path_check url_check_segments(const char *path);

#endif
