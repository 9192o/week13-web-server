#ifndef CONNECTION_H
#define CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PHASE1_HEADER_MS
#define PHASE1_HEADER_MS 10000
#endif

#ifndef PHASE1_IDLE_MS
#define PHASE1_IDLE_MS 30000
#endif

#ifndef PHASE1_TOTAL_MS
#define PHASE1_TOTAL_MS 60000
#endif

#ifndef PHASE1_SEND_MS
#define PHASE1_SEND_MS 30000
#endif

typedef enum {
    CONN_RESULT_OK = 0,
    CONN_RESULT_TIMEOUT,
    CONN_RESULT_INTR,
    CONN_RESULT_AGAIN,
    CONN_RESULT_EOF,
    CONN_RESULT_RESET,
    CONN_RESULT_ERROR
} conn_result_t;

int connection_deadline_now(struct timespec *ts);
int connection_deadline_add(const struct timespec *base, uint32_t timeout_ms,
                            const struct timespec *stage_min,
                            struct timespec *out);
int connection_deadline_from_now(uint32_t timeout_ms,
                                 const struct timespec *stage_min,
                                 struct timespec *out);
int64_t connection_deadline_remaining_ms(const struct timespec *deadline);

int connection_set_socket_timeout(int fd, int optname,
                                  const struct timespec *deadline);

conn_result_t connection_recv_some(int fd, void *buf, size_t max_len,
                                   const struct timespec *deadline,
                                   size_t *out_read);
conn_result_t connection_recv_all(int fd, void *buf, size_t len,
                                  const struct timespec *deadline,
                                  size_t *out_done);
conn_result_t connection_send_all(int fd, const void *buf, size_t len,
                                  const struct timespec *deadline,
                                  size_t *out_done);

int connection_stopwatch_start(struct timespec *out);
uint64_t connection_stopwatch_elapsed_ms(const struct timespec *start);

conn_result_t connection_drain(int fd, size_t max_bytes, size_t *out_drained);

typedef struct {
    const char *method;
    const char *path;
    int status;
    uint64_t bytes_sent;
    bool complete;
    uint64_t duration_ms;
} conn_access_record_t;

int connection_log_access(int fd, const conn_access_record_t *rec);

#define CONNECTION_PATH_TRUNCATED "<truncated>"
#define CONNECTION_MAX_PATH_LEN 512

int connection_log_ready(int fd, int port);
int connection_log_fd_prepare(int fd);

typedef struct {
    const char *phase;
    int error_code;
    const char *message;
} conn_diag_record_t;

int connection_log_diag(int fd, const conn_diag_record_t *rec);

#ifdef CONNECTION_TESTABLE
void connection_test_set_clock_gettime(int (*fn)(clockid_t, struct timespec *));
void connection_test_set_setsockopt(int (*fn)(int, int, int, const void *, socklen_t));
void connection_test_set_recv(ssize_t (*fn)(int, void *, size_t, int));
void connection_test_set_send(ssize_t (*fn)(int, const void *, size_t, int));
#endif

#ifdef __cplusplus
}
#endif

#endif /* CONNECTION_H */
