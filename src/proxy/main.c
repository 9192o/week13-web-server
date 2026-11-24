#include "proxy_exchange.h"
#include "proxy_routes.h"
#include "session_app.h"
#include "webserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct proxy_config
{
    struct proxy_routes *routes;
};
static void error_response(struct http_response *response, int status)
{
    const char *reason = http_reason_phrase(status);
    *response = (struct http_response){
        .status = status,
        .content_type = "text/plain; charset=utf-8",
        .body = {reason, strlen(reason)},
        .file_fd = -1,
    };
}

static bool prepare(const void *config, const struct http_request *request,
                    char *location, struct http_response *response)
{
    (void)location;
    const struct proxy_config *proxy = config;
    if (proxy_route_match(proxy->routes, &request->target))
        return false;
    error_response(response, 404);
    return true;
}

static struct session_stream *finish(const void *config,
                                     const struct http_request *request,
                                     bool head, char *location,
                                     struct http_response *response)
{
    (void)location;
    const struct proxy_config *proxy = config;
    const struct proxy_route *route = proxy_route_select(proxy->routes, &request->target);
    if (!route)
    {
        error_response(response, 404);
        return NULL;
    }
    struct session_stream *stream =
        proxy_exchange_create(&route->address, route->host, request, head);
    if (!stream)
        error_response(response, 502);
    return stream;
}

static const struct session_stream_ops exchange_ops = {
    .destroy = proxy_exchange_destroy,
    .fd = proxy_exchange_fd,
    .interest = proxy_exchange_interest,
    .has_output = proxy_exchange_has_output,
    .status = proxy_exchange_status,
    .started = proxy_exchange_started,
    .pump = proxy_exchange_pump,
    .remaining_ms = proxy_exchange_remaining_ms,
    .cap_deadline = proxy_exchange_cap_deadline,
};

static unsigned int parse_positive(const char *text)
{
    unsigned int result = 0;
    for (; *text; ++text)
    {
        if (*text < '0' || *text > '9' || result > 6553)
            return 0;
        result = result * 10 + (unsigned int)(*text - '0');
    }
    return result <= 65535 ? result : 0;
}

int main(int argc, char **argv)
{
    unsigned int port = 0, limit = 0;
    struct proxy_routes routes = {0};
    for (int i = 1; i < argc; i += 2)
    {
        if (i + 1 == argc)
            goto usage;
        if (strcmp(argv[i], "--port") == 0)
        {
            if (port || !(port = parse_positive(argv[i + 1])))
                goto usage;
        }
        else if (strcmp(argv[i], "--max-connections") == 0)
        {
            if (limit || !(limit = parse_positive(argv[i + 1])))
                goto usage;
        }
        else if (strcmp(argv[i], "--route") == 0)
        {
            if (proxy_route_add(&routes, argv[i + 1]) < 0)
                goto usage;
        }
        else
            goto usage;
    }
    if (!port || !routes.count)
        goto usage;
    const struct proxy_config config = {.routes = &routes};
    const struct session_app app = {
        .config = &config,
        .prepare = prepare,
        .finish = finish,
        .stream_ops = &exchange_ops,
        .close_after_response = true,
    };
    return http_server_run((unsigned short)port, limit ? limit : 256u, &app);

usage:
    fputs("Usage: ./reverse-proxy --port <1-65535> --route /prefix/=IPv4:PORT"
          " [--route /prefix/=IPv4:PORT]... [--max-connections <1-65535>]\n",
          stderr);
    return EXIT_FAILURE;
}
