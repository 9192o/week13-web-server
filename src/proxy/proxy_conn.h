#ifndef PROXY_CONN_H
#define PROXY_CONN_H

#include "proxy.h"

#include "chunked.h"
#include "http_upstream.h"
#include "proxy_internal.h"

#include <stdint.h>

#define PROXY_BUF_SIZE 65536

typedef enum {
    BODY_NONE,
    BODY_CONTENT_LENGTH,
    BODY_CHUNKED,
    BODY_CLOSE
} body_mode_t;

typedef enum {
    PROXY_CONNECTING,
    PROXY_SENDING,
    PROXY_READING_HEADERS,
    PROXY_RELAY_BODY,
    PROXY_DONE
} proxy_state_t;

typedef struct {
    char prefix[MAX_ROUTE_PREFIX];
    route_type_t type;
    int strip_prefix;
    proxy_backend_t backends[MAX_BACKENDS];
    int backend_count;
    int rr_index;
} proxy_route_t;

struct proxy_conn {
    ev_source_t source; /* must stay first */

    conn_t *client;
    int upstream_fd;
    int upstream_in_loop;
    proxy_state_t state;
    proxy_route_t *route;
    proxy_backend_t *backend;

    char *req_out;
    size_t req_out_len;
    size_t req_out_sent;

    char *resp_out;
    size_t resp_out_len;
    size_t resp_out_sent;

    char *resp_hdr;
    size_t resp_hdr_len;
    size_t resp_hdr_cap;
    upstream_response_t resp;

    body_mode_t req_body;
    size_t req_remaining;
    chunk_parser_t req_chunk;
    int req_body_done;

    body_mode_t resp_body;
    size_t resp_remaining;
    chunk_parser_t resp_chunk;
    int resp_body_done;

    char c2u[PROXY_BUF_SIZE];
    size_t c2u_len;
    size_t c2u_off;

    char u2c[PROXY_BUF_SIZE];
    size_t u2c_len;
    size_t u2c_off;

    int head_only;
    int client_keep_alive;
    int close_client;
    int response_started;
    int upstream_keep_alive;
    int upstream_eof;
    int continue_pending;
    size_t continue_sent;
    int dead;
    int status;
    size_t body_bytes;
    uint32_t tried_mask;

    uint64_t deadline_ms;
    struct proxy_conn *next;
    struct proxy_conn *next_dead;
};

#endif
