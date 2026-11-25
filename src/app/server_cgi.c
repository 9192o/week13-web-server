#include "server_internal.h"

#include "cgi.h"
#include "conn_common.h"
#include "http.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint64_t g_task_seq = 0;

static void *cgi_task_fn(void *arg) {
    cgi_job_t *job = arg;
    cgi_result_t *res = cgi_job_execute(job);
    cgi_job_free(job);
    return res;
}

static conn_t *find_by_task(conn_t *head, uint64_t id) {
    for (conn_t *c = head; c != NULL; c = c->next) {
        if (c->task_id == id)
            return c;
    }
    return NULL;
}

int submit_cgi(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
               const config_t *cfg, int close_after) {
    char remote[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &c->peer.sin_addr, remote, sizeof remote) == NULL)
        snprintf(remote, sizeof remote, "?");

    cgi_job_t *job = cgi_job_create(&c->req, cfg, remote, close_after);
    if (job == NULL) {
        if (conn_common_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1,
                                    c->head_only, 0,
                                    cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 0;
    }

    uint64_t id = ++g_task_seq;
    if (task_pool_submit(pool, id, cgi_task_fn, job) < 0) {
        int err = errno;
        cgi_job_free(job);

        int status = err == EAGAIN ? HTTP_SERVICE_UNAVAILABLE
                                   : HTTP_INTERNAL_SERVER_ERROR;
        int retry_after = err == EAGAIN;
        if (conn_common_build_error(c, status, 1, c->head_only, retry_after,
                                    cfg->request_timeout_sec) < 0) {
            http_request_free(&c->req);
            return -1;
        }
        c->requests++;
        http_request_free(&c->req);
        return 0;
    }

    c->task_id = id;
    c->state = ST_WAIT_TASK;
    c->head_only = c->req.method == HTTP_HEAD;
    c->requests++;
    c->deadline_ms = now_ms() +
        (uint64_t)(cfg->request_timeout_sec + 5) * 1000;
    http_request_free(&c->req);

    if (ev_del(loop, c->fd) < 0)
        return -1;
    return 0;
}

void drain_task_completions(ev_loop_t *loop, task_pool_t *pool,
                            conn_t *head, const config_t *cfg) {
    int fd = task_pool_completion_fd(pool);
    char buf[256];
    while (read(fd, buf, sizeof buf) > 0)
        ;

    uint64_t id = 0;
    void *data = NULL;
    while (task_pool_pop_result(pool, &id, &data) == 1) {
        cgi_result_t *res = data;
        conn_t *c = find_by_task(head, id);

        if (c != NULL && !c->closed && c->state == ST_WAIT_TASK) {
            if (res == NULL || res->error || res->response == NULL) {
                if (conn_common_build_error(c, HTTP_INTERNAL_SERVER_ERROR, 1,
                                            c->head_only, 0,
                                            cfg->request_timeout_sec) < 0) {
                    conn_common_close(loop, c);
                } else if (ev_add(loop, c->fd, EV_WRITE, c) < 0) {
                    conn_common_close(loop, c);
                }
            } else {
                free(c->out);
                c->out = res->response;
                res->response = NULL;
                c->out_len = res->response_len;
                c->out_sent = 0;
                c->close_after = res->close_connection ||
                                 conn_common_stopping();
                c->file_fd = -1;
                c->file_remaining = 0;
                c->state = ST_WRITE;
                c->deadline_ms = now_ms() +
                    (uint64_t)cfg->request_timeout_sec * 1000;
                conn_common_log_response(c, res->status, res->body_bytes);
                if (ev_add(loop, c->fd, EV_WRITE, c) < 0)
                    conn_common_close(loop, c);
            }
        }

        if (res != NULL)
            cgi_result_free(res);
    }
}

void free_pending_results(task_pool_t *pool) {
    uint64_t id = 0;
    void *data = NULL;
    while (task_pool_pop_result(pool, &id, &data) == 1)
        cgi_result_free(data);
}
