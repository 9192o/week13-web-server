#ifndef PROXY_WIRE_H
#define PROXY_WIRE_H

#include "http.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Body framing classification for a single HTTP response.
 *
 * PROXY_NONE    - A status code that never carries a message body.
 * PROXY_LENGTH  - Fixed Content-Length body.
 * PROXY_CHUNKED - Chunked Transfer-Encoding body.
 * PROXY_EOF     - Close-delimited body; downstream is sent chunked.
 */
enum proxy_frame
{
    PROXY_NONE,
    PROXY_LENGTH,
    PROXY_CHUNKED,
    PROXY_EOF
};

/*
 * Result of parsing a backend response status line + headers.
 *
 * frame    - How the caller should read the response body.
 * length   - Length when frame == PROXY_LENGTH (metadata only for HEAD).
 * consumed - Bytes consumed from the input buffer (status line + headers).
 * status   - Three-digit status code from the status line.
 */
struct proxy_head
{
    enum proxy_frame frame;
    size_t length;
    size_t consumed;
    int status;
    bool upstream_keep;
};

/*
 * Serialize the request head to send to a backend.
 *
 * The request line uses origin-form reconstructed from request->target.path and
 * request->target.query, preserving the original target path and query string.
 *
 * Hop-by-hop headers (Connection, Keep-Alive, Transfer-Encoding, Upgrade, TE,
 * Trailer, Proxy-Connection) and any header nominated by a Connection header
 * are removed.  Host is rewritten to the supplied `host`.  Content-Length is
 * rewritten from request->body.length (already decoded by the receiver).
 * Expect is handled locally. A new `Via: 1.1 week13` line is appended.
 * The upstream request uses HTTP/1.1 persistence.
 *
 * Returns the number of bytes written to `out` (not including a NUL
 * terminator), or 0 if a required argument is missing or the buffer is too
 * small.
 */
size_t proxy_request_head(const struct http_request *request, const char *host,
                          char *out, size_t cap);

/*
 * Parse a backend response status line and headers.
 *
 * Returns:
 *   1  - A complete head was parsed and a sanitized response head was written
 *        to `out`; `*outlen` receives the written byte count.  `head` is fully
 *        populated, including for 1xx interim responses.  The caller must skip
 *        1xx responses (100..199) and treat 101 as an error.
 *   0  - Need more input.  `head` is zeroed.
 *  -1  - Invalid syntax or output buffer too small; ignore output fields.
 *
 * The output response head is regenerated as HTTP/1.1.  Hop-by-hop headers and
 * Connection-nominated headers are removed.  Framing is regenerated:
 *   PROXY_LENGTH  -> Content-Length header
 *   PROXY_CHUNKED -> Transfer-Encoding: chunked
 *   PROXY_EOF     -> Transfer-Encoding: chunked (downstream chunked)
 *   PROXY_NONE    -> no framing header, except optional 304 length metadata
 * Via is preserved as-is. Downstream persistence is limited to fixed length.
 */
int proxy_response_head(const char *data, size_t len, bool is_head,
                        struct proxy_head *head, char *out, size_t cap,
                        size_t *outlen, bool keep_client);

#endif /* PROXY_WIRE_H */
