#include "http_response.h"

#include <stdio.h>

static const struct {
    int status;
    const char *reason;
} reasons[] = {
    {200, "OK"},
    {301, "Moved Permanently"},
    {400, "Bad Request"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {408, "Request Timeout"},
    {411, "Length Required"},
    {413, "Content Too Large"},
    {414, "URI Too Long"},
    {431, "Request Header Fields Too Large"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {505, "HTTP Version Not Supported"},
};

const char *http_reason(int status)
{
    size_t i;

    for (i = 0; i < sizeof reasons / sizeof reasons[0]; i++)
        if (reasons[i].status == status)
            return reasons[i].reason;
    return NULL;
}

int http_normalize_status(int status)
{
    if (http_reason(status) != NULL)
        return status;
    fprintf(stderr, "BUG: unknown status %d\n", status);
    return 500;
}

int http_format_head(char *buf, size_t cap, int status,
                     const char *content_type, long long content_length,
                     const char *extra, int keep_alive, time_t now)
{
    return http_format_head_raw(buf, cap, http_normalize_status(status),
                                content_type, content_length, extra,
                                keep_alive, now);
}

int http_format_head_raw(char *buf, size_t cap, int status,
                         const char *content_type, long long content_length,
                         const char *extra, int keep_alive, time_t now)
{
    const char *reason = http_reason(status);
    char date[64];
    struct tm tm;
    int n;

    if (reason == NULL)
        reason = "";           /* 표에 없는 코드: 빈 문구 ("HTTP/1.1 201 ") */

    /* RFC 9110 IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT" (C 로캘 기준) */
    if (gmtime_r(&now, &tm) == NULL ||
        strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0)
        date[0] = '\0';

    n = snprintf(buf, cap,
                 "HTTP/1.1 %d %s\r\n"
                 "Date: %s\r\n"
                 "Server: " HTTP_SERVER_NAME "\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %lld\r\n"
                 "%s"
                 "%s"
                 "\r\n",
                 status, reason, date, content_type,
                 content_length, extra ? extra : "",
                 keep_alive ? "" : "Connection: close\r\n");
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "BUG: response head overflow\n");
        return -1;
    }
    return n;
}

int http_format_error_body(char *buf, size_t cap, int status)
{
    const char *reason;
    int n;

    status = http_normalize_status(status);
    reason = http_reason(status);
    n = snprintf(buf, cap,
                 "<!doctype html>\n"
                 "<html><head><meta charset=\"utf-8\"><title>%d %s</title></head>\n"
                 "<body><h1>%d %s</h1><hr><p>" HTTP_SERVER_NAME "</p></body></html>\n",
                 status, reason, status, reason);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}
