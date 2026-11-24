#ifndef PROXY_H
#define PROXY_H

#include "config.h"
#include "server_conn.h"

#include <stdint.h>

typedef struct proxy_conn proxy_conn_t;

int proxy_init(const config_t *cfg);
void proxy_cleanup(void);
void proxy_sweep(void);
int proxy_next_timeout_ms(void);

/* Returns 1 when the proxy started, 0 when an error response was queued,
 * -1 on a fatal internal error. */
int proxy_start(conn_t *c, int route_index, const http_request_t *req);
void proxy_kick(conn_t *c);

void proxy_handle_client_event(conn_t *c, uint32_t events);
void proxy_handle_upstream_event(proxy_conn_t *p, uint32_t events);

/* Detach and free the proxy state attached to c. Safe to call repeatedly. */
void proxy_detach(conn_t *c);

/* Close active upstreams and all pooled idle connections. */
void proxy_shutdown_all(void);

/* True when the route at route_index is a proxy route. */
int proxy_route_is_proxy(int route_index);

#endif
