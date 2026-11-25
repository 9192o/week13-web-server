#ifndef CONN_COMMON_H
#define CONN_COMMON_H

#include "config.h"
#include "event_backend.h"
#include "server_conn.h"

#include <netinet/in.h>
#include <stddef.h>

typedef int (*conn_process_fn)(conn_t *c, ev_loop_t *loop, void *ctx,
                               const config_t *cfg);

typedef struct {
    void (*detach)(conn_t *c);
    int (*next_timeout_ms)(void);
} conn_common_hooks_t;

void conn_common_set_hooks(const conn_common_hooks_t *hooks);

int conn_common_set_nonblocking(int fd);
int conn_common_set_cloexec(int fd);
int conn_common_create_listen_socket(int port);

int conn_common_setup_signals(void);
int conn_common_signal_fd(void);
void conn_common_close_signals(void);
int conn_common_stopping(void);

int conn_common_has_header_end(const char *buf, size_t len);
int conn_common_request_is_head(const char *buf, size_t len);
int conn_common_has_crlf(const char *buf, size_t len);
int conn_common_request_line_uri_too_long(const char *buf, size_t len,
                                          size_t max_uri);

conn_t *conn_common_new(int fd, const struct sockaddr_in *peer,
                        const config_t *cfg, ev_loop_t *loop);
void conn_common_free(conn_t *c);
void conn_common_close(ev_loop_t *loop, conn_t *c);
void conn_common_sweep(conn_t **head, int *count);

void conn_common_log_begin(conn_t *c, const char *method, const char *path);
void conn_common_log_response(conn_t *c, int status, size_t body_bytes);
void conn_common_log_end(conn_t *c);
void conn_common_reset_response(conn_t *c);

int conn_common_build_error(conn_t *c, int status, int close_connection,
                            int head, int retry_after, int timeout_sec);

void conn_common_reject_overloaded(int fd, const struct sockaddr_in *peer);
void conn_common_accept_new(int listenfd, ev_loop_t *loop, conn_t **head,
                            int *count, const config_t *cfg);
void conn_common_begin_drain(ev_loop_t *loop, int *listenfd, conn_t *head);
void conn_common_start_shutdown_watchdog(int timeout_ms);

int conn_common_next_timeout(conn_t *head);
void conn_common_expire_timers(ev_loop_t *loop, conn_t *head,
                               const config_t *cfg);

int conn_common_read_headers(conn_t *c, ev_loop_t *loop, const config_t *cfg,
                             conn_process_fn process, void *ctx);
int conn_common_on_writable(conn_t *c, ev_loop_t *loop, const config_t *cfg,
                            conn_process_fn process, void *ctx);

#endif
