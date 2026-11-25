#include "app.h"
#include "cgi.h"
#include "session_app.h"
#include <string.h>

static bool prepare(const void *config, const struct http_request *request,
                    char *location, struct http_response *response)
{
    const struct web_config *web = config;
    if (cgi_route(request))
    {
        if (web->cgi)
            return false;
        const char *reason = http_reason_phrase(404);
        *response = (struct http_response){.status = 404, .file_fd = -1,
            .content_type = "text/plain; charset=utf-8", .body = {reason, strlen(reason)}};
        return true;
    }
    struct router_result route = app_match_request(request);
    if (route.handler)
        return false;
    *response = app_handle_request(web->root_fd, request, &route, location);
    return true;
}

static struct session_stream *finish(const void *config,
                                     const struct http_request *request,
                                     bool head, char *location,
                                     struct http_response *response)
{
    (void)head;
    const struct web_config *web = config;
    if (web->cgi && cgi_route(request))
    {
        int error;
        struct session_stream *stream = cgi_submit(web->cgi, request, &error);
        if (stream)
            return stream;
        const char *reason = http_reason_phrase(error);
        *response = (struct http_response){.status = error, .file_fd = -1,
            .content_type = "text/plain; charset=utf-8", .body = {reason, strlen(reason)}};
        return NULL;
    }
    struct router_result route = app_match_request(request);
    *response = app_handle_request(web->root_fd, request, &route, location);
    return NULL;
}

struct session_app web_app(const struct web_config *config)
{
    return (struct session_app){.config = config, .prepare = prepare, .finish = finish,
                                .stream_ops = &cgi_ops};
}
