#ifndef CONN_LOOP_H
#define CONN_LOOP_H

#include "config.h"
#include "conn_common.h"
#include "event_backend.h"
#include "server_conn.h"

#include <stdint.h>

typedef struct {
    const char *name;
    const conn_common_hooks_t *hooks;

    /* Create app resources (task pool, proxy init). Return an extra fd to
     * watch, or -1 when there is none. */
    int (*start)(void *ctx, ev_loop_t *loop);
    void (*stop)(void *ctx);

    /* Extra fd event (for example, task-pool completions). */
    void (*extra_event)(void *ctx, ev_loop_t *loop, conn_t *head,
                        uint32_t events);
    void (*tick)(void *ctx);

    int (*readable)(conn_t *c, ev_loop_t *loop, void *ctx,
                    const config_t *cfg);
    int (*writable)(conn_t *c, ev_loop_t *loop, void *ctx,
                    const config_t *cfg);

    void (*client_event)(conn_t *c, uint32_t events);
    void (*upstream_event)(void *source, uint32_t events);
} conn_loop_app_t;

int conn_loop_run(const config_t *cfg, const conn_loop_app_t *app, void *ctx);

#endif
