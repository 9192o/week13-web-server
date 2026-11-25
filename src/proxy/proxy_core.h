#ifndef PROXY_CORE_H
#define PROXY_CORE_H

#include "config.h"
#include "proxy_conn.h"
#include "proxy_internal.h"

#include <stdint.h>

#define PROXY_HEALTH_COOLDOWN_MS 1000

/* Route/backend table (proxy_route.c). */
int proxy_route_init(const config_t *cfg);
void proxy_route_cleanup(void);
const config_t *proxy_config(void);
int proxy_route_count(void);
proxy_route_t *proxy_route_at(int index);
proxy_backend_t *proxy_select_backend(proxy_route_t *r, uint32_t skip_mask,
                                       int *out_index);

/* Connection lifecycle (proxy_connection.c), used by the relay code. */
void proxy_update_events(proxy_conn_t *p);
void proxy_close_upstream(proxy_conn_t *p);
void proxy_fail(proxy_conn_t *p, int status);
void proxy_abort(proxy_conn_t *p);
void proxy_finish(proxy_conn_t *p);

/* Request/response body relay (proxy_relay.c). */
void proxy_flush_upstream(proxy_conn_t *p);
void proxy_read_client_body(proxy_conn_t *p);
void proxy_read_upstream_headers(proxy_conn_t *p);
void proxy_read_upstream_body(proxy_conn_t *p);
void proxy_drain_eof(proxy_conn_t *p);
void proxy_flush_continue(proxy_conn_t *p);
void proxy_flush_client(proxy_conn_t *p);

#endif
