#ifndef PROXY_H
#define PROXY_H

#include "http_parser.h"

#include <netinet/in.h>
#include <stddef.h>

/*
 * 리버스 프록시 (Phase 3).
 * 규칙 "prefix=host:port" 에 맞는 경로는 백엔드로 넘기고 응답을 클라이언트에 중계한다.
 * 이 모듈은 규칙 해석, 경로 매칭, 요청/응답 헤더 재작성, 백엔드 connect 만 한다.
 * 실제 중계(읽고 보내기)는 server.c 의 serve_proxy 가 한다.
 */

#define PROXY_MAX_ROUTES   16
#define PROXY_MAX_BACKENDS 8     /* 규칙 하나에 둘 수 있는 백엔드 수 */
#define PROXY_PREFIX_MAX   256
#define PROXY_NAME_MAX     320   /* "host:port" 로그 표시용 */

struct proxy_backend {
    char name[PROXY_NAME_MAX];       /* 규칙에 적힌 "host:port" 그대로 (로그용) */
    struct sockaddr_in addr;         /* 시작할 때 getaddrinfo(AF_INET) 로 한 번 해석 */
};

struct proxy_route {
    char prefix[PROXY_PREFIX_MAX];   /* '/' 로 시작. 문자열 앞부분이 같으면 매칭 */
    struct proxy_backend backends[PROXY_MAX_BACKENDS];
    int nbackends;                   /* 1 이상 */
    /*
     * 라운드로빈 카운터. 워커들이 함께 쓰므로 atomic (그냥 ++ 하면 lost update 로
     * 같은 백엔드에 몰리거나 증가가 사라진다). 규칙마다 따로 센다.
     */
    _Atomic unsigned long long next;
};

struct proxy_table {
    struct proxy_route routes[PROXY_MAX_ROUTES];
    int count;
};

/*
 * "-R" 인자 하나를 해석한다: "/api/=127.0.0.1:8001,localhost:8002".
 *  - prefix 는 '/' 로 시작, PROXY_PREFIX_MAX 미만
 *  - '=' 뒤는 쉼표로 나눈 백엔드 1~PROXY_MAX_BACKENDS 개 (같은 백엔드를 여러 번 적어도 됨)
 *  - 백엔드는 마지막 ':' 앞이 호스트(비어 있으면 안 됨), 뒤가 1~65535 포트
 *  - 호스트는 getaddrinfo(AF_INET) 의 첫 결과로 정한다
 * 라운드로빈 카운터는 0 에서 시작한다 (첫 요청은 첫 번째 백엔드).
 * 성공 0, 실패 -1 (errbuf 에 이유).
 */
int proxy_parse_rule(const char *spec, struct proxy_route *out,
                     char *errbuf, size_t errlen);

/*
 * 디코딩된 경로에 맞는 규칙. 여러 개가 맞으면 prefix 가 가장 긴 것.
 * 없으면 NULL. (카운터를 바꾸는 proxy_pick 에 넘기므로 const 가 아님)
 */
struct proxy_route *proxy_match(struct proxy_table *t, const char *path);

/*
 * 라운드로빈으로 이번 요청의 백엔드를 고른다: (카운터++) % nbackends.
 * 여러 워커가 동시에 불러도 안전하다 (atomic fetch-add).
 */
const struct proxy_backend *proxy_pick(struct proxy_route *r);

/*
 * 목록에서 b 다음 백엔드 (끝이면 처음으로). connect 실패 재시도용.
 * 라운드로빈 카운터는 건드리지 않는다 (새 요청의 분배는 그대로).
 */
const struct proxy_backend *proxy_next(const struct proxy_route *r,
                                       const struct proxy_backend *b);

/*
 * 규칙의 백엔드 중 프록시 자기 자신을 가리키는 것: 루프백(127.0.0.0/8) 또는
 * 0.0.0.0 이고 포트가 listen_port 와 같은 첫 백엔드. 없으면 NULL.
 */
const struct proxy_backend *proxy_find_self(const struct proxy_route *r,
                                            int listen_port);

/*
 * 백엔드로 보낼 요청 헤더를 out 에 쓴다.
 *   "<METHOD> <원래 path>[?<query>] HTTP/1.0\r\n"
 *   + 클라이언트 헤더 (hop-by-hop 과 Expect 는 뺌, Host 등은 그대로)
 *   + "Connection: close\r\n\r\n"
 * with_body 가 1 이면 Content-Length 를 그대로 두고 (호출자가 본문을 이어서 보냄, P8),
 * 0 이면 Content-Length 도 뺀다 (본문을 보내지 않는 GET).
 * Expect 를 빼는 이유: 100-continue 는 프록시가 클라이언트와 이미 처리했고, 본문을
 * 헤더와 함께 바로 보내므로 백엔드가 1xx 를 보낼 이유가 없다.
 * 반환: 쓴 바이트 수. cap 이 부족하면 -1.
 */
int proxy_build_request(const struct http_request *req, int with_body,
                        char *out, size_t cap);

struct proxy_response {
    int status;
    const char *reason;              /* 문구 원문 (빈 문자열 가능) */
    struct http_header headers[HTTP_MAX_HEADERS];
    size_t header_count;
    long long body_len;              /* 204/304 → 0, Content-Length 값, 없으면 -1 (EOF 까지) */
    size_t head_len;                 /* 빈 줄까지의 바이트 수 = 본문 시작 오프셋 */
};

/*
 * 백엔드 응답의 상태 줄 + 헤더를 파싱한다. buf[0..len) 안에 "\r\n\r\n" 이 있어야 한다.
 * buf 의 헤더 부분은 제자리에서 바뀐다 (구분자 자리에 NUL). 본문 바이트는 건드리지 않는다.
 * 반환: NULL 성공, 실패면 502 로 응답할 이유 문자열 (로그용):
 *   상태 줄 형식, 1xx, 헤더 형식(obs-fold 포함), 헤더 개수 초과,
 *   Content-Length 가 숫자가 아님 / 여러 개인데 값이 다름, Transfer-Encoding 있음.
 */
const char *proxy_parse_response(char *buf, size_t len, struct proxy_response *r);

/*
 * 클라이언트에 보낼 응답 헤더를 쓴다.
 *   "HTTP/1.1 <status> <원래 문구>\r\n" + 백엔드 헤더 (hop-by-hop 뺌)
 *   + keep_alive 가 0 이면 "Connection: close\r\n" + "\r\n"
 * keep_alive 가 1 이면 Connection 헤더를 쓰지 않는다 (HTTP/1.1 기본 = 유지).
 * 반환: 쓴 바이트 수. cap 이 부족하면 -1.
 */
int proxy_format_response_head(const struct proxy_response *r, int keep_alive,
                               char *out, size_t cap);

/*
 * 백엔드에 연결한다. timeout_ms 안에 연결되지 않으면 실패 (errno = ETIMEDOUT).
 * 응답 없는 IP 로의 connect 가 커널 기본값(1~2분)까지 막히지 않게 한다.
 * 소켓은 close-on-exec (CGI 자식이 물려받지 않게). 돌려줄 때는 blocking 모드.
 * 반환: 연결된 fd, 실패면 -1 (errno 유지: ECONNREFUSED, ETIMEDOUT 등).
 */
int proxy_connect(const struct proxy_backend *b, int timeout_ms);

#endif
