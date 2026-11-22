#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <sys/types.h>

/* 요청 라인 + 헤더를 담는 고정 버퍼 크기. URI 상한(8192)보다 넉넉하게. */
#define REQUEST_HEAD_MAX 16384

/* 요청 라인 + 헤더 전체를 받아야 하는 데드라인 (read 한 번이 아니라 전체 기준) */
#define HEADER_TIMEOUT_MS 5000

/* 에러 응답 뒤 남은 수신 데이터를 비울 때의 한도 (lingering close) */
#define LINGER_MAX_BYTES  (64 * 1024)
#define LINGER_TIMEOUT_MS 1000

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

/* accept 루프. 연결을 하나씩 순차 처리한다 (Phase 1). */
void server_run(int listen_fd);

/*
 * 연결 하나 처리: 헤더를 읽고 파싱한 뒤 응답하고 닫는다.
 * peer 는 접근 로그에 찍을 "ip:port" 문자열.
 */
void server_handle(int conn_fd, const char *peer);

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

/* 부분 쓰기와 EINTR 을 처리하며 len 바이트를 모두 쓴다. 실패 시 -1. */
int write_all(int fd, const void *buf, size_t len);

#endif
