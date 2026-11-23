#ifndef BODY_H
#define BODY_H

#include <stddef.h>

/* 헤더를 읽은 버퍼와, 그 안에 이미 들어온 본문 앞부분에 대한 정보 */
struct body_input {
    const char *buf;          /* 헤더 버퍼 */
    size_t buffered_len;      /* buf 에 들어 있는 바이트 수 */
    size_t head_len;          /* 본문이 시작하는 오프셋 */
    long long content_length;
};

enum read_body_result {
    READ_BODY_OK,
    READ_BODY_CLOSED,         /* 본문을 다 받기 전에 클라이언트가 끊음 (S9) */
    READ_BODY_TIMEOUT,        /* 데드라인 안에 본문이 다 오지 않음 (408) */
    READ_BODY_ERROR           /* read/poll 오류 */
};

/*
 * 본문 content_length 바이트를 out 에 모은다. 먼저 buf[head_len..buffered_len) 에서
 * 가져오고, 모자라면 fd 에서 timeout_ms 전체 데드라인 안에 읽는다.
 * content_length 보다 많이 읽지 않는다. 상한 판단은 호출자 책임.
 * out 은 content_length 바이트 이상이어야 한다 (0 이면 NULL 가능).
 * *consumed: buf 에서 쓴 바이트 수 (헤더 + buf 에서 가져간 본문).
 *            READ_BODY_OK 일 때만 의미가 있다. 나머지는 다음 요청의 일부 (phase 2).
 */
enum read_body_result read_request_body(int fd, const struct body_input *in,
                                        int timeout_ms, char *out,
                                        size_t *consumed);

/*
 * read_request_body 와 같지만 본문을 저장하지 않고 읽어서 버린다 (phase 2 keep-alive 용).
 * 상한 판단은 호출자 책임. *consumed 의미도 같다.
 */
enum read_body_result discard_request_body(int fd, const struct body_input *in,
                                           int timeout_ms, size_t *consumed);

#endif
