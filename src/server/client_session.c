#define _POSIX_C_SOURCE 200809L

#include "client_session.h"
#include "connection.h"
#include "http.h"
#include "session_app.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/*
 * Per-call I/O budgets keep one session from starving the scheduler.
 * CS_RECV_BUDGET limits bytes read in one advance() call.
 * CS_SEND_BUDGET limits bytes written in one advance() call.
 * CS_FILE_BUFSZ is the transfer buffer between file and socket.
 */
#define CS_RECV_BUDGET 8192
#define CS_SEND_BUDGET 65536
#define CS_FILE_BUFSZ  8192
#define CS_KEEPALIVE_MS 5000
#define CS_MAX_REQUESTS 100

enum cs_state {
    CS_IDLE,      /* successful previous request, waiting for a new one */
    CS_READ_HEAD, /* accumulating request line + headers in head_buf */
    CS_READ_BODY, /* decoding request body */
    CS_STREAM,    /* driving an application response stream */
    CS_WRITING,   /* sending final response head/body */
    CS_CLOSED     /* fd closed, awaiting destroy */
};

enum cs_status {
    CS_STATUS_OK,      /* progress made; caller may loop again */
    CS_STATUS_BLOCK,   /* waiting for readability/writability */
    CS_STATUS_DONE,    /* current stage finished (e.g. response sent) */
    CS_STATUS_ERROR    /* unrecoverable I/O error; fd already closed */
};

struct client_session {
    int fd;
    const struct session_app *app;
    int log_fd;
    enum cs_state state;
    int logged;
    bool log_request;
    bool wants_close;
    bool body_complete;
    unsigned int request_count;
    char pending[CS_RECV_BUDGET];
    size_t pending_len;

    struct timespec accepted;
    struct timespec total_deadline;
    struct timespec header_deadline;
    struct timespec body_deadline;
    struct timespec send_deadline;
    struct timespec stage_deadline;
    struct timespec shutdown_deadline;
    int have_shutdown_deadline;

    conn_access_record_t access;

    /*
     * Borrowed-slice backing store.  request.line, headers, target, and the
     * final path log all borrow from head_buf.  head_buf must outlive the
     * request and the response preparation that uses the request.
     */
    char head_buf[HTTP_REQUEST_HEAD_MAX];
    size_t head_len;
    size_t body_feed_offset; /* first body byte still sitting in head_buf */

    struct http_request_line_result line;
    struct http_headers_result headers;
    struct http_target target;
    struct http_body_parser body_parser;
    int body_initialized;

    struct http_request request;
    struct session_stream *stream;
    char location[HTTP_REQUEST_LINE_MAX + 2];

    char method_log[HTTP_REQUEST_LINE_MAX];
    char path_log[HTTP_REQUEST_LINE_MAX];

    int head;
    int validated;

    /*
     * Final response queued for transmission.  When file_fd >= 0, ownership
     * of that descriptor transfers into this session and is closed on
     * replacement or destroy.
     */
    struct http_response final_response;
    int response_queued;

    char send_header[HTTP_REQUEST_LINE_MAX + 512];
    size_t send_header_len;
    size_t send_header_pos;

    size_t body_pos;      /* bytes of non-file response body already sent */
    size_t file_remaining; /* bytes of file body still to read */
    char send_body_buf[CS_FILE_BUFSZ];
    size_t send_body_buf_len;
    size_t send_body_buf_pos;

    int sent_continue;
    int need_continue;
    size_t continue_pos;

};

static int cs_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (flags & O_NONBLOCK)
        return 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int cs_timespec_gt(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec)
        return a->tv_sec > b->tv_sec;
    return a->tv_nsec > b->tv_nsec;
}

/* Apply the persistent shutdown deadline if one has been set. */
static void cs_cap_at_shutdown(struct client_session *c, struct timespec *deadline)
{
    if (c->have_shutdown_deadline && cs_timespec_gt(deadline, &c->shutdown_deadline))
        *deadline = c->shutdown_deadline;
}

static void cs_close_client(struct client_session *c)
{
    if (c->fd >= 0)
    {
        close(c->fd);
        c->fd = -1;
    }
    c->state = CS_CLOSED;
}

/* Non-blocking recv wrapper.  Retries EINTR; EAGAIN is a normal block. */
static conn_result_t cs_recv_raw(int fd, void *buf, size_t max_len, size_t *out_read)
{
    *out_read = 0;
    for (;;)
    {
        ssize_t r = recv(fd, buf, max_len, 0);
        if (r > 0)
        {
            *out_read = (size_t)r;
            return CONN_RESULT_OK;
        }
        if (r == 0)
            return CONN_RESULT_EOF;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return CONN_RESULT_AGAIN;
        if (errno == ECONNRESET || errno == EPROTO)
            return CONN_RESULT_RESET;
        return CONN_RESULT_ERROR;
    }
}

/* Non-blocking send wrapper with MSG_NOSIGNAL.  Retries EINTR. */
static conn_result_t cs_send_raw(int fd, const void *buf, size_t len, size_t *out_sent)
{
    *out_sent = 0;
    for (;;)
    {
        ssize_t s = send(fd, buf, len, MSG_NOSIGNAL);
        if (s > 0)
        {
            *out_sent = (size_t)s;
            return CONN_RESULT_OK;
        }
        if (s == 0)
            return CONN_RESULT_ERROR;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return CONN_RESULT_AGAIN;
        if (errno == EPIPE || errno == ECONNRESET)
            return CONN_RESULT_RESET;
        return CONN_RESULT_ERROR;
    }
}

static void cs_log_access(struct client_session *c)
{
    if (!c->log_request || c->log_fd < 0 || c->logged)
        return;
    c->access.duration_ms = connection_stopwatch_elapsed_ms(&c->accepted);
    (void)connection_log_access(c->log_fd, &c->access);
    c->logged = 1;
}

static void cs_release_response(struct client_session *c)
{
    if (c->response_queued && c->final_response.file_fd >= 0)
        close(c->final_response.file_fd);
    c->response_queued = 0;
}

static bool cs_reset_request(struct client_session *c, bool idle)
{
    cs_release_response(c);
    if (c->body_initialized)
        http_body_destroy(&c->body_parser);
    c->body_initialized = 0;
    c->head_len = c->pending_len;
    memcpy(c->head_buf, c->pending, c->pending_len);
    c->pending_len = 0;
    c->line = (struct http_request_line_result){0};
    c->headers = (struct http_headers_result){0};
    c->body_feed_offset = 0;
    c->head = c->validated = c->logged = 0;
    c->access = (conn_access_record_t){.status = -1};
    c->sent_continue = c->need_continue = 0;
    c->continue_pos = 0;
    c->wants_close = c->body_complete = false;
    c->log_request = !idle;
    c->state = idle ? CS_IDLE : CS_READ_HEAD;
    if (idle)
        return connection_deadline_from_now(CS_KEEPALIVE_MS, NULL,
                                            &c->stage_deadline) == 0;
    if (connection_deadline_now(&c->accepted) < 0)
        return false;
    connection_deadline_add(&c->accepted, PHASE1_TOTAL_MS, NULL, &c->total_deadline);
    connection_deadline_add(&c->accepted, PHASE1_HEADER_MS, &c->total_deadline,
                            &c->stage_deadline);
    return true;
}

/* Store a final response without changing state (used while still reading body). */
static void cs_set_pending_response(struct client_session *c,
                                    const struct http_response *response)
{
    if (response == &c->final_response)
        return;
    if (c->response_queued && c->final_response.file_fd >= 0)
    {
        close(c->final_response.file_fd);
        c->final_response.file_fd = -1;
    }
    c->final_response = *response;
    c->response_queued = 1;
    c->access.status = response->status;
}

static void queue_final_response(struct client_session *c,
                                 const struct http_response *response)
{
    cs_set_pending_response(c, response);

    c->final_response.keep_alive = c->body_complete && !c->wants_close &&
        !c->have_shutdown_deadline && c->request_count < CS_MAX_REQUESTS;
    c->send_header_len = 0;
    c->send_header_pos = 0;
    c->body_pos = 0;
    c->send_body_buf_len = 0;
    c->send_body_buf_pos = 0;

    if (c->final_response.file_fd >= 0)
        c->file_remaining = c->final_response.body.length;
    else
        c->file_remaining = 0;

    if (connection_deadline_from_now(PHASE1_SEND_MS, NULL, &c->send_deadline) < 0)
    {
        cs_close_client(c);
        return;
    }
    cs_cap_at_shutdown(c, &c->send_deadline);
    c->stage_deadline = c->send_deadline;
    c->state = CS_WRITING;
}

static void cs_queue_error_response(struct client_session *c, int status)
{
    c->wants_close = true;
    const char *reason = http_reason_phrase(status);
    struct http_response error_response = {
        .status = status,
        .content_type = "text/plain; charset=utf-8",
        .body = {reason, strlen(reason)},
        .file_fd = -1,
    };
    queue_final_response(c, &error_response);
}

static bool cs_advance_stream(struct client_session *c, bool upstream_readable,
                              bool upstream_writable, bool downstream_writable)
{
    const struct session_stream_ops *ops = c->app->stream_ops;
    if (ops->response)
    {
        struct http_response response;
        if (upstream_readable && ops->response(c->stream, &response,
                                                c->location, sizeof(c->location)))
        {
            ops->destroy(c->stream);
            c->stream = NULL;
            queue_final_response(c, &response);
        }
        return true;
    }
    size_t sent = 0;
    bool allow_keep = !c->wants_close && !c->have_shutdown_deadline &&
                      c->request_count < CS_MAX_REQUESTS;
    int result = ops->pump(c->stream, upstream_readable, upstream_writable,
                           c->fd, downstream_writable, allow_keep, &sent);
    c->access.bytes_sent += sent;
    if (ops->started(c->stream))
        c->access.status = ops->status(c->stream);
    if (!result)
        return true;
    bool started = ops->started(c->stream);
    bool keep = result == 1 && allow_keep && ops->keep_client(c->stream);
    ops->destroy(c->stream);
    c->stream = NULL;
    if (result >= 400 && !started)
    {
        cs_queue_error_response(c, result);
        return c->state != CS_CLOSED;
    }
    if (result == 1)
    {
        size_t discarded;
        c->access.complete = 1;
        if (keep)
        {
            cs_log_access(c);
            if (cs_reset_request(c, c->pending_len == 0))
                return c->state == CS_IDLE || client_session_advance(c, false, false);
            cs_close_client(c);
            return false;
        }
        shutdown(c->fd, SHUT_WR);
        connection_drain(c->fd, HTTP_REQUEST_HEAD_MAX, &discarded);
    }
    cs_close_client(c);
    return false;
}

static void cs_handle_timeout(struct client_session *c)
{
    if (c->have_shutdown_deadline)
    {
        cs_close_client(c);
        return;
    }
    switch (c->state)
    {
    case CS_IDLE:
        cs_close_client(c);
        break;
    case CS_READ_HEAD:
    case CS_READ_BODY:
        cs_queue_error_response(c, 408);
        break;
    case CS_WRITING:
        cs_close_client(c);
        break;
    case CS_STREAM:
    {
        const struct session_stream_ops *ops = c->app->stream_ops;
        bool started = ops->started && ops->started(c->stream);
        c->app->stream_ops->destroy(c->stream);
        c->stream = NULL;
        if (started)
            cs_close_client(c);
        else
            cs_queue_error_response(c, 504);
        break;
    }
    default:
        break;
    }
}

/* Build response head once; http_format_response_head borrows the response. */
static enum cs_status cs_build_header(struct client_session *c)
{
    if (c->send_header_len != 0)
        return CS_STATUS_OK;
    c->send_header_len = http_format_response_head(&c->final_response,
                                                   c->send_header,
                                                   sizeof(c->send_header));
    if (c->send_header_len == 0)
    {
        cs_close_client(c);
        return CS_STATUS_ERROR;
    }
    return CS_STATUS_OK;
}

/* Send the final response (header + body/file).  Updates access.bytes_sent. */
static enum cs_status cs_write_response(struct client_session *c)
{
    if (cs_build_header(c) != CS_STATUS_OK)
        return CS_STATUS_ERROR;

    size_t budget = CS_SEND_BUDGET;

    while (c->send_header_pos < c->send_header_len && budget > 0)
    {
        size_t n = 0;
        conn_result_t r = cs_send_raw(c->fd,
                                      c->send_header + c->send_header_pos,
                                      c->send_header_len - c->send_header_pos,
                                      &n);
        if (r == CONN_RESULT_AGAIN)
            return CS_STATUS_BLOCK;
        if (r != CONN_RESULT_OK)
        {
            cs_close_client(c);
            return CS_STATUS_ERROR;
        }
        c->send_header_pos += n;
        c->access.bytes_sent += n;
        budget -= n;
    }
    if (c->send_header_pos < c->send_header_len)
        return CS_STATUS_BLOCK;

    if (c->head)
        return CS_STATUS_DONE;

    if (c->final_response.file_fd < 0)
    {
        size_t total = c->final_response.body.length;
        while (c->body_pos < total && budget > 0)
        {
            size_t n = 0;
            size_t remaining = total - c->body_pos;
            conn_result_t r = cs_send_raw(c->fd,
                                          c->final_response.body.data + c->body_pos,
                                          remaining < budget ? remaining : budget,
                                          &n);
            if (r == CONN_RESULT_AGAIN)
                return CS_STATUS_BLOCK;
            if (r != CONN_RESULT_OK)
            {
                cs_close_client(c);
                return CS_STATUS_ERROR;
            }
            c->body_pos += n;
            c->access.bytes_sent += n;
            budget -= n;
        }
        if (c->body_pos == total)
            return CS_STATUS_DONE;
        return CS_STATUS_BLOCK;
    }

    /* File body: read buffered chunks and send them, handling partial writes. */
    while (budget > 0)
    {
        if (c->send_body_buf_pos < c->send_body_buf_len)
        {
            size_t n = 0;
            size_t remaining = c->send_body_buf_len - c->send_body_buf_pos;
            conn_result_t r = cs_send_raw(c->fd,
                                          c->send_body_buf + c->send_body_buf_pos,
                                          remaining < budget ? remaining : budget,
                                          &n);
            if (r == CONN_RESULT_AGAIN)
                return CS_STATUS_BLOCK;
            if (r != CONN_RESULT_OK)
            {
                cs_close_client(c);
                return CS_STATUS_ERROR;
            }
            c->send_body_buf_pos += n;
            c->access.bytes_sent += n;
            budget -= n;
            continue;
        }
        if (c->file_remaining == 0)
            return CS_STATUS_DONE;

        size_t want = c->file_remaining < sizeof(c->send_body_buf)
                          ? c->file_remaining
                          : sizeof(c->send_body_buf);
        ssize_t got = read(c->final_response.file_fd, c->send_body_buf, want);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
        {
            /* File changed size or read failed mid-response. */
            c->access.complete = 0;
            cs_close_client(c);
            return CS_STATUS_ERROR;
        }
        c->send_body_buf_len = (size_t)got;
        c->send_body_buf_pos = 0;
        c->file_remaining -= (size_t)got;
    }
    return CS_STATUS_BLOCK;
}

/* Body parser completed; hand off to the app (dynamic) or start writing (static). */
static void cs_finalize_body(struct client_session *c)
{
    c->request.body = (struct http_slice){c->body_parser.data, c->body_parser.length};
    if (!c->response_queued)
    {
        struct http_response response = {.file_fd = -1};
        c->stream = c->app->finish(c->app->config, &c->request, c->head,
                                   c->location, &response);
        if (!c->stream)
        {
            queue_final_response(c, &response);
            return;
        }
        if (c->have_shutdown_deadline)
            c->app->stream_ops->cap_deadline(c->stream, &c->shutdown_deadline);
        c->state = CS_STREAM;
    }
    else
    {
        /* Static 200 response was queued before the body; start writing now. */
        queue_final_response(c, &c->final_response);
    }
}

/* Feed one contiguous buffer to the body parser and handle the result. */
static enum cs_status cs_feed_body_chunk(struct client_session *c,
                                         const char *data, size_t len)
{
    struct http_body_result result = http_parse_body(&c->body_parser, data, len);
    if (result.status == HTTP_PARSE_ERROR)
    {
        cs_queue_error_response(c, result.error_status);
        return CS_STATUS_OK; /* transitioned to writing */
    }
    if (result.status == HTTP_PARSE_COMPLETE)
    {
        size_t trailing = len - result.consumed;
        if (trailing > sizeof(c->pending))
        {
            cs_close_client(c);
            return CS_STATUS_ERROR;
        }
        memcpy(c->pending, data + result.consumed, trailing);
        c->pending_len = trailing;
        c->body_complete = true;
        cs_finalize_body(c);
        return CS_STATUS_DONE;
    }
    return CS_STATUS_OK;
}

/* Feed body bytes that arrived inside the head read before the body state began. */
/* Send 100 Continue when the client is waiting for it. */
static enum cs_status cs_send_continue(struct client_session *c)
{
    if (c->sent_continue)
    {
        c->need_continue = 0;
        c->continue_pos = 0;
        return CS_STATUS_OK;
    }
    const char *msg = HTTP_CONTINUE_RESPONSE;
    size_t total = strlen(msg);
    while (c->continue_pos < total)
    {
        size_t n = 0;
        conn_result_t r = cs_send_raw(c->fd, msg + c->continue_pos,
                                      total - c->continue_pos, &n);
        if (r == CONN_RESULT_AGAIN)
            return CS_STATUS_BLOCK;
        if (r != CONN_RESULT_OK)
        {
            cs_close_client(c);
            return CS_STATUS_ERROR;
        }
        c->continue_pos += n;
    }
    c->sent_continue = 1;
    c->need_continue = 0;
    c->continue_pos = 0;
    return CS_STATUS_OK;
}

/* Read body bytes from the socket and feed the parser, batched per advance(). */
static enum cs_status cs_read_body(struct client_session *c, bool readable)
{
    if (!readable)
        return CS_STATUS_BLOCK;

    char buf[CS_RECV_BUDGET];
    size_t budget = CS_RECV_BUDGET;

    while (budget > 0 && c->state == CS_READ_BODY)
    {
        size_t want = budget < sizeof(buf) ? budget : sizeof(buf);
        size_t n = 0;
        conn_result_t r = cs_recv_raw(c->fd, buf, want, &n);
        if (r == CONN_RESULT_AGAIN)
            return CS_STATUS_BLOCK;
        if (r == CONN_RESULT_EOF)
        {
            cs_queue_error_response(c, 400);
            return CS_STATUS_OK;
        }
        if (r != CONN_RESULT_OK)
        {
            cs_close_client(c);
            return CS_STATUS_ERROR;
        }
        if (n == 0)
            continue;
        if (connection_deadline_from_now(PHASE1_IDLE_MS, &c->total_deadline,
                                         &c->body_deadline) < 0)
        {
            cs_close_client(c);
            return CS_STATUS_ERROR;
        }
        c->stage_deadline = c->body_deadline;
        budget -= n;
        enum cs_status st = cs_feed_body_chunk(c, buf, n);
        if (st == CS_STATUS_DONE || st == CS_STATUS_ERROR)
            return st;
        /* NEED_MORE: keep reading until budget is consumed. */
    }
    return CS_STATUS_BLOCK;
}

/* Parse the accumulated head buffer and advance to body, error, or writing. */
static enum cs_status cs_process_head(struct client_session *c)
{
    if (c->line.status == HTTP_PARSE_NEED_MORE)
    {
        c->line = http_parse_request_line(c->head_buf, c->head_len);
        if (c->line.status == HTTP_PARSE_NEED_MORE)
        {
            if (c->head_len >= HTTP_REQUEST_HEAD_MAX)
            {
                cs_queue_error_response(c, 414);
                return CS_STATUS_OK;
            }
            return CS_STATUS_BLOCK;
        }
        if (c->line.status == HTTP_PARSE_ERROR)
        {
            cs_queue_error_response(c, c->line.error_status);
            return CS_STATUS_OK;
        }

        /* HEAD is remembered so error/success responses suppress the body. */
        c->head = c->line.request.method.length == 4 &&
                  memcmp(c->line.request.method.data, "HEAD", 4) == 0;
        if (c->line.request.method.length < sizeof(c->method_log))
        {
            memcpy(c->method_log, c->line.request.method.data,
                   c->line.request.method.length);
            c->method_log[c->line.request.method.length] = '\0';
            c->access.method = c->method_log;
        }
    }

    if (c->headers.status == HTTP_PARSE_NEED_MORE)
    {
        c->headers = http_parse_headers(c->head_buf + c->line.consumed,
                                        c->head_len - c->line.consumed,
                                        c->line.consumed);
        if (c->headers.status == HTTP_PARSE_NEED_MORE)
        {
            if (c->head_len >= HTTP_REQUEST_HEAD_MAX)
            {
                cs_queue_error_response(c, 431);
                return CS_STATUS_OK;
            }
            return CS_STATUS_BLOCK;
        }
        if (c->headers.status == HTTP_PARSE_ERROR)
        {
            cs_queue_error_response(c, c->headers.error_status);
            return CS_STATUS_OK;
        }
    }

    int err = http_validate_request_head(&c->line.request, &c->headers.headers,
                                         &c->target);
    if (err)
    {
        cs_queue_error_response(c, err);
        return CS_STATUS_OK;
    }

    if (c->target.path.length && c->target.path.length < sizeof(c->path_log))
    {
        memcpy(c->path_log, c->target.path.data, c->target.path.length);
        c->path_log[c->target.path.length] = '\0';
        c->access.path = c->path_log;
    }

    c->request = (struct http_request){
        .line = c->line.request,
        .headers = c->headers.headers,
        .target = c->target,
    };
    err = http_body_init(&c->body_parser, &c->headers.headers);
    c->body_initialized = 1;
    if (err)
    {
        cs_queue_error_response(c, err);
        return CS_STATUS_OK;
    }
    c->validated = 1;
    ++c->request_count;
    c->wants_close = http_connection_wants_close(&c->headers.headers);

    if (connection_deadline_from_now(PHASE1_IDLE_MS, &c->total_deadline,
                                     &c->body_deadline) < 0)
    {
        cs_close_client(c);
        return CS_STATUS_ERROR;
    }
    cs_cap_at_shutdown(c, &c->body_deadline);
    c->stage_deadline = c->body_deadline;
    c->body_feed_offset = c->line.consumed + c->headers.consumed;

    struct http_response app_response = {.file_fd = -1};
    if (c->app->prepare(c->app->config, &c->request, c->location, &app_response))
    {
        if (app_response.status != 200)
        {
            /* An early route error may reuse only a request known to have no body. */
            if (c->body_parser.state == HTTP_BODY_DONE)
            {
                size_t trailing = c->head_len - c->body_feed_offset;
                if (trailing > sizeof(c->pending))
                {
                    if (app_response.file_fd >= 0)
                        close(app_response.file_fd);
                    cs_close_client(c);
                    return CS_STATUS_ERROR;
                }
                memcpy(c->pending, c->head_buf + c->body_feed_offset, trailing);
                c->pending_len = trailing;
                c->body_complete = true;
            }
            queue_final_response(c, &app_response);
            return CS_STATUS_OK;
        }
        cs_set_pending_response(c, &app_response);
    }

    c->state = CS_READ_BODY;
    enum cs_status body_status = cs_feed_body_chunk(c, c->head_buf + c->body_feed_offset,
                                                    c->head_len - c->body_feed_offset);
    if (body_status == CS_STATUS_OK && c->state == CS_READ_BODY &&
        c->body_parser.expect_continue && !c->sent_continue)
        c->need_continue = 1;
    return CS_STATUS_OK;
}

/* Read more head bytes, bounded by the head-buffer capacity. */
static enum cs_status cs_read_head(struct client_session *c, bool readable)
{
    if (!readable)
        return CS_STATUS_BLOCK;
    if (c->head_len >= HTTP_REQUEST_HEAD_MAX)
        return CS_STATUS_OK; /* let cs_process_head report the limit error */

    size_t space = HTTP_REQUEST_HEAD_MAX - c->head_len;
    size_t want = space < CS_RECV_BUDGET ? space : CS_RECV_BUDGET;
    size_t n = 0;
    conn_result_t r = cs_recv_raw(c->fd, c->head_buf + c->head_len, want, &n);
    if (r == CONN_RESULT_AGAIN)
        return CS_STATUS_BLOCK;
    if (r == CONN_RESULT_EOF)
    {
        if (c->state == CS_IDLE)
        {
            cs_close_client(c);
            return CS_STATUS_ERROR;
        }
        cs_queue_error_response(c, 400);
        return CS_STATUS_OK;
    }
    if (r != CONN_RESULT_OK)
    {
        cs_close_client(c);
        return CS_STATUS_ERROR;
    }
    c->head_len += n;
    if (c->state == CS_IDLE)
    {
        c->state = CS_READ_HEAD;
        c->log_request = true;
        if (connection_deadline_now(&c->accepted) < 0)
        {
            cs_close_client(c);
            return CS_STATUS_ERROR;
        }
        connection_deadline_add(&c->accepted, PHASE1_TOTAL_MS, NULL, &c->total_deadline);
        connection_deadline_add(&c->accepted, PHASE1_HEADER_MS, &c->total_deadline,
                                &c->stage_deadline);
    }
    return CS_STATUS_OK;
}

struct client_session *client_session_create(int fd, int log_fd,
                                             const struct session_app *app)
{
    struct client_session *c = calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;

    c->fd = fd;
    c->app = app;
    c->log_fd = log_fd;
    c->state = CS_READ_HEAD;
    c->access.status = -1;
    c->log_request = true;
    c->final_response.file_fd = -1;

    if (connection_deadline_now(&c->accepted) < 0)
    {
        free(c);
        return NULL;
    }

    connection_deadline_add(&c->accepted, PHASE1_TOTAL_MS, NULL,
                            &c->total_deadline);
    connection_deadline_add(&c->accepted, PHASE1_HEADER_MS,
                            &c->total_deadline, &c->header_deadline);
    c->stage_deadline = c->header_deadline;

    if (cs_set_nonblocking(fd) < 0)
    {
        free(c);
        return NULL;
    }

    return c;
}

void client_session_destroy(struct client_session *client)
{
    if (client == NULL)
        return;

    cs_release_response(client);
    if (client->stream)
        client->app->stream_ops->destroy(client->stream);
    if (client->body_initialized)
        http_body_destroy(&client->body_parser);
    cs_close_client(client);
    cs_log_access(client);

    free(client);
}

short client_session_interest(const struct client_session *client)
{
    switch (client->state)
    {
    case CS_IDLE:
    case CS_READ_HEAD:
        return POLLIN;
    case CS_READ_BODY:
        return client->need_continue ? POLLOUT : POLLIN;
    case CS_WRITING:
        return POLLOUT;
    case CS_STREAM:
        return client->app->stream_ops->has_output &&
               client->app->stream_ops->has_output(client->stream) ? POLLOUT : 0;
    default:
        return 0;
    }
}

int64_t client_session_remaining_ms(const struct client_session *client)
{
    if (client->state == CS_CLOSED)
        return -1;
    if (client->state == CS_STREAM)
        return client->app->stream_ops->remaining_ms(client->stream);
    return (int64_t)connection_deadline_remaining_ms(&client->stage_deadline);
}

bool client_session_begin_shutdown(struct client_session *client,
                                   const struct timespec *shutdown_deadline)
{
    if (!client->validated)
    {
        client->log_request = false;
        return false;
    }

    client->have_shutdown_deadline = 1;
    client->shutdown_deadline = *shutdown_deadline;
    if (client->send_header_pos == 0)
    {
        client->final_response.keep_alive = 0;
        client->send_header_len = 0;
    }

    cs_cap_at_shutdown(client, &client->total_deadline);
    cs_cap_at_shutdown(client, &client->header_deadline);
    cs_cap_at_shutdown(client, &client->body_deadline);
    cs_cap_at_shutdown(client, &client->send_deadline);
    cs_cap_at_shutdown(client, &client->stage_deadline);
    if (client->stream)
        client->app->stream_ops->cap_deadline(client->stream, shutdown_deadline);

    return true;
}

bool client_session_advance(struct client_session *client, bool readable,
                            bool writable)
{
    if (client->state == CS_CLOSED)
        return false;

    /*
     * Loop until the current state blocks, finishes a stage, or an error
     * closes the socket.  The guard prevents accidental busy loops if a
     * transition chain unexpectedly cycles.
     */
    for (int loop = 0; loop < 64; ++loop)
    {
        if (client->state == CS_CLOSED)
            return false;

        if (client_session_remaining_ms(client) == 0)
        {
            cs_handle_timeout(client);
            continue;
        }

        switch (client->state)
        {
        case CS_STREAM:
            return cs_advance_stream(client, false, false, writable);
        case CS_IDLE:
        case CS_READ_HEAD:
        {
            enum cs_status st = client->head_len > 0
                ? cs_process_head(client) : CS_STATUS_BLOCK;
            if (st == CS_STATUS_OK)
                continue;
            if (st == CS_STATUS_ERROR)
                return false;
            st = cs_read_head(client, readable);
            if (st == CS_STATUS_BLOCK)
                return true;
            if (st == CS_STATUS_ERROR)
                return false;

            /* We consumed the readable event; further progress uses buffered bytes. */
            readable = false;
            st = cs_process_head(client);
            if (st == CS_STATUS_ERROR)
                return false;
            if (st == CS_STATUS_BLOCK)
                return true;
            continue;
        }

        case CS_READ_BODY:
        {
            if (client->need_continue)
            {
                enum cs_status st = cs_send_continue(client);
                if (st == CS_STATUS_BLOCK)
                    return true;
                if (st == CS_STATUS_ERROR)
                    return false;
                continue;
            }

            enum cs_status st = cs_read_body(client, readable);
            if (st == CS_STATUS_ERROR)
                return false;
            if (st == CS_STATUS_DONE || client->state != CS_READ_BODY)
                continue;
            return true;
        }

        case CS_WRITING:
        {
            if (!writable)
                return true;
            enum cs_status st = cs_write_response(client);
            if (st == CS_STATUS_ERROR)
                return false;
            if (st == CS_STATUS_DONE)
            {
                client->access.complete = 1;
                if (client->final_response.keep_alive && !client->have_shutdown_deadline)
                {
                    cs_log_access(client);
                    if (!cs_reset_request(client, client->pending_len == 0))
                    {
                        cs_close_client(client);
                        return false;
                    }
                    if (client->state == CS_IDLE)
                        return true;
                    /* Buffered request bytes have no independent epoll event. */
                    writable = false;
                    continue;
                }
                size_t discarded;
                shutdown(client->fd, SHUT_WR);
                connection_drain(client->fd, HTTP_REQUEST_HEAD_MAX, &discarded);
                cs_close_client(client);
                return false;
            }
            return true;
        }

        default:
            return false;
        }
    }

    return true;
}

int client_session_upstream_fd(const struct client_session *client)
{
    return client->stream ? client->app->stream_ops->fd(client->stream) : -1;
}

short client_session_upstream_interest(const struct client_session *client)
{
    return client->stream ? client->app->stream_ops->interest(client->stream) : 0;
}

bool client_session_advance_upstream(struct client_session *client, bool readable,
                                     bool writable)
{
    if (client->state != CS_STREAM)
        return true;
    if (client_session_remaining_ms(client) == 0)
        return client_session_advance(client, false, false);
    return cs_advance_stream(client, readable, writable, false);
}
