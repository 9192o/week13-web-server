#include "http.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Request-line parsing. */
static int is_token_character(unsigned char byte)
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
        if (!is_token_character((unsigned char)*cursor))
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

/* Header parsing. */
struct http_headers_result http_parse_headers(const char *data, size_t length, size_t request_line_length)
{
    struct http_headers_result result = {
        .status = HTTP_PARSE_ERROR,
        .error_status = 400};
    struct http_headers headers = {0};
    size_t budget = HTTP_REQUEST_HEAD_MAX - request_line_length;
    size_t offset = 0;

    while (1)
    {
        size_t line_end = offset;
        int found_crlf = 0;
        for (; line_end < length; ++line_end)
        {
            if (line_end >= budget || line_end - offset >= HTTP_HEADER_LINE_MAX)
            {
                result.error_status = 431;
                return result;
            }
            unsigned char byte = (unsigned char)data[line_end];
            if (byte == '\r')
            {
                if (line_end + 1 == length)
                    break;
                if (data[line_end + 1] != '\n')
                    return result;
                if (line_end + 2 > budget || line_end - offset + 2 > HTTP_HEADER_LINE_MAX)
                {
                    result.error_status = 431;
                    return result;
                }
                found_crlf = 1;
                break;
            }
            if ((byte < 32 && byte != '\t') || byte == 127)
                return result;
        }

        if (!found_crlf)
        {
            if (length >= budget || length - offset >= HTTP_HEADER_LINE_MAX)
                result.error_status = 431;
            else
            {
                result.status = HTTP_PARSE_NEED_MORE;
                result.error_status = 0;
            }
            return result;
        }

        if (line_end == offset)
        {
            result.status = HTTP_PARSE_COMPLETE;
            result.error_status = 0;
            result.consumed = line_end + 2;
            result.headers = headers;
            return result;
        }
        if (headers.count == HTTP_HEADER_COUNT_MAX)
        {
            result.error_status = 431;
            return result;
        }

        const char *name = data + offset;
        const char *colon = memchr(name, ':', line_end - offset);
        if (colon == NULL || colon == name)
            return result;
        for (const char *cursor = name; cursor < colon; ++cursor)
            if (!is_token_character((unsigned char)*cursor))
                return result;

        const char *value = colon + 1;
        const char *value_end = data + line_end;
        while (value < value_end && (*value == ' ' || *value == '\t'))
            ++value;
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
            --value_end;

        headers.fields[headers.count++] = (struct http_header){
            .name = {name, (size_t)(colon - name)},
            .value = {value, (size_t)(value_end - value)}};
        offset = line_end + 2;
    }
}

/* Body parsing. */
static int token_byte(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9') ||
           (byte != 0 && strchr("!#$%&'*+-.^_`|~", byte) != NULL);
}

static int equal_case(struct http_slice value, const char *text)
{
    if (value.length != strlen(text))
        return 0;
    for (size_t i = 0; i < value.length; ++i)
    {
        unsigned char byte = (unsigned char)value.data[i];
        if (byte >= 'A' && byte <= 'Z')
            byte += 'a' - 'A';
        if (byte != (unsigned char)text[i])
            return 0;
    }
    return 1;
}

static void skip_ows(const char **cursor, const char *end)
{
    while (*cursor < end && (**cursor == ' ' || **cursor == '\t'))
        ++*cursor;
}

static int read_token(const char **cursor, const char *end)
{
    const char *start = *cursor;
    while (*cursor < end && token_byte((unsigned char)**cursor))
        ++*cursor;
    return *cursor != start;
}

static int read_value(const char **cursor, const char *end)
{
    if (*cursor == end || **cursor != '"')
        return read_token(cursor, end);
    ++*cursor;
    while (*cursor < end)
    {
        unsigned char byte = (unsigned char)*(*cursor)++;
        if (byte == '"')
            return 1;
        if (byte == '\\')
        {
            if (*cursor == end)
                return 0;
            byte = (unsigned char)*(*cursor)++;
        }
        if ((byte < 32 && byte != '\t') || byte == 127)
            return 0;
    }
    return 0;
}

/* Called immediately after ';'; chunk extensions allow a valueless token. */
static int read_parameter(const char **cursor, const char *end, int require_value)
{
    skip_ows(cursor, end);
    if (!read_token(cursor, end))
        return 0;
    const char *after_name = *cursor;
    skip_ows(cursor, end);
    if (*cursor < end && **cursor == '=')
    {
        ++*cursor;
        skip_ows(cursor, end);
        return read_value(cursor, end);
    }
    *cursor = after_name;
    return !require_value;
}

static int parse_transfer_encoding(const struct http_headers *headers)
{
    size_t chunked_count = 0;
    int last_chunked = 0;
    int unsupported = 0;
    for (size_t i = 0; i < headers->count; ++i)
    {
        const struct http_header *field = &headers->fields[i];
        if (!equal_case(field->name, "transfer-encoding"))
            continue;
        const char *cursor = field->value.data;
        const char *end = cursor + field->value.length;
        do
        {
            skip_ows(&cursor, end);
            const char *start = cursor;
            if (!read_token(&cursor, end))
                return 400;
            last_chunked = equal_case((struct http_slice){start, (size_t)(cursor - start)}, "chunked");
            chunked_count += (size_t)last_chunked;
            unsupported |= !last_chunked;
            skip_ows(&cursor, end);
            while (cursor < end && *cursor == ';')
            {
                ++cursor;
                if (!read_parameter(&cursor, end, 1) || last_chunked)
                    return 400;
                skip_ows(&cursor, end);
            }
            if (cursor == end)
                break;
            if (*cursor++ != ',' || cursor == end)
                return 400;
        } while (1);
    }
    if (chunked_count != 1 || !last_chunked)
        return 400;
    return unsupported ? 501 : 0;
}

static int parse_expect(const struct http_headers *headers, int *wanted)
{
    int unsupported = 0;
    for (size_t i = 0; i < headers->count; ++i)
    {
        const struct http_header *field = &headers->fields[i];
        if (!equal_case(field->name, "expect"))
            continue;
        const char *cursor = field->value.data;
        const char *end = cursor + field->value.length;
        do
        {
            skip_ows(&cursor, end);
            const char *start = cursor;
            if (!read_token(&cursor, end))
                return 400;
            int known = equal_case((struct http_slice){start, (size_t)(cursor - start)}, "100-continue");
            skip_ows(&cursor, end);
            if (cursor < end && *cursor == '=')
            {
                known = 0;
                ++cursor;
                skip_ows(&cursor, end);
                if (!read_value(&cursor, end))
                    return 400;
                skip_ows(&cursor, end);
                while (cursor < end && *cursor == ';')
                {
                    ++cursor;
                    if (!read_parameter(&cursor, end, 0))
                        return 400;
                    skip_ows(&cursor, end);
                }
            }
            *wanted |= known;
            unsupported |= !known;
            if (cursor == end)
                break;
            if (*cursor++ != ',' || cursor == end)
                return 400;
        } while (1);
    }
    return unsupported ? 417 : 0;
}

int http_body_init(struct http_body_parser *parser, const struct http_headers *headers)
{
    *parser = (struct http_body_parser){.state = HTTP_BODY_DONE};
    const struct http_slice *content_length = NULL;
    int transfer_encoding = 0;
    for (size_t i = 0; i < headers->count; ++i)
    {
        const struct http_header *field = &headers->fields[i];
        if (equal_case(field->name, "content-length"))
        {
            if (content_length != NULL)
            {
                parser->error_status = 400;
                break;
            }
            content_length = &field->value;
        }
        if (equal_case(field->name, "transfer-encoding"))
            transfer_encoding = 1;
    }
    if (!parser->error_status && content_length && transfer_encoding)
        parser->error_status = 400;
    if (!parser->error_status && transfer_encoding)
    {
        parser->error_status = parse_transfer_encoding(headers);
        parser->state = HTTP_BODY_CHUNK_SIZE;
    }
    if (!parser->error_status && content_length)
    {
        size_t value = 0;
        if (content_length->length == 0)
            parser->error_status = 400;
        for (size_t i = 0; i < content_length->length; ++i)
        {
            unsigned char byte = (unsigned char)content_length->data[i];
            if (byte < '0' || byte > '9' || value > (SIZE_MAX - (byte - '0')) / 10)
            {
                parser->error_status = 400;
                break;
            }
            value = value * 10 + (byte - '0');
        }
        if (!parser->error_status && value > HTTP_BODY_MAX)
            parser->error_status = 413;
        if (!parser->error_status && value != 0)
        {
            parser->remaining = value;
            parser->state = HTTP_BODY_LENGTH;
        }
    }
    if (!parser->error_status)
        parser->error_status = parse_expect(headers, &parser->expect_continue);
    if (parser->error_status)
        parser->state = HTTP_BODY_FAILED;
    return parser->error_status;
}

static int append_body(struct http_body_parser *parser, const char *data, size_t length)
{
    if (length == 0)
        return 0;
    size_t needed = parser->length + length;
    if (needed > parser->capacity)
    {
        size_t capacity = parser->capacity ? parser->capacity : 4096;
        while (capacity < needed)
            capacity *= 2;
        if (capacity > HTTP_BODY_MAX)
            capacity = HTTP_BODY_MAX;
        char *grown = realloc(parser->data, capacity);
        if (grown == NULL)
            return 500;
        parser->data = grown;
        parser->capacity = capacity;
    }
    memcpy(parser->data + parser->length, data, length);
    parser->length = needed;
    return 0;
}

static int parse_chunk_size(struct http_body_parser *parser)
{
    const char *cursor = parser->line;
    const char *end = cursor + parser->line_length - 2;
    size_t size = 0;
    const char *start = cursor;
    while (cursor < end)
    {
        unsigned char byte = (unsigned char)*cursor;
        unsigned int digit;
        if (byte >= '0' && byte <= '9')
            digit = byte - '0';
        else if (byte >= 'a' && byte <= 'f')
            digit = byte - 'a' + 10;
        else if (byte >= 'A' && byte <= 'F')
            digit = byte - 'A' + 10;
        else
            break;
        if (size > (SIZE_MAX - digit) / 16)
            return 400;
        size = size * 16 + digit;
        ++cursor;
    }
    if (cursor == start)
        return 400;
    const char *first_semicolon = NULL;
    while (cursor < end)
    {
        skip_ows(&cursor, end);
        if (cursor == end || *cursor != ';')
            return 400;
        if (!first_semicolon)
            first_semicolon = cursor;
        ++cursor;
        if (!read_parameter(&cursor, end, 0))
            return 400;
    }
    if (first_semicolon)
    {
        size_t extra = (size_t)(end - first_semicolon);
        if (extra > HTTP_CHUNK_EXTENSIONS_MAX - parser->extension_bytes)
            return 431;
        parser->extension_bytes += extra;
    }
    if (size > HTTP_BODY_MAX - parser->length)
        return 413;
    parser->remaining = size;
    parser->state = size ? HTTP_BODY_CHUNK_DATA : HTTP_BODY_TRAILERS;
    return 0;
}

static int parse_trailer(struct http_body_parser *parser)
{
    size_t length = parser->line_length - 2;
    if (length == 0)
    {
        parser->state = HTTP_BODY_DONE;
        return 0;
    }
    if (++parser->trailer_count > HTTP_TRAILER_COUNT_MAX)
        return 431;
    const char *colon = memchr(parser->line, ':', length);
    if (colon == NULL || colon == parser->line)
        return 400;
    for (const char *cursor = parser->line; cursor < colon; ++cursor)
        if (!token_byte((unsigned char)*cursor))
            return 400;
    struct http_slice name = {parser->line, (size_t)(colon - parser->line)};
    const char *forbidden[] = {
        "host", "content-length", "transfer-encoding", "connection", "trailer",
        "te", "upgrade", "expect", "content-encoding", "content-type",
        "authorization", "proxy-authorization", "cookie"};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i)
        if (equal_case(name, forbidden[i]))
            return 400;
    /* Other syntactically valid trailers are ignored, never merged into headers. */
    return 0;
}

struct http_body_result http_parse_body(struct http_body_parser *parser, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length && parser->state != HTTP_BODY_DONE && parser->state != HTTP_BODY_FAILED)
    {
        int error = 0;
        switch (parser->state)
        {
        case HTTP_BODY_LENGTH:
        case HTTP_BODY_CHUNK_DATA:
        {
            size_t take = length - offset;
            if (take > parser->remaining)
                take = parser->remaining;
            error = append_body(parser, data + offset, take);
            if (!error)
            {
                offset += take;
                parser->remaining -= take;
                if (parser->remaining == 0)
                    parser->state = parser->state == HTTP_BODY_LENGTH
                                        ? HTTP_BODY_DONE
                                        : HTTP_BODY_CHUNK_CR;
            }
            break;
        }
        case HTTP_BODY_CHUNK_CR:
            if (data[offset++] != '\r')
                error = 400;
            else
                parser->state = HTTP_BODY_CHUNK_LF;
            break;
        case HTTP_BODY_CHUNK_LF:
            if (data[offset++] != '\n')
                error = 400;
            else
                parser->state = HTTP_BODY_CHUNK_SIZE;
            break;
        case HTTP_BODY_CHUNK_SIZE:
        case HTTP_BODY_TRAILERS:
        {
            unsigned char byte = (unsigned char)data[offset++];
            if (parser->state == HTTP_BODY_TRAILERS && ++parser->trailer_bytes > HTTP_TRAILER_MAX)
                error = 431;
            else if ((byte < 32 && byte != '\t' && byte != '\r' && byte != '\n') || byte == 127)
                error = 400;
            else if ((byte == '\n' && (parser->line_length == 0 ||
                                       parser->line[parser->line_length - 1] != '\r')) ||
                     (parser->line_length && parser->line[parser->line_length - 1] == '\r' &&
                      byte != '\n'))
                error = 400;
            else
            {
                parser->line[parser->line_length++] = (char)byte;
                if (byte == '\n')
                {
                    error = parser->state == HTTP_BODY_CHUNK_SIZE
                                ? parse_chunk_size(parser)
                                : parse_trailer(parser);
                    parser->line_length = 0;
                }
                else if (parser->line_length == HTTP_CHUNK_LINE_MAX)
                    error = 431;
            }
            break;
        }
        case HTTP_BODY_DONE:
        case HTTP_BODY_FAILED:
            break;
        }
        if (error)
        {
            parser->error_status = error;
            parser->state = HTTP_BODY_FAILED;
        }
    }
    return (struct http_body_result){
        .status = parser->state == HTTP_BODY_FAILED ? HTTP_PARSE_ERROR
                  : parser->state == HTTP_BODY_DONE ? HTTP_PARSE_COMPLETE
                                                    : HTTP_PARSE_NEED_MORE,
        .consumed = offset,
        .error_status = parser->error_status};
}

void http_body_destroy(struct http_body_parser *parser)
{
    free(parser->data);
    *parser = (struct http_body_parser){0};
}

/* Response bytes. */

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

const char HTTP_CONTENT_TOO_LARGE_RESPONSE[] =
    "HTTP/1.1 413 Content Too Large\r\n"
    "Content-Length: 0\r\nConnection: close\r\n\r\n";

const char HTTP_HEADERS_TOO_LARGE_RESPONSE[] =
    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
    "Content-Length: 0\r\nConnection: close\r\n\r\n";

const char HTTP_EXPECTATION_FAILED_RESPONSE[] =
    "HTTP/1.1 417 Expectation Failed\r\n"
    "Content-Length: 0\r\nConnection: close\r\n\r\n";

const char HTTP_NOT_IMPLEMENTED_RESPONSE[] =
    "HTTP/1.1 501 Not Implemented\r\n"
    "Content-Length: 0\r\nConnection: close\r\n\r\n";

const char HTTP_INTERNAL_ERROR_RESPONSE[] =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Length: 0\r\nConnection: close\r\n\r\n";

const char HTTP_CONTINUE_RESPONSE[] = "HTTP/1.1 100 Continue\r\n\r\n";
