#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>
#include <stddef.h>

#define LOGGER_LINE_MAX 512
#define LOGGER_DEFAULT_QUEUE 1024

/*
 * Single logger thread with a bounded queue. Enqueue only; the logger thread
 * writes whole lines to out_fd. When the queue is full, lines are dropped and
 * counted.
 */
int logger_start(int out_fd, int capacity);
void logger_stop(void);
int logger_running(void);

void logger_log(const char *fmt, ...);
void logger_vlog(const char *fmt, va_list ap);
void logger_access(const char *ip, const char *method, const char *path,
                   int status, size_t bytes);
size_t logger_dropped(void);

#endif
