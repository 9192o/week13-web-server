#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>

ssize_t readn(int fd, void *buf, size_t n);
ssize_t readn_stop(int fd, void *buf, size_t n,
                   const volatile sig_atomic_t *stop);
ssize_t writen(int fd, const void *buf, size_t n);
void log_error(const char *fmt, ...);
const char *mime_type(const char *path);
int http_date_format(time_t t, char *buf, size_t len);
int http_date_parse(const char *s, time_t *out);

#endif
