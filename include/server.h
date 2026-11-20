#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <sys/types.h>

/* 요청 라인 + 헤더를 담는 고정 버퍼 크기. URI 상한(8192)보다 넉넉하게. */
#define REQUEST_HEAD_MAX 16384

enum read_head_result {
    READ_HEAD_OK,        /* \r\n\r\n 까지 받음 */
    READ_HEAD_CLOSED,    /* 헤더 끝 전에 클라이언트가 끊음 (read == 0) */
    READ_HEAD_ERROR,     /* read 오류 (ECONNRESET 등) */
    READ_HEAD_TOO_LARGE  /* 버퍼가 찰 때까지 헤더 끝이 없음 */
};

/*
 * socket → SO_REUSEADDR → bind(INADDR_ANY:port) → listen.
 * 성공하면 listen 소켓 fd, 실패하면 어느 단계인지 perror 후 -1.
 */
int server_listen(int port);

/* accept 루프. 연결을 하나씩 순차 처리한다 (Phase 1). */
void server_run(int listen_fd);

/* 연결 하나 처리: 헤더 끝까지 읽고 고정 응답을 보낸 뒤 close. */
void server_handle(int conn_fd);

/*
 * buf[0..len) 에서 "\r\n\r\n" 을 찾아 그 다음 위치(본문 시작)를 돌려준다.
 * 없으면 NULL.
 */
const char *find_header_end(const char *buf, size_t len);

/*
 * fd 에서 헤더 끝(\r\n\r\n)이 나올 때까지 buf 에 누적해서 읽는다.
 * *out_len 에는 읽은 총 바이트 수(헤더 뒤에 붙어 온 본문 일부 포함 가능).
 */
enum read_head_result read_request_head(int fd, char *buf, size_t cap,
                                        size_t *out_len);

/* 부분 쓰기와 EINTR 을 처리하며 len 바이트를 모두 쓴다. 실패 시 -1. */
int write_all(int fd, const void *buf, size_t len);

#endif
