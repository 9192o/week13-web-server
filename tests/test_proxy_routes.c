#include "proxy_routes.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    struct proxy_routes routes = {0};
    assert(proxy_route_add(&routes, "/api/admin/=127.0.0.1:8002") == 0);
    assert(proxy_route_add(&routes, "/api/=127.0.0.1:8001") == 0);
    assert(proxy_route_add(&routes, "/api/=127.0.0.1:8003") == 0);
    assert(proxy_route_add(&routes, "/api/=127.0.0.1:08003") == -1);
    assert(proxy_route_add(&routes, "/api=127.0.0.1:8001") == -1);
    assert(proxy_route_add(&routes, "/other/=127.0.0.1:65536") == -1);
    assert(proxy_route_add(&routes, "/other/=localhost:8001") == -1);
    struct http_target target = {.form = HTTP_TARGET_ORIGIN};
    target.path = (struct http_slice){"/api/admin/read", sizeof("/api/admin/read") - 1};
    assert(proxy_route_match(&routes, &target) == &routes.entries[0]);
    assert(strcmp(routes.entries[0].host, "127.0.0.1:8002") == 0);
    target.path = (struct http_slice){"/api/adminish", sizeof("/api/adminish") - 1};
    assert(proxy_route_match(&routes, &target) == &routes.entries[1]);
    assert(proxy_route_select(&routes, &target) == &routes.entries[1]);
    assert(proxy_route_match(&routes, &target) == &routes.entries[1]);
    assert(proxy_route_select(&routes, &target) == &routes.entries[2]);
    assert(proxy_route_select(&routes, &target) == &routes.entries[1]);
    target.path = (struct http_slice){"/apish/", sizeof("/apish/") - 1};
    assert(proxy_route_match(&routes, &target) == NULL);
    target.path = (struct http_slice){"/api/", sizeof("/api/") - 1};
    target.form = HTTP_TARGET_ABSOLUTE;
    assert(proxy_route_match(&routes, &target) == NULL);
}
