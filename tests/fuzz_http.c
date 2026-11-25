#include "http.h"
#include "proxy_wire.h"

#include <stdint.h>
#include <stddef.h>

#define FUZZ_OUT_MAX 65536

/*
 * The fuzzer exercises the pure parsers used by both the web server and the
 * reverse proxy.  No sockets or files are touched here.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > HTTP_BODY_MAX + HTTP_REQUEST_HEAD_MAX)
        return 0;

    const char *input = (const char *)data;

    struct http_request_line_result line =
        http_parse_request_line(input, size);
    if (line.status == HTTP_PARSE_COMPLETE && line.consumed <= size)
    {
        struct http_headers_result headers = http_parse_headers(
            input + line.consumed, size - line.consumed, line.consumed);
        if (headers.status == HTTP_PARSE_COMPLETE &&
            line.consumed + headers.consumed <= size)
        {
            struct http_target target;
            if (http_validate_request_head(&line.request, &headers.headers,
                                           &target) == 0)
            {
                struct http_request request = {
                    .line = line.request,
                    .headers = headers.headers,
                    .target = target,
                };

                struct http_body_parser body;
                if (http_body_init(&body, &headers.headers) == 0)
                {
                    const char *body_data =
                        input + line.consumed + headers.consumed;
                    size_t body_size =
                        size - line.consumed - headers.consumed;
                    (void)http_parse_body(&body, body_data, body_size);
                    request.body = (struct http_slice){body.data, body.length};

                    char request_out[FUZZ_OUT_MAX];
                    (void)proxy_request_head(&request, "127.0.0.1:1",
                                             request_out, sizeof request_out);
                }
                http_body_destroy(&body);
            }
        }
    }

    struct proxy_head head;
    char response_out[FUZZ_OUT_MAX];
    size_t response_len = 0;
    (void)proxy_response_head(input, size, false, &head, response_out,
                              sizeof response_out, &response_len, true);
    (void)proxy_response_head(input, size, true, &head, response_out,
                              sizeof response_out, &response_len, false);

    return 0;
}
