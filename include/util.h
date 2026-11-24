#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>

ssize_t readn(int fd, void *buf, size_t n);
ssize_t readn_stop(int fd, void *buf, size_t n,
                   const volatile sig_atomic_t *stop);
ssize_t readn_deadline(int fd, void *buf, size_t n, uint64_t deadline_ms,
                       const volatile sig_atomic_t *stop);
uint64_t now_ms(void);
ssize_t writen(int fd, const void *buf, size_t n);
ssize_t write_body(int fd, const void *buf, size_t n);
void lingering_close(int fd);
void log_error(const char *fmt, ...);
const char *mime_type(const char *path);
int http_date_format(time_t t, char *buf, size_t len);
int http_date_parse(const char *s, time_t *out);

#endif
