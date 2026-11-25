#ifndef PROXY_WIRE_H
#define PROXY_WIRE_H

#include "proxy_conn.h"

void proxy_build_request(conn_t *c, const http_request_t *req,
                         proxy_conn_t *p);
void proxy_build_response(proxy_conn_t *p);

#endif
