#include "proxy_routes.h"

#include <arpa/inet.h>
#include <string.h>

int proxy_route_add(struct proxy_routes *routes, const char *arg)
{
    const char *equal = strchr(arg, '=');
    if (!equal || equal == arg || arg[0] != '/' || equal[-1] != '/' ||
        routes->count == PROXY_ROUTE_MAX)
        return -1;
    size_t length = (size_t)(equal - arg);
    if (length >= HTTP_REQUEST_LINE_MAX)
        return -1;
    for (const char *p = arg; p < equal; ++p)
    {
        char c = *p;
        if (!(c == '/' || c == '-' || c == '_' ||
              (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9')) ||
            (c == '/' && p > arg && p[-1] == '/'))
            return -1;
    }
    const char *host = equal + 1;
    const char *colon = strchr(host, ':');
    char ip[INET_ADDRSTRLEN];
    if (!colon || colon == host || (size_t)(colon - host) >= sizeof(ip) || !colon[1])
        return -1;
    memcpy(ip, host, (size_t)(colon - host));
    ip[colon - host] = '\0';
    struct sockaddr_in address = {.sin_family = AF_INET};
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1)
        return -1;
    unsigned int port = 0;
    for (const char *p = colon + 1; *p; ++p)
    {
        if (*p < '0' || *p > '9' || port > 6553)
            return -1;
        port = port * 10 + (unsigned int)(*p - '0');
    }
    if (!port || port > 65535)
        return -1;
    address.sin_port = htons((uint16_t)port);
    const size_t index = routes->count;
    size_t first = index, last = index;
    for (size_t i = 0; i < index; ++i)
    {
        const struct proxy_route *peer = &routes->entries[i];
        if (peer->length != length || memcmp(peer->prefix, arg, length) != 0)
            continue;
        if (peer->address.sin_addr.s_addr == address.sin_addr.s_addr &&
            peer->address.sin_port == address.sin_port)
            return -1;
        if (first == index)
            first = i;
        last = i;
    }
    routes->entries[index] = (struct proxy_route){
        .prefix = arg, .length = length, .address = address, .host = host,
        .next_peer = first, .cursor = index,
    };
    routes->entries[last].next_peer = index;
    ++routes->count;
    return 0;
}

const struct proxy_route *proxy_route_match(const struct proxy_routes *routes,
                                            const struct http_target *target)
{
    const struct proxy_route *best = NULL;
    if (!routes || target->form != HTTP_TARGET_ORIGIN)
        return NULL;
    for (size_t i = 0; i < routes->count; ++i)
    {
        const struct proxy_route *route = &routes->entries[i];
        if (route->length <= target->path.length &&
            (!best || route->length > best->length) &&
            memcmp(route->prefix, target->path.data, route->length) == 0)
            best = route;
    }
    return best;
}

const struct proxy_route *proxy_route_select(struct proxy_routes *routes,
                                             const struct http_target *target)
{
    const struct proxy_route *match = proxy_route_match(routes, target);
    if (!match)
        return NULL;
    struct proxy_route *group = &routes->entries[match - routes->entries];
    const struct proxy_route *selected = &routes->entries[group->cursor];
    group->cursor = selected->next_peer;
    return selected;
}
