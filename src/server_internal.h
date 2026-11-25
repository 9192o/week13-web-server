#ifndef SERVER_INTERNAL_H
#define SERVER_INTERNAL_H

#include "config.h"
#include "event_backend.h"
#include "server_conn.h"
#include "task_pool.h"

int process_buffer(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
                   const config_t *cfg);
int submit_cgi(conn_t *c, ev_loop_t *loop, task_pool_t *pool,
               const config_t *cfg, int close_after);
void drain_task_completions(ev_loop_t *loop, task_pool_t *pool,
                            conn_t *head, const config_t *cfg);
void free_pending_results(task_pool_t *pool);

#endif
