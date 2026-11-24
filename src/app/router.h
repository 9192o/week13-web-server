#ifndef ROUTER_H
#define ROUTER_H

#include "http.h"

typedef struct http_response (*route_handler)(const struct http_request *request);

struct route
{
    const char *method;
    const char *path;
    route_handler handler;
};

struct router_result
{
    route_handler handler; /* NULL for static candidates and routing errors. */
    int status;
    const char *allow;
    char path[HTTP_REQUEST_LINE_MAX];
};

/* Requires validated head and one non-null handler per distinct dynamic path. No I/O. */
struct router_result router_match_request(const struct http_request *request,
                                          const struct route *routes, size_t route_count);

#endif
