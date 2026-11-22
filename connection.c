#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "connection.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef CONNECTION_TESTABLE
static int (*conn_clock_gettime_fn)(clockid_t, struct timespec *) = clock_gettime;
static int (*conn_setsockopt_fn)(int, int, int, const void *, socklen_t) = setsockopt;
static ssize_t (*conn_recv_fn)(int, void *, size_t, int) = recv;
static ssize_t (*conn_send_fn)(int, const void *, size_t, int) = send;

void connection_test_set_clock_gettime(int (*fn)(clockid_t, struct timespec *)) {
    conn_clock_gettime_fn = fn;
}

void connection_test_set_setsockopt(int (*fn)(int, int, int, const void *, socklen_t)) {
    conn_setsockopt_fn = fn;
}

void connection_test_set_recv(ssize_t (*fn)(int, void *, size_t, int)) {
    conn_recv_fn = fn;
}

void connection_test_set_send(ssize_t (*fn)(int, const void *, size_t, int)) {
    conn_send_fn = fn;
}

#define CONN_CLOCK_GETTIME conn_clock_gettime_fn
#define CONN_SETSOCKOPT conn_setsockopt_fn
#define CONN_RECV conn_recv_fn
#define CONN_SEND conn_send_fn
#else
#define CONN_CLOCK_GETTIME clock_gettime
#define CONN_SETSOCKOPT setsockopt
#define CONN_RECV recv
#define CONN_SEND send
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int timespec_gt(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec != b->tv_sec) {
        return a->tv_sec > b->tv_sec;
    }
    return a->tv_nsec > b->tv_nsec;
}

int connection_deadline_now(struct timespec *ts) {
    return CONN_CLOCK_GETTIME(CLOCK_MONOTONIC, ts);
}

int connection_deadline_add(const struct timespec *base, uint32_t timeout_ms,
                            const struct timespec *stage_min,
                            struct timespec *out) {
    time_t sec = base->tv_sec + (time_t)(timeout_ms / 1000u);
    long nsec = base->tv_nsec + (long)(timeout_ms % 1000u) * 1000000L;
    if (nsec >= 1000000000L) {
        sec++;
        nsec -= 1000000000L;
    }
    out->tv_sec = sec;
    out->tv_nsec = nsec;
    if (stage_min != NULL && timespec_gt(out, stage_min)) {
        *out = *stage_min;
    }
    return 0;
}

int connection_deadline_from_now(uint32_t timeout_ms,
                                 const struct timespec *stage_min,
                                 struct timespec *out) {
    struct timespec now;
    if (CONN_CLOCK_GETTIME(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return connection_deadline_add(&now, timeout_ms, stage_min, out);
}

int64_t connection_deadline_remaining_ms(const struct timespec *deadline) {
    struct timespec now;
    if (CONN_CLOCK_GETTIME(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    int64_t sec = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    int64_t nsec = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    int64_t remaining = sec * 1000000000LL + nsec;
    return remaining > 0 ? (remaining + 999999LL) / 1000000LL : 0;
}

int connection_set_socket_timeout(int fd, int optname,
                                  const struct timespec *deadline) {
    int64_t ms = connection_deadline_remaining_ms(deadline);
    if (ms <= 0) {
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = (time_t)(ms / 1000LL);
    tv.tv_usec = (suseconds_t)((ms % 1000LL) * 1000LL);
    return CONN_SETSOCKOPT(fd, SOL_SOCKET, optname, &tv, sizeof(tv));
}

static conn_result_t classify_recv_error(int err,
                                         const struct timespec *deadline) {
    if (err == ECONNRESET || err == EPROTO) {
        return CONN_RESULT_RESET;
    }
    if (err == EAGAIN || err == EWOULDBLOCK) {
        if (connection_deadline_remaining_ms(deadline) <= 0) {
            return CONN_RESULT_TIMEOUT;
        }
        return CONN_RESULT_AGAIN;
    }
    return CONN_RESULT_ERROR;
}

conn_result_t connection_recv_some(int fd, void *buf, size_t max_len,
                                   const struct timespec *deadline,
                                   size_t *out_read) {
    *out_read = 0;
    for (;;) {
        if (connection_deadline_remaining_ms(deadline) <= 0) {
            return CONN_RESULT_TIMEOUT;
        }
        if (connection_set_socket_timeout(fd, SO_RCVTIMEO, deadline) != 0) {
            if (connection_deadline_remaining_ms(deadline) <= 0) {
                return CONN_RESULT_TIMEOUT;
            }
            return CONN_RESULT_ERROR;
        }
        ssize_t r = CONN_RECV(fd, buf, max_len, 0);
        if (r > 0) {
            *out_read = (size_t)r;
            return connection_deadline_remaining_ms(deadline) > 0 ?
                   CONN_RESULT_OK : CONN_RESULT_TIMEOUT;
        }
        if (r == 0) {
            return CONN_RESULT_EOF;
        }
        if (errno == EINTR) {
            continue;
        }
        conn_result_t res = classify_recv_error(errno, deadline);
        if (res == CONN_RESULT_AGAIN) {
            continue;
        }
        return res;
    }
}

conn_result_t connection_recv_all(int fd, void *buf, size_t len,
                                  const struct timespec *deadline,
                                  size_t *out_done) {
    size_t done = 0;
    while (done < len) {
        if (connection_deadline_remaining_ms(deadline) <= 0) {
            *out_done = done;
            return CONN_RESULT_TIMEOUT;
        }
        if (connection_set_socket_timeout(fd, SO_RCVTIMEO, deadline) != 0) {
            if (connection_deadline_remaining_ms(deadline) <= 0) {
                *out_done = done;
                return CONN_RESULT_TIMEOUT;
            }
            *out_done = done;
            return CONN_RESULT_ERROR;
        }
        ssize_t r = CONN_RECV(fd, (char *)buf + done, len - done, 0);
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        if (r == 0) {
            *out_done = done;
            return CONN_RESULT_EOF;
        }
        if (errno == EINTR) {
            continue;
        }
        conn_result_t res = classify_recv_error(errno, deadline);
        if (res == CONN_RESULT_AGAIN) {
            continue;
        }
        *out_done = done;
        return res;
    }
    *out_done = done;
    return CONN_RESULT_OK;
}

static conn_result_t classify_send_error(int err,
                                         const struct timespec *deadline) {
    if (err == EPIPE || err == ECONNRESET) {
        return CONN_RESULT_RESET;
    }
    if (err == EAGAIN || err == EWOULDBLOCK) {
        if (connection_deadline_remaining_ms(deadline) <= 0) {
            return CONN_RESULT_TIMEOUT;
        }
        return CONN_RESULT_AGAIN;
    }
    return CONN_RESULT_ERROR;
}

conn_result_t connection_send_all(int fd, const void *buf, size_t len,
                                  const struct timespec *deadline,
                                  size_t *out_done) {
    size_t done = 0;
    while (done < len) {
        if (connection_deadline_remaining_ms(deadline) <= 0) {
            *out_done = done;
            return CONN_RESULT_TIMEOUT;
        }
        if (connection_set_socket_timeout(fd, SO_SNDTIMEO, deadline) != 0) {
            if (connection_deadline_remaining_ms(deadline) <= 0) {
                *out_done = done;
                return CONN_RESULT_TIMEOUT;
            }
            *out_done = done;
            return CONN_RESULT_ERROR;
        }
        ssize_t s = CONN_SEND(fd, (const char *)buf + done, len - done,
                              MSG_NOSIGNAL);
        if (s > 0) {
            done += (size_t)s;
            continue;
        }
        if (s == 0) {
            *out_done = done;
            return CONN_RESULT_ERROR;
        }
        if (errno == EINTR) {
            continue;
        }
        conn_result_t res = classify_send_error(errno, deadline);
        if (res == CONN_RESULT_AGAIN) {
            continue;
        }
        *out_done = done;
        return res;
    }
    *out_done = done;
    return CONN_RESULT_OK;
}

int connection_stopwatch_start(struct timespec *out) {
    return CONN_CLOCK_GETTIME(CLOCK_MONOTONIC, out);
}

uint64_t connection_stopwatch_elapsed_ms(const struct timespec *start) {
    struct timespec now;
    if (CONN_CLOCK_GETTIME(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    int64_t sec = (int64_t)now.tv_sec - (int64_t)start->tv_sec;
    int64_t nsec = (int64_t)now.tv_nsec - (int64_t)start->tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000LL;
    }
    return (uint64_t)(sec * 1000LL + nsec / 1000000LL);
}

conn_result_t connection_drain(int fd, size_t max_bytes, size_t *out_drained) {
    *out_drained = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return CONN_RESULT_ERROR;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return CONN_RESULT_ERROR;
    }
    char buf[4096];
    while (*out_drained < max_bytes) {
        size_t want = max_bytes - *out_drained;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        ssize_t r = read(fd, buf, want);
        if (r > 0) {
            *out_drained += (size_t)r;
            continue;
        }
        if (r == 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        fcntl(fd, F_SETFL, flags);
        return CONN_RESULT_ERROR;
    }
    fcntl(fd, F_SETFL, flags);
    return CONN_RESULT_OK;
}

static int json_escape_to_buf(const char *s, char *out, size_t out_sz) {
    if (s == NULL) {
        return snprintf(out, out_sz, "null");
    }
    size_t j = 0;
    if (j < out_sz) {
        out[j] = '"';
    }
    j++;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        const char *esc = NULL;
        char tmp[7];
        switch (*p) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\f':
            esc = "\\f";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\r':
            esc = "\\r";
            break;
        case '\t':
            esc = "\\t";
            break;
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                esc = tmp;
            }
            break;
        }
        if (esc != NULL) {
            size_t k = 0;
            while (esc[k] != '\0') {
                if (j < out_sz) {
                    out[j] = esc[k];
                }
                j++;
                k++;
            }
        } else {
            if (j < out_sz) {
                out[j] = (char)*p;
            }
            j++;
        }
    }
    if (j < out_sz) {
        out[j] = '"';
    }
    j++;
    if (j < out_sz) {
        out[j] = '\0';
    }
    return (int)j;
}

int connection_log_access(int fd, const conn_access_record_t *rec) {
    char buf[PIPE_BUF + 1];
    char method_str[128];
    char path_str[CONNECTION_MAX_PATH_LEN * 6 + 4];
    char status_str[16];
    const char *path_out = rec->path;
    if (path_out != NULL && strlen(path_out) > CONNECTION_MAX_PATH_LEN) {
        path_out = CONNECTION_PATH_TRUNCATED;
    }

    int needed_method = json_escape_to_buf(rec->method, method_str, sizeof(method_str));
    if (needed_method >= (int)sizeof(method_str)) {
        snprintf(method_str, sizeof(method_str), "null");
    }

    int needed_path = json_escape_to_buf(path_out, path_str, sizeof(path_str));
    if (needed_path >= (int)sizeof(path_str)) {
        snprintf(path_str, sizeof(path_str), "\"" CONNECTION_PATH_TRUNCATED "\"");
    }

    if (rec->status < 0) {
        snprintf(status_str, sizeof(status_str), "null");
    } else {
        snprintf(status_str, sizeof(status_str), "%d", rec->status);
    }

    int n = snprintf(buf, sizeof(buf),
                     "{\"method\":%s,\"path\":%s,\"status\":%s,"
                     "\"duration_ms\":%llu,\"bytes_sent\":%llu,\"complete\":%s}\n",
                     method_str, path_str, status_str,
                     (unsigned long long)rec->duration_ms,
                     (unsigned long long)rec->bytes_sent,
                     rec->complete ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        json_escape_to_buf(CONNECTION_PATH_TRUNCATED, path_str, sizeof(path_str));
        if (needed_path >= (int)sizeof(path_str)) {
            snprintf(path_str, sizeof(path_str), "\"" CONNECTION_PATH_TRUNCATED "\"");
        }
        n = snprintf(buf, sizeof(buf),
                     "{\"method\":%s,\"path\":%s,\"status\":%s,"
                     "\"duration_ms\":%llu,\"bytes_sent\":%llu,\"complete\":%s}\n",
                     method_str, path_str, status_str,
                     (unsigned long long)rec->duration_ms,
                     (unsigned long long)rec->bytes_sent,
                     rec->complete ? "true" : "false");
        if (n < 0 || (size_t)n >= sizeof(buf)) {
            return -1;
        }
    }

    ssize_t w = write(fd, buf, (size_t)n);
    if (w < 0) {
        return -1;
    }
    return (int)w;
}

int connection_log_ready(int fd, int port) {
    char buf[64];
    int n;
    if (port >= 0) {
        n = snprintf(buf, sizeof(buf), "READY %d\n", port);
    } else {
        n = snprintf(buf, sizeof(buf), "READY\n");
    }
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return -1;
    }
    ssize_t w = write(fd, buf, (size_t)n);
    return (w < 0) ? -1 : (int)w;
}

int connection_log_fd_prepare(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    int new_fd;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        new_fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    } else {
        new_fd = dup(fd);
    }
    return new_fd;
}

int connection_log_diag(int fd, const conn_diag_record_t *rec) {
    char phase_str[128];
    char msg_str[256];
    char buf[PIPE_BUF + 1];

    json_escape_to_buf(rec->phase, phase_str, sizeof(phase_str));
    json_escape_to_buf(rec->message, msg_str, sizeof(msg_str));

    int n = snprintf(buf, sizeof(buf),
                     "{\"phase\":%s,\"error\":%d,\"message\":%s}\n",
                     phase_str, rec->error_code, msg_str);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return -1;
    }
    ssize_t w = write(fd, buf, (size_t)n);
    return (w < 0) ? -1 : (int)w;
}
