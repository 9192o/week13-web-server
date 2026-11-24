#include "logger.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_t thread;
    char *lines;
    int capacity;
    int head;
    int tail;
    int count;
    int out_fd;
    int shutdown;
    int started;
} logger_t;

static logger_t g_logger;
static _Atomic int g_running;
static _Atomic size_t g_dropped;

static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n > 0) {
            buf += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
}

static void *logger_main(void *arg) {
    logger_t *log = arg;
    char line[LOGGER_LINE_MAX];

    for (;;) {
        pthread_mutex_lock(&log->mutex);
        while (log->count == 0 && !log->shutdown)
            pthread_cond_wait(&log->not_empty, &log->mutex);
        if (log->count == 0 && log->shutdown) {
            pthread_mutex_unlock(&log->mutex);
            break;
        }

        memcpy(line, log->lines + (size_t)log->head * LOGGER_LINE_MAX,
               LOGGER_LINE_MAX);
        log->head = (log->head + 1) % log->capacity;
        log->count--;
        pthread_mutex_unlock(&log->mutex);

        write_all(log->out_fd, line, strnlen(line, LOGGER_LINE_MAX));
    }

    return NULL;
}

static void logger_destroy(void) {
    logger_t *log = &g_logger;
    pthread_cond_destroy(&log->not_empty);
    pthread_mutex_destroy(&log->mutex);
    free(log->lines);
    memset(log, 0, sizeof *log);
    g_running = 0;
}

int logger_start(int out_fd, int capacity) {
    if (g_running)
        return 0;
    if (capacity <= 0)
        capacity = LOGGER_DEFAULT_QUEUE;

    logger_t *log = &g_logger;
    memset(log, 0, sizeof *log);
    log->lines = calloc((size_t)capacity, LOGGER_LINE_MAX);
    if (log->lines == NULL)
        return -1;
    log->capacity = capacity;
    log->out_fd = out_fd;

    if (pthread_mutex_init(&log->mutex, NULL) != 0) {
        free(log->lines);
        log->lines = NULL;
        return -1;
    }
    if (pthread_cond_init(&log->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&log->mutex);
        free(log->lines);
        log->lines = NULL;
        return -1;
    }

    g_dropped = 0;
    if (pthread_create(&log->thread, NULL, logger_main, log) != 0) {
        pthread_cond_destroy(&log->not_empty);
        pthread_mutex_destroy(&log->mutex);
        free(log->lines);
        log->lines = NULL;
        return -1;
    }

    log->started = 1;
    g_running = 1;
    return 0;
}

void logger_stop(void) {
    logger_t *log = &g_logger;
    if (!g_running || !log->started)
        return;

    if (g_dropped > 0)
        logger_log("logger: dropped %zu lines", (size_t)g_dropped);

    pthread_mutex_lock(&log->mutex);
    log->shutdown = 1;
    pthread_cond_broadcast(&log->not_empty);
    pthread_mutex_unlock(&log->mutex);

    pthread_join(log->thread, NULL);
    log->started = 0;
    logger_destroy();
}

static void logger_enqueue(const char *line, size_t len) {
    if (len == 0)
        return;

    if (!g_running) {
        write_all(STDERR_FILENO, line, len);
        return;
    }

    logger_t *log = &g_logger;
    pthread_mutex_lock(&log->mutex);
    if (log->count == log->capacity) {
        g_dropped++;
        pthread_mutex_unlock(&log->mutex);
        return;
    }

    char *slot = log->lines + (size_t)log->tail * LOGGER_LINE_MAX;
    size_t copy = len < LOGGER_LINE_MAX ? len : LOGGER_LINE_MAX - 1;
    memcpy(slot, line, copy);
    slot[copy] = '\0';
    log->tail = (log->tail + 1) % log->capacity;
    log->count++;
    pthread_cond_signal(&log->not_empty);
    pthread_mutex_unlock(&log->mutex);
}

void logger_vlog(const char *fmt, va_list ap) {
    char line[LOGGER_LINE_MAX];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n < 0)
        return;

    size_t len = (size_t)n;
    if (len >= sizeof line)
        len = sizeof line - 1;
    if (len == 0 || line[len - 1] != '\n') {
        if (len + 1 < sizeof line) {
            line[len++] = '\n';
            line[len] = '\0';
        }
    }
    logger_enqueue(line, len);
}

void logger_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logger_vlog(fmt, ap);
    va_end(ap);
}

void logger_access(const char *ip, const char *method, const char *path,
                   int status, size_t bytes) {
    char line[LOGGER_LINE_MAX];
    int n = snprintf(line, sizeof line, "%s \"%s %s\" %d %zu\n",
                     ip != NULL ? ip : "-",
                     method != NULL ? method : "-",
                     path != NULL ? path : "-",
                     status, bytes);
    if (n <= 0)
        return;
    size_t len = (size_t)n;
    if (len >= sizeof line)
        len = sizeof line - 1;
    logger_enqueue(line, len);
}

int logger_running(void) {
    return g_running != 0;
}

size_t logger_dropped(void) {
    return (size_t)g_dropped;
}
