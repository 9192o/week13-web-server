#include "url.h"

#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int url_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;

    while (*in != '\0') {
        char c = *in;

        if (c == '%') {
            int hi = hex_value(in[1]);
            int lo = hi < 0 ? -1 : hex_value(in[2]);   /* in[1] 이 NUL 이면 in[2] 를 보지 않음 */
            if (hi < 0 || lo < 0)
                return -1;
            c = (char)(hi * 16 + lo);
            if (c == '\0')                              /* %00: C 문자열을 자를 수 있음 */
                return -1;
            in += 3;
        } else {
            in++;
        }
        if (o + 1 >= cap)
            return -1;
        out[o++] = c;
    }
    if (cap == 0)
        return -1;
    out[o] = '\0';
    return 0;
}

enum url_path_check url_check_segments(const char *path)
{
    enum url_path_check result = URL_PATH_OK;
    const char *seg = path;

    for (;;) {
        size_t n = strcspn(seg, "/");

        if (n == 2 && seg[0] == '.' && seg[1] == '.')
            return URL_PATH_DOTDOT;            /* 가장 심각한 것이므로 바로 반환 */
        if (n > 0 && seg[0] == '.')
            result = URL_PATH_DOTFILE;         /* 뒤에 ".." 이 있을 수 있어 계속 본다 */
        if (seg[n] == '\0')
            return result;
        seg += n + 1;
    }
}

int url_check_path(const char *raw, char *decoded, size_t cap)
{
    if (url_decode(raw, decoded, cap) < 0)
        return 400;
    switch (url_check_segments(decoded)) {
    case URL_PATH_DOTDOT:
        return 400;
    case URL_PATH_DOTFILE:
        return 404;
    case URL_PATH_OK:
        break;
    }
    return 0;
}

int url_is_cgi(const char *decoded)
{
    size_t n = strlen(URL_CGI_PREFIX);

    return strncmp(decoded, URL_CGI_PREFIX, n) == 0 &&
           (decoded[n] == '\0' || decoded[n] == '/');
}
