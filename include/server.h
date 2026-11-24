#ifndef SERVER_H
#define SERVER_H

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

/* 요청 라인 + 헤더를 담는 고정 버퍼 크기. URI 상한(8192)보다 넉넉하게. */
#define REQUEST_HEAD_MAX 16384

/*
 * 시간 제한 값(*_TIMEOUT_MS)은 빌드할 때 -D 로 바꿀 수 있다.
 * make ftest 는 짧은 값으로 빌드해서 시간 제한 시나리오를 몇 초 안에 돌린다.
 */

/* 요청 라인 + 헤더 전체를 받아야 하는 데드라인 (read 한 번이 아니라 전체 기준) */
#ifndef HEADER_TIMEOUT_MS
#define HEADER_TIMEOUT_MS 5000
#endif

/* 에러 응답 뒤 남은 수신 데이터를 비울 때의 한도 (lingering close) */
#define LINGER_MAX_BYTES  (64 * 1024)
#ifndef LINGER_TIMEOUT_MS
#define LINGER_TIMEOUT_MS 1000
#endif

/* 접근 로그에 남길 요청 첫 줄의 최대 바이트 (넘으면 "...") */
#define REQLINE_LOG_MAX 200

/* POST 본문 상한 (Content-Length 가 넘으면 읽기 전에 413) */
#define REQUEST_BODY_MAX (1024 * 1024)

/* 헤더를 다 받은 뒤 새로 시작하는 본문 전체 데드라인 (넘으면 408) */
#ifndef BODY_TIMEOUT_MS
#define BODY_TIMEOUT_MS 10000
#endif

/* 응답 하나(헤더 + 본문) 전체를 보내야 하는 데드라인 (넘으면 그냥 close) */
#ifndef SEND_TIMEOUT_MS
#define SEND_TIMEOUT_MS 30000
#endif

enum send_result {
    SEND_OK = 0,
    SEND_ERROR = -1,     /* EPIPE, ECONNRESET 등 */
    SEND_TIMEOUT = -2    /* deadline 까지 다 보내지 못함 */
};

enum read_head_result {
    READ_HEAD_OK,        /* \r\n\r\n 까지 받음 */
    READ_HEAD_CLOSED,    /* 헤더 끝 전에 클라이언트가 끊음 (read == 0) */
    READ_HEAD_ERROR,     /* read/poll 오류 (ECONNRESET 등) */
    READ_HEAD_TOO_LARGE, /* 버퍼가 찰 때까지 헤더 끝이 없음 */
    READ_HEAD_TIMEOUT    /* 데드라인 안에 헤더 끝이 오지 않음 */
};

/*
 * socket → SO_REUSEADDR → bind(INADDR_ANY:port) → listen.
 * 성공하면 listen 소켓 fd, 실패하면 어느 단계인지 perror 후 -1.
 */
int server_listen(int port);

/*
 * SIGINT/SIGTERM 핸들러가 1 로 켜는 종료 플래그 (main.c 가 SA_RESTART 없이 등록).
 * accept 가 EINTR 로 돌아오면 server_run 이 이를 보고 반환한다.
 */
extern volatile sig_atomic_t server_stop;

/*
 * accept 루프 + 워커 스레드 풀 (Phase 2).
 * accept 한 연결을 큐(queue_cap 칸)에 넣고, 워커 workers 개가 꺼내 server_handle 한다.
 * 큐가 가득 차면 기다리지 않고 503 을 보내고 닫는다 (큐에 넣지 않음).
 * server_stop 이 켜지면: accept 를 멈추고, 워커는 들고 있던 연결만 끝내고 종료(join),
 * 큐에 남아 있던 연결은 응답 없이 닫는다. listen_fd 는 닫지 않는다.
 * 반환: 0 정상 종료, -1 큐/워커를 만들지 못함.
 */
int server_run(int listen_fd, const char *root, int workers, int queue_cap);

/*
 * 연결 하나 처리: 헤더를 읽고 파싱한 뒤 응답하고 닫는다.
 * peer 는 접근 로그에 찍을 "ip:port" 문자열, root 는 정적 파일 루트(realpath).
 */
void server_handle(int conn_fd, const char *peer, const char *root);

/*
 * buf[0..len) 에서 "\r\n\r\n" 을 찾아 그 다음 위치(본문 시작)를 돌려준다.
 * 없으면 NULL.
 */
const char *find_header_end(const char *buf, size_t len);

/*
 * fd 에서 헤더 끝(\r\n\r\n)이 나올 때까지 buf 에 누적해서 읽는다.
 * 전체가 timeout_ms 안에 와야 한다 (poll 로 남은 시간만큼만 기다림).
 * *out_len 에는 읽은 총 바이트 수(헤더 뒤에 붙어 온 본문 일부 포함 가능).
 * TIMEOUT 일 때 *out_len 으로 0바이트(무응답)와 1바이트 이상(408)을 구분한다.
 */
enum read_head_result read_request_head(int fd, char *buf, size_t cap,
                                        int timeout_ms, size_t *out_len);

/*
 * 부분 쓰기와 EINTR 을 처리하며 len 바이트를 모두 쓴다. 실패 시 -1.
 * 시간 제한이 없으므로 클라이언트 소켓에는 send_all 을 쓴다 (이건 파이프용).
 */
int write_all(int fd, const void *buf, size_t len);

/*
 * 소켓 fd 로 len 바이트를 모두 보낸다. deadline(now_ms 기준)이 지나면 SEND_TIMEOUT.
 * send 는 MSG_DONTWAIT 라 막히지 않고, 소켓 버퍼가 차면 poll 로 남은 시간만큼만 기다린다.
 * 조금씩 계속 받아 가는 클라이언트여도 deadline 을 넘기면 멈춘다 (응답 전체 기준).
 * send 를 쓰므로 소켓 전용 (파이프에는 write_all).
 */
enum send_result send_all(int fd, const void *buf, size_t len, long long deadline);

/* CLOCK_MONOTONIC 기준 밀리초 (데드라인 계산용) */
long long now_ms(void);

/*
 * deadline(now_ms 기준)까지 fd 가 읽을 수 있게 되길 기다린다.
 * 반환: 1 읽기 가능, 0 시간 초과, -1 poll 오류.
 */
int wait_readable(int fd, long long deadline);

#endif
