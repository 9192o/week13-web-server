#ifndef CGI_H
#define CGI_H

#include <limits.h>
#include <stddef.h>

#define CGI_OUTPUT_MAX       (1024 * 1024)  /* 넘으면 kill + 500 */
#ifndef CGI_TIMEOUT_MS                      /* 빌드할 때 -D 로 바꿀 수 있음 (server.h 참고) */
#define CGI_TIMEOUT_MS       5000           /* fork 부터. 넘으면 kill + 500 */
#endif
#define CGI_CONTENT_TYPE_MAX 256            /* 넘으면 500 */

/*
 * 디코딩된 경로(url_is_cgi 가 참)를 실행할 스크립트로 해석한다.
 * /cgi-bin/<이름> 한 단계만 허용 (/cgi-bin, /cgi-bin/, 하위 경로는 404).
 * 반환: 0 이면 out_path(PATH_MAX)에 realpath. 그 외에는 상태 코드
 *   404 없음, 403 권한·root/cgi-bin 밖·일반 파일 아님·실행 권한 없음, 500 기타.
 */
int cgi_resolve(const char *root, const char *decoded, char *out_path);

/* CGI 에 넘길 요청 정보 (환경 변수와 stdin) */
struct cgi_request {
    const char *script_path;     /* cgi_resolve 결과 */
    const char *script_name;     /* SCRIPT_NAME: 디코딩된 요청 경로 */
    const char *method;          /* REQUEST_METHOD */
    const char *query;           /* QUERY_STRING, NULL 이면 "" */
    const char *protocol;        /* SERVER_PROTOCOL */
    const char *content_type;    /* CONTENT_TYPE, NULL 이면 안 넘김 */
    long long content_length;    /* CONTENT_LENGTH, < 0 이면 안 넘김 */
    const char *body;            /* stdin 으로 넘길 본문 */
    size_t body_len;
    const char *remote_addr;
    int remote_port;
    const char *server_name;
    int server_port;
};

enum cgi_fail {
    CGI_OK,
    CGI_FAIL_SIGNAL,      /* detail = 시그널 번호 */
    CGI_FAIL_EXIT,        /* detail = exit 코드 (exec 실패는 127) */
    CGI_FAIL_TIMEOUT,
    CGI_FAIL_TOO_LARGE,
    CGI_FAIL_BAD_HEADER,
    CGI_FAIL_INTERNAL     /* pipe/fork/malloc/poll 실패 */
};

struct cgi_result {
    enum cgi_fail fail;
    int detail;
    int status;                                   /* Status 헤더 또는 200 */
    char content_type[CGI_CONTENT_TYPE_MAX + 1];
    char *output;                                 /* CGI 출력 전체 (cgi_result_free) */
    size_t output_len;
    size_t body_off;                              /* 본문 시작 = output + body_off */
};

/*
 * CGI 출력의 헤더 부분을 해석한다. 줄 끝은 LF 또는 CRLF.
 * Content-Type(필수, 한 번)과 Status(선택, 한 번, 세 자리 숫자)만 보고
 * 나머지 헤더는 버린다. CGI 가 보낸 상태 문구도 무시.
 * Status 허용 범위: 200~599, 단 204·205·304 제외 (1xx 는 최종 응답 불가,
 * 204/205/304 는 본문을 가질 수 없음).
 * 반환: 0 성공, -1 형식 오류 (빈 줄 없음, 콜론 없는 줄, Content-Type 없음·중복·너무 김,
 *       Status 중복·형식 오류·허용 범위 밖).
 */
int cgi_parse_output(const char *out, size_t len, int *status,
                     char *content_type, size_t ct_cap, size_t *body_off);

/*
 * 스크립트를 fork + execve 로 실행해 출력을 모은다 (출력 전체 버퍼링).
 * stdin 쓰기와 stdout 읽기를 poll 로 동시에 처리하고, 항상 waitpid 로 거둔다.
 * 결과는 *res 에. res->fail == CGI_OK 일 때만 status/content_type/본문이 유효.
 */
void cgi_run(const struct cgi_request *req, int timeout_ms, struct cgi_result *res);

void cgi_result_free(struct cgi_result *res);

#endif
