#ifndef ROUTER_H
#define ROUTER_H

#include "http.h"

enum router_handler
{
    ROUTER_ERROR,
    ROUTER_STATIC,
    ROUTER_ECHO
};

struct router_result
{
    enum router_handler handler;
    int status;
    const char *allow;
    char path[HTTP_REQUEST_LINE_MAX];
};

/* Requires validated head. Decodes and classifies the path; no I/O. */
struct router_result router_match_request(const struct http_request *request);

#endif
