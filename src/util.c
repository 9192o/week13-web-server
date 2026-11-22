#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

ssize_t readn_stop(int fd, void *buf, size_t n,
                   const volatile sig_atomic_t *stop) {
    size_t nleft = n;
    char *p = buf;

    while (nleft > 0) {
        ssize_t r = read(fd, p, nleft);
        if (r < 0) {
            if (errno == EINTR) {
                if (stop != NULL && *stop) {
                    errno = EINTR;
                    return -1;
                }
                continue;
            }
            return -1;
        }
        if (r == 0)
            break;
        nleft -= (size_t)r;
        p += r;
    }
    return (ssize_t)(n - nleft);
}

ssize_t readn(int fd, void *buf, size_t n) {
    return readn_stop(fd, buf, n, NULL);
}

ssize_t writen(int fd, const void *buf, size_t n) {
    size_t nleft = n;
    const char *p = buf;

    while (nleft > 0) {
        ssize_t r = write(fd, p, nleft);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        nleft -= (size_t)r;
        p += r;
    }
    return (ssize_t)(n - nleft);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL)
        return "application/octet-stream";

    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp(dot, ".css") == 0)
        return "text/css; charset=utf-8";
    if (strcasecmp(dot, ".js") == 0)
        return "text/javascript; charset=utf-8";
    if (strcasecmp(dot, ".json") == 0)
        return "application/json";
    if (strcasecmp(dot, ".png") == 0)
        return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcasecmp(dot, ".svg") == 0)
        return "image/svg+xml";
    if (strcasecmp(dot, ".txt") == 0)
        return "text/plain; charset=utf-8";
    if (strcasecmp(dot, ".ico") == 0)
        return "image/x-icon";
    if (strcasecmp(dot, ".pdf") == 0)
        return "application/pdf";

    return "application/octet-stream";
}

static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy =
        (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static time_t timegm_utc(int year, int month, int day,
                         int hour, int min, int sec) {
    int64_t days = days_from_civil(year, month, day);
    return (time_t)(days * 86400 + hour * 3600 + min * 60 + sec);
}

static int month_index(const char *name) {
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    for (int i = 0; i < 12; i++) {
        if (strcasecmp(name, months[i]) == 0)
            return i + 1;
    }
    return -1;
}

int http_date_format(time_t t, char *buf, size_t len) {
    static const char *weekdays[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    struct tm tm;
    if (gmtime_r(&t, &tm) == NULL)
        return -1;
    if (tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11)
        return -1;

    int n = snprintf(buf, len, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                     weekdays[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
                     tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return (n < 0 || (size_t)n >= len) ? -1 : 0;
}

int http_date_parse(const char *s, time_t *out) {
    char month[4] = {0};
    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    int consumed = 0;

    int n = sscanf(s, "%*3s, %d %3s %d %d:%d:%d GMT%n",
                   &day, month, &year, &hour, &min, &sec, &consumed);
    if (n != 6 || s[consumed] != '\0')
        return -1;

    int mon = month_index(month);
    if (mon < 0)
        return -1;
    if (day < 1 || day > 31 || year < 1970 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 ||
        sec < 0 || sec > 60)
        return -1;

    *out = timegm_utc(year, mon, day, hour, min, sec);
    return 0;
}
