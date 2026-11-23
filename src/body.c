#include "body.h"
#include "server.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#define DISCARD_BUF 4096

/* out 이 NULL 이면 버리기 모드 (함수 안 4KB 임시 버퍼로 읽음) */
static enum read_body_result body_loop(int fd, const struct body_input *in,
                                       int timeout_ms, char *out,
                                       size_t *consumed)
{
    char junk[DISCARD_BUF];
    long long need = in->content_length;
    long long got;
    size_t avail = in->buffered_len - in->head_len;
    size_t take = (long long)avail < need ? avail : (size_t)need;
    long long deadline;

    /* 헤더 버퍼에 같이 들어온 본문 앞부분 */
    if (out != NULL && take > 0)
        memcpy(out, in->buf + in->head_len, take);
    got = (long long)take;

    deadline = now_ms() + timeout_ms;
    while (got < need) {
        long long left = need - got;
        char *dst;
        size_t want;
        ssize_t n;
        int ready = wait_readable(fd, deadline);

        if (ready == 0)
            return READ_BODY_TIMEOUT;
        if (ready < 0)
            return READ_BODY_ERROR;

        if (out != NULL) {
            dst = out + got;
            want = (size_t)left;
        } else {
            dst = junk;
            want = left < (long long)sizeof junk ? (size_t)left : sizeof junk;
        }
        n = read(fd, dst, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return READ_BODY_ERROR;
        }
        if (n == 0)
            return READ_BODY_CLOSED;
        got += n;
    }

    *consumed = in->head_len + take;
    return READ_BODY_OK;
}

enum read_body_result read_request_body(int fd, const struct body_input *in,
                                        int timeout_ms, char *out,
                                        size_t *consumed)
{
    return body_loop(fd, in, timeout_ms, out, consumed);
}

enum read_body_result discard_request_body(int fd, const struct body_input *in,
                                           int timeout_ms, size_t *consumed)
{
    return body_loop(fd, in, timeout_ms, NULL, consumed);
}
