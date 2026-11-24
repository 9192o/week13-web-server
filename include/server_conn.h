#ifndef SERVER_CONN_H
#define SERVER_CONN_H

#include "access_log.h"
#include "config.h"
#include "ev_source.h"
#include "http.h"

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    ST_READ,
    ST_READ_BODY,
    ST_WAIT_TASK,
    ST_PROXY,
    ST_WRITE
} conn_state_t;

struct proxy_conn;
struct ev_loop;

typedef struct conn {
    ev_source_t source; /* must stay first: ev_event_t.ptr is ev_source_t * */

    int fd;
    struct sockaddr_in peer;
    conn_state_t state;
    int closed;

    char in[16384];
    size_t in_len;

    http_request_t req;

    char *out;
    size_t out_len;
    size_t out_sent;

    int file_fd;
    off_t file_offset;
    size_t file_remaining;
    char *file_buf;
    size_t file_buf_len;
    size_t file_buf_sent;

    int head_only;
    int close_after;
    int requests;
    uint64_t task_id;
    size_t body_received;
    uint64_t deadline_ms;
    int in_loop;

    access_log_t log;
    int log_has_request;

    struct proxy_conn *proxy;
    struct ev_loop *loop;
    const config_t *cfg;

    struct conn *next;
} conn_t;

int server_conn_build_error(conn_t *c, int status, int close_connection,
                            int head, int retry_after);
void server_conn_close(conn_t *c);
void server_conn_log_response(conn_t *c, int status, size_t body_bytes);
void server_conn_log_end(conn_t *c);
int server_is_stopping(void);

#endif
