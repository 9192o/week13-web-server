#include "http.h"

#include <string.h>

static int is_method_character(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9') ||
           (byte != 0 && strchr("!#$%&'*+-.^_`|~", byte) != NULL);
}

struct http_request_line_result http_parse_request_line(const char *data, size_t length)
{
    struct http_request_line_result result = {
        .status = HTTP_PARSE_ERROR,
        .error_status = 400};
    size_t line_length = 0;
    int found_crlf = 0;

    for (size_t i = 0; i < length && i < HTTP_REQUEST_LINE_MAX; ++i)
    {
        unsigned char byte = (unsigned char)data[i];
        if (byte == '\r')
        {
            if (i + 1 == length)
                break;

            if (data[i + 1] != '\n')
                return result;

            if (i + 2 > HTTP_REQUEST_LINE_MAX)
            {
                result.error_status = 414;
                return result;
            }
            line_length = i;
            found_crlf = 1;
            break;
        }
        if (byte < 32 || byte == 127)
            return result;
    }

    if (!found_crlf)
    {
        if (length >= HTTP_REQUEST_LINE_MAX)
            result.error_status = 414;
        else
        {
            result.status = HTTP_PARSE_NEED_MORE;
            result.error_status = 0;
        }
        return result;
    }

    const char *method_end = memchr(data, ' ', line_length);
    if (method_end == NULL || method_end == data)
        return result;

    for (const char *cursor = data; cursor < method_end; ++cursor)
        if (!is_method_character((unsigned char)*cursor))
            return result;

    const char *target = method_end + 1;
    const char *line_end = data + line_length;
    const char *target_end = memchr(target, ' ', (size_t)(line_end - target));
    if (target_end == NULL || target_end == target)
        return result;

    const char *version = target_end + 1;
    if (line_end - version != 8 || memcmp(version, "HTTP/", 5) != 0 ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9')
    {
        return result;
    }

    result.status = HTTP_PARSE_COMPLETE;
    result.error_status = 0;
    result.consumed = line_length + 2;
    result.request.method = (struct http_slice){data, (size_t)(method_end - data)};
    result.request.target = (struct http_slice){target, (size_t)(target_end - target)};
    result.request.version_major = (unsigned int)(version[5] - '0');
    result.request.version_minor = (unsigned int)(version[7] - '0');
    return result;
}

const char HTTP_OK_RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

const char HTTP_BAD_REQUEST_RESPONSE[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

const char HTTP_URI_TOO_LONG_RESPONSE[] =
    "HTTP/1.1 414 URI Too Long\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";
