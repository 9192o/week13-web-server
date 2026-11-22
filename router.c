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

static int valid_utf8(const unsigned char *path, size_t length)
{
    for (size_t i = 0; i < length;)
    {
        unsigned char first = path[i++];
        if (first < 0x80)
            continue;
        unsigned int rest;
        unsigned char low = 0x80, high = 0xbf;
        if (first >= 0xc2 && first <= 0xdf)
            rest = 1;
        else if (first >= 0xe0 && first <= 0xef)
        {
            rest = 2;
            if (first == 0xe0) low = 0xa0;
            if (first == 0xed) high = 0x9f;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            rest = 3;
            if (first == 0xf0) low = 0x90;
            if (first == 0xf4) high = 0x8f;
        }
        else
            return 0;
        if (length - i < rest || path[i] < low || path[i] > high)
            return 0;
        for (++i; --rest; ++i)
            if (path[i] < 0x80 || path[i] > 0xbf)
                return 0;
    }
    return 1;
}

static struct router_result route_error(int status, const char *allow)
{
    return (struct router_result){.status = status, .allow = allow};
}

struct router_result router_match_request(const struct http_request *request)
{
    int get = slice_is(request->line.method, "GET");
    int head = slice_is(request->line.method, "HEAD");
    int post = slice_is(request->line.method, "POST");
    if (!get && !head && !post)
        return route_error(501, NULL);
    if (request->target.form != HTTP_TARGET_ORIGIN &&
        request->target.form != HTTP_TARGET_ABSOLUTE)
        return route_error(404, NULL);

    struct router_result result = {.handler = ROUTER_STATIC, .status = 200};
    char *path = result.path;
    if (request->target.path.length >= sizeof(result.path))
        return route_error(414, NULL);
    size_t length = 0;
    for (size_t i = 0; i < request->target.path.length; ++i)
    {
        unsigned char byte = (unsigned char)request->target.path.data[i];
        if (byte == '%')
        {
            if (request->target.path.length - i < 3)
                return route_error(400, NULL);
            int high = hex_value((unsigned char)request->target.path.data[i + 1]);
            int low = hex_value((unsigned char)request->target.path.data[i + 2]);
            if (high < 0 || low < 0)
                return route_error(400, NULL);
            byte = (unsigned char)(high * 16 + low);
            if (byte == '/')
                return route_error(400, NULL);
            i += 2;
        }
        if (!byte || byte == '\\')
            return route_error(400, NULL);
        path[length++] = (char)byte;
    }
    struct http_slice decoded = {path, length};
    if (!valid_utf8((const unsigned char *)path, length))
        return route_error(400, NULL);
    if (slice_is(decoded, "/echo"))
    {
        if (post)
            return (struct router_result){.handler = ROUTER_ECHO, .status = 200};
        return route_error(405, "POST");
    }
    for (size_t start = 1; start < length;)
    {
        size_t end = start;
        while (end < length && path[end] != '/')
            ++end;
        size_t part_length = end - start;
        if (!part_length)
            return route_error(403, NULL);
        if (path[start] == '.')
            return route_error(part_length == 1 || (part_length == 2 && path[start + 1] == '.') ?
                               403 : 404, NULL);
        start = end + 1;
    }
    return result;
}
