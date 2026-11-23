/*
 * adder CGI: a, b 를 더해 text/plain 으로 출력한다.
 *   GET  → QUERY_STRING 에서, POST → stdin(CONTENT_LENGTH 바이트)에서 "a=1&b=2" 를 읽는다.
 *   다른 키는 무시. a/b 가 없거나 두 번 나오거나, 부호+숫자가 아니거나,
 *   범위를 넘거나 합이 넘치면 Status: 400.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INPUT_MAX (1024 * 1024)   /* 서버의 본문 상한과 같음 */

static int bad_request(void)
{
    printf("Status: 400\r\n"
           "Content-Type: text/plain; charset=utf-8\r\n"
           "\r\n"
           "invalid input: need integers a and b\n");
    return 0;                      /* 400 은 정상 응답이므로 exit 0 */
}

/* [s, s+len) 가 부호 하나(선택) + 숫자 1개 이상이고 long 범위면 0 */
static int parse_long(const char *s, size_t len, long *out)
{
    char tmp[32];
    char *end;
    size_t i = 0;

    if (len == 0 || len >= sizeof tmp)
        return -1;
    if (s[0] == '+' || s[0] == '-')
        i = 1;
    if (i == len)
        return -1;
    for (; i < len; i++)
        if (s[i] < '0' || s[i] > '9')
            return -1;
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    errno = 0;
    *out = strtol(tmp, &end, 10);
    if (errno == ERANGE || *end != '\0')
        return -1;
    return 0;
}

/* POST 본문을 CONTENT_LENGTH 만큼 읽는다. 실패하면 NULL */
static char *read_body(void)
{
    const char *cl = getenv("CONTENT_LENGTH");
    char *buf, *end;
    long len;
    size_t got = 0;

    if (cl == NULL)
        return NULL;
    errno = 0;
    len = strtol(cl, &end, 10);
    if (errno != 0 || end == cl || *end != '\0' || len < 0 || len > INPUT_MAX)
        return NULL;
    buf = malloc((size_t)len + 1);
    if (buf == NULL)
        return NULL;
    while (got < (size_t)len) {
        ssize_t n = read(STDIN_FILENO, buf + got, (size_t)len - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {              /* 본문이 덜 옴 */
            free(buf);
            return NULL;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    return buf;
}

int main(void)
{
    const char *method = getenv("REQUEST_METHOD");
    char *owned = NULL;
    const char *input, *p;
    long a = 0, b = 0, sum;
    int have_a = 0, have_b = 0;

    if (method != NULL && strcmp(method, "POST") == 0) {
        owned = read_body();
        if (owned == NULL)
            return bad_request();
        input = owned;
    } else {
        input = getenv("QUERY_STRING");
        if (input == NULL)
            input = "";
    }

    for (p = input; *p != '\0';) {
        size_t plen = strcspn(p, "&");
        const char *eq = memchr(p, '=', plen);

        if (eq != NULL) {
            size_t klen = (size_t)(eq - p);
            const char *v = eq + 1;
            size_t vlen = plen - klen - 1;

            if (klen == 1 && (p[0] == 'a' || p[0] == 'b')) {
                int *have = p[0] == 'a' ? &have_a : &have_b;
                long *dst = p[0] == 'a' ? &a : &b;
                if (*have || parse_long(v, vlen, dst) < 0) {
                    free(owned);
                    return bad_request();
                }
                *have = 1;
            }
        }
        p += plen;
        if (*p == '&')
            p++;
    }
    free(owned);

    if (!have_a || !have_b ||
        (b > 0 && a > LONG_MAX - b) || (b < 0 && a < LONG_MIN - b))
        return bad_request();
    sum = a + b;

    printf("Content-Type: text/plain; charset=utf-8\r\n"
           "\r\n"
           "%ld\n", sum);
    return 0;
}
