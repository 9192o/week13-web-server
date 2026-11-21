#include "router.h"

#include <string.h>

static int slice_is(struct http_slice slice, const char *text)
{
    return slice.length == strlen(text) &&
           memcmp(slice.data, text, slice.length) == 0;
}

static int hex_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9')
        return byte - '0';
    if (byte >= 'a' && byte <= 'f')
        return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F')
        return byte - 'A' + 10;
    return -1;
}

struct router_result router_match_request(const struct http_request *request)
{
    int get = slice_is(request->line.method, "GET");
    int head = slice_is(request->line.method, "HEAD");
    int post = slice_is(request->line.method, "POST");
    if (!get && !head && !post)
        return (struct router_result){ROUTER_ERROR, 501, NULL};
    if (request->target.form != HTTP_TARGET_ORIGIN &&
        request->target.form != HTTP_TARGET_ABSOLUTE)
        return (struct router_result){ROUTER_ERROR, 404, NULL};

    char path[HTTP_REQUEST_LINE_MAX];
    size_t length = 0;
    for (size_t i = 0; i < request->target.path.length; ++i)
    {
        unsigned char byte = (unsigned char)request->target.path.data[i];
        if (byte == '%')
        {
            if (request->target.path.length - i < 3)
                return (struct router_result){ROUTER_ERROR, 400, NULL};
            int high = hex_value((unsigned char)request->target.path.data[i + 1]);
            int low = hex_value((unsigned char)request->target.path.data[i + 2]);
            if (high < 0 || low < 0)
                return (struct router_result){ROUTER_ERROR, 400, NULL};
            byte = (unsigned char)(high * 16 + low);
            i += 2;
        }
        if (!byte)
            return (struct router_result){ROUTER_ERROR, 400, NULL};
        path[length++] = (char)byte;
    }
    struct http_slice decoded = {path, length};
    if (slice_is(decoded, "/") || slice_is(decoded, "/index.html"))
    {
        if (get || head)
            return (struct router_result){ROUTER_INDEX, 200, NULL};
        return (struct router_result){ROUTER_ERROR, 405, "GET, HEAD"};
    }
    if (slice_is(decoded, "/echo"))
    {
        if (post)
            return (struct router_result){ROUTER_ECHO, 200, NULL};
        return (struct router_result){ROUTER_ERROR, 405, "POST"};
    }
    return (struct router_result){ROUTER_ERROR, 404, NULL};
}
