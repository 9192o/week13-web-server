#define _POSIX_C_SOURCE 200809L
#include "proxy_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROXY_STATUS_LINE_MAX HTTP_REQUEST_LINE_MAX

struct parsed_status_line
{
    unsigned int version_major;
    unsigned int version_minor;
    int status;
    const char *reason;
    size_t reason_len;
};

static int slice_equal_case(struct http_slice value, const char *text)
{
    size_t len = strlen(text);
    if (value.length != len)
        return 0;
    for (size_t i = 0; i < len; ++i)
    {
        unsigned char a = (unsigned char)value.data[i];
        unsigned char b = (unsigned char)text[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z')
            b += 'a' - 'A';
        if (a != b)
            return 0;
    }
    return 1;
}

static int token_equal_case(const char *start, size_t len,
                            const struct http_slice *name)
{
    if (len != name->length)
        return 0;
    for (size_t i = 0; i < len; ++i)
    {
        unsigned char a = (unsigned char)start[i];
        unsigned char b = (unsigned char)name->data[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z')
            b += 'a' - 'A';
        if (a != b)
            return 0;
    }
    return 1;
}

static void skip_ows(const char **cursor, const char *end)
{
    while (*cursor < end && (**cursor == ' ' || **cursor == '\t'))
        ++*cursor;
}

static int is_hop_by_hop(const struct http_slice *name)
{
    static const char *hop[] = {
        "connection", "keep-alive", "transfer-encoding", "upgrade",
        "te", "trailer", "proxy-connection", "proxy-authorization",
        "proxy-authenticate"};
    for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); ++i)
        if (slice_equal_case(*name, hop[i]))
            return 1;
    return 0;
}

static int is_nominated(const struct http_headers *headers,
                        const struct http_slice *name)
{
    for (size_t i = 0; i < headers->count; ++i)
    {
        const struct http_header *field = &headers->fields[i];
        if (!slice_equal_case(field->name, "connection"))
            continue;
        const char *cursor = field->value.data;
        const char *end = cursor + field->value.length;
        while (cursor < end)
        {
            skip_ows(&cursor, end);
            const char *start = cursor;
            while (cursor < end && *cursor != ',')
                ++cursor;
            const char *last = cursor;
            while (last > start && (last[-1] == ' ' || last[-1] == '\t'))
                --last;
            if (start < last &&
                token_equal_case(start, (size_t)(last - start), name))
                return 1;
            if (cursor < end)
                ++cursor;
        }
    }
    return 0;
}

static int append_bytes(char *out, size_t cap, size_t *pos, const char *data,
                        size_t len)
{
    if (*pos > cap || len > cap - *pos)
        return -1;
    if (len)
        memcpy(out + *pos, data, len);
    *pos += len;
    return 0;
}

static int append_cstr(char *out, size_t cap, size_t *pos, const char *s)
{
    return append_bytes(out, cap, pos, s, strlen(s));
}

static int append_slice(char *out, size_t cap, size_t *pos, struct http_slice s)
{
    return append_bytes(out, cap, pos, s.data, s.length);
}

size_t proxy_request_head(const struct http_request *request, const char *host,
                          char *out, size_t cap)
{
    if (!request || !host || !out || cap == 0)
        return 0;

    size_t pos = 0;

    if (append_slice(out, cap, &pos, request->line.method) < 0)
        return 0;
    if (append_cstr(out, cap, &pos, " ") < 0)
        return 0;
    if (append_slice(out, cap, &pos, request->target.path) < 0)
        return 0;
    if (request->target.query.data)
    {
        if (append_cstr(out, cap, &pos, "?") < 0)
            return 0;
        if (append_slice(out, cap, &pos, request->target.query) < 0)
            return 0;
    }
    if (append_cstr(out, cap, &pos, " HTTP/1.1\r\n") < 0)
        return 0;

    if (append_cstr(out, cap, &pos, "Host: ") < 0)
        return 0;
    if (append_cstr(out, cap, &pos, host) < 0)
        return 0;
    if (append_cstr(out, cap, &pos, "\r\n") < 0)
        return 0;

    for (size_t i = 0; i < request->headers.count; ++i)
    {
        const struct http_header *field = &request->headers.fields[i];
        if (is_hop_by_hop(&field->name))
            continue;
        if (slice_equal_case(field->name, "host") ||
            slice_equal_case(field->name, "content-length") ||
            slice_equal_case(field->name, "expect"))
            continue;
        if (is_nominated(&request->headers, &field->name))
            continue;

        if (append_slice(out, cap, &pos, field->name) < 0)
            return 0;
        if (append_cstr(out, cap, &pos, ": ") < 0)
            return 0;
        if (append_slice(out, cap, &pos, field->value) < 0)
            return 0;
        if (append_cstr(out, cap, &pos, "\r\n") < 0)
            return 0;
    }

    char cl[64];
    int cl_len = snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n",
                          request->body.length);
    if (cl_len <= 0 || (size_t)cl_len >= sizeof(cl))
        return 0;
    if (append_bytes(out, cap, &pos, cl, (size_t)cl_len) < 0)
        return 0;

    if (append_cstr(out, cap, &pos, "Via: 1.1 week13\r\n") < 0)
        return 0;

    if (append_cstr(out, cap, &pos, "Connection: close\r\n\r\n") < 0)
        return 0;

    return pos;
}

static int parse_status_line(const char *data, size_t len,
                             struct parsed_status_line *line,
                             size_t *consumed)
{
    size_t i = 0;
    for (; i < len && i < PROXY_STATUS_LINE_MAX; ++i)
    {
        unsigned char b = (unsigned char)data[i];
        if (b == '\r')
        {
            if (i + 1 == len)
                return 0;
            if (data[i + 1] != '\n')
                return -1;
            break;
        }
        if (b < 32 || b == 127)
            return -1;
    }

    if (i == len || i == PROXY_STATUS_LINE_MAX)
    {
        if (len >= PROXY_STATUS_LINE_MAX)
            return -1;
        return 0;
    }

    size_t line_len = i;
    *consumed = i + 2;

    if (line_len < 13 || memcmp(data, "HTTP/", 5) != 0)
        return -1;
    if (data[5] < '0' || data[5] > '9' || data[6] != '.' ||
        data[7] < '0' || data[7] > '9' || data[8] != ' ')
        return -1;
    if (data[9] < '0' || data[9] > '9' || data[10] < '0' ||
        data[10] > '9' || data[11] < '0' || data[11] > '9' ||
        data[12] != ' ')
        return -1;

    int status = (data[9] - '0') * 100 + (data[10] - '0') * 10 +
                 (data[11] - '0');
    if (status < 100 || status > 599)
        return -1;

    line->version_major = (unsigned int)(data[5] - '0');
    line->version_minor = (unsigned int)(data[7] - '0');
    if (line->version_major != 1 || line->version_minor > 1)
        return -1;
    line->status = status;
    line->reason = data + 13;
    line->reason_len = line_len - 13;
    for (size_t j = 0; j < line->reason_len; ++j)
    {
        unsigned char b = (unsigned char)line->reason[j];
        if (b < 32 || b == 127)
            return -1;
    }
    return 1;
}

static int parse_content_length(const struct http_headers *headers,
                                size_t *value)
{
    const struct http_slice *cl = NULL;
    for (size_t i = 0; i < headers->count; ++i)
    {
        if (!slice_equal_case(headers->fields[i].name, "content-length"))
            continue;
        if (cl != NULL)
            return -1;
        cl = &headers->fields[i].value;
    }
    if (!cl)
        return 0;
    if (cl->length == 0)
        return -1;

    size_t v = 0;
    for (size_t i = 0; i < cl->length; ++i)
    {
        unsigned char b = (unsigned char)cl->data[i];
        if (b < '0' || b > '9')
            return -1;
        unsigned int digit = b - '0';
        if (v > (SIZE_MAX - digit) / 10)
            return -1;
        v = v * 10 + digit;
    }
    *value = v;
    return 1;
}

static int has_chunked_transfer_encoding(const struct http_headers *headers)
{
    int found = 0;
    for (size_t i = 0; i < headers->count; ++i)
    {
        if (!slice_equal_case(headers->fields[i].name, "transfer-encoding"))
            continue;
        if (found || !slice_equal_case(headers->fields[i].value, "chunked"))
            return -1;
        found = 1;
    }
    return found;
}

int proxy_response_head(const char *data, size_t len, bool is_head,
                        struct proxy_head *head, char *out, size_t cap,
                        size_t *outlen)
{
    if (!data || !head || !out || !outlen || cap == 0)
        return -1;

    *head = (struct proxy_head){0};
    *outlen = 0;

    struct parsed_status_line line;
    size_t status_consumed = 0;
    int rc = parse_status_line(data, len, &line, &status_consumed);
    if (rc <= 0)
        return rc;

    head->status = line.status;

    struct http_headers_result headers = http_parse_headers(
        data + status_consumed, len - status_consumed, status_consumed);
    if (headers.status == HTTP_PARSE_NEED_MORE)
    {
        *head = (struct proxy_head){0};
        return 0;
    }
    if (headers.status == HTTP_PARSE_ERROR)
        return -1;

    head->consumed = status_consumed + headers.consumed;

    size_t cl_value = 0;
    int cl_present = parse_content_length(&headers.headers, &cl_value);
    if (cl_present < 0)
        return -1;

    int chunked = has_chunked_transfer_encoding(&headers.headers);
    if (chunked < 0 || (chunked && (cl_present || line.version_minor == 0)))
        return -1;

    if (line.status / 100 == 1 || line.status == 204 || line.status == 304)
    {
        head->frame = PROXY_NONE;
        head->length = 0;
    }
    else if (is_head)
    {
        head->frame = PROXY_LENGTH;
        head->length = cl_present ? cl_value : 0;
    }
    else if (chunked)
    {
        head->frame = PROXY_CHUNKED;
        head->length = 0;
    }
    else if (cl_present)
    {
        head->frame = PROXY_LENGTH;
        head->length = cl_value;
    }
    else
    {
        head->frame = PROXY_EOF;
        head->length = 0;
    }

    size_t pos = 0;
    if (append_cstr(out, cap, &pos, "HTTP/1.1 ") < 0)
        return -1;
    char status_str[4];
    snprintf(status_str, sizeof(status_str), "%03d", line.status);
    if (append_bytes(out, cap, &pos, status_str, 3) < 0)
        return -1;
    if (append_cstr(out, cap, &pos, " ") < 0)
        return -1;
    if (append_bytes(out, cap, &pos, line.reason, line.reason_len) < 0)
        return -1;
    if (append_cstr(out, cap, &pos, "\r\n") < 0)
        return -1;

    for (size_t i = 0; i < headers.headers.count; ++i)
    {
        const struct http_header *field = &headers.headers.fields[i];
        if (is_hop_by_hop(&field->name))
            continue;
        if (slice_equal_case(field->name, "content-length"))
            continue;
        if (slice_equal_case(field->name, "transfer-encoding"))
            continue;
        if (is_nominated(&headers.headers, &field->name))
            continue;

        if (append_slice(out, cap, &pos, field->name) < 0)
            return -1;
        if (append_cstr(out, cap, &pos, ": ") < 0)
            return -1;
        if (append_slice(out, cap, &pos, field->value) < 0)
            return -1;
        if (append_cstr(out, cap, &pos, "\r\n") < 0)
            return -1;
    }

    if ((head->frame == PROXY_LENGTH && (!is_head || cl_present)) ||
        (line.status == 304 && cl_present))
    {
        char cl[64];
        int n = snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n",
                         cl_value);
        if (n <= 0 || (size_t)n >= sizeof(cl))
            return -1;
        if (append_bytes(out, cap, &pos, cl, (size_t)n) < 0)
            return -1;
    }
    else if (head->frame == PROXY_CHUNKED || head->frame == PROXY_EOF)
    {
        if (append_cstr(out, cap, &pos,
                        "Transfer-Encoding: chunked\r\n") < 0)
            return -1;
    }

    if (append_cstr(out, cap, &pos, "Connection: close\r\n\r\n") < 0)
        return -1;

    *outlen = pos;
    return 1;
}
