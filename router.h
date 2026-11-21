#ifndef ROUTER_H
#define ROUTER_H

#include "http.h"

enum router_handler
{
    ROUTER_ERROR,
    ROUTER_INDEX,
    ROUTER_ECHO
};

struct router_result
{
    enum router_handler handler;
    int status;
    const char *allow;
};

/* Requires validated head. Matches decoded path once; ignores query. No I/O. */
struct router_result router_match_request(const struct http_request *request);

#endif
