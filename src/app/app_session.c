#include "app.h"
#include "session_app.h"

static bool prepare(const void *config, const struct http_request *request,
                    char *location, struct http_response *response)
{
    struct router_result route = app_match_request(request);
    if (route.handler)
        return false;
    *response = app_handle_request(*(const int *)config, request, &route, location);
    return true;
}

static struct session_stream *finish(const void *config,
                                     const struct http_request *request,
                                     bool head, char *location,
                                     struct http_response *response)
{
    (void)head;
    struct router_result route = app_match_request(request);
    *response = app_handle_request(*(const int *)config, request, &route, location);
    return NULL;
}

struct session_app web_app(const int *root_fd)
{
    return (struct session_app){.config = root_fd, .prepare = prepare, .finish = finish};
}
