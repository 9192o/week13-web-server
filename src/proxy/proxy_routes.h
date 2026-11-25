#ifndef PROXY_ROUTES_H
#define PROXY_ROUTES_H

#include "http.h"
#include <netinet/in.h>
#include <time.h>

#define PROXY_ROUTE_MAX 32

struct proxy_route {
    const char *prefix;
    size_t length;
    struct sockaddr_in address;
    const char *host;
    size_t next_peer;
    size_t cursor; /* Used only by the first entry for a prefix. */
    int idle; /* Encoded fd + 1, or zero. */
    struct timespec idle_deadline;
};

struct proxy_routes {
    size_t count;
    struct proxy_route entries[PROXY_ROUTE_MAX];
};

int proxy_route_add(struct proxy_routes *routes, const char *arg);
const struct proxy_route *proxy_route_match(const struct proxy_routes *routes,
                                            const struct http_target *target);
/* Select one backend and advance only the matched prefix's cursor. */
const struct proxy_route *proxy_route_select(struct proxy_routes *routes,
                                             const struct http_target *target);

#endif
