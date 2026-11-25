#ifndef HTTP_UPSTREAM_H
#define HTTP_UPSTREAM_H

#include "config.h"
#include "http.h"

#include <stddef.h>

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} upstream_header_t;

typedef struct {
    int status;
    int version_major;
    int version_minor;
    char reason[64];
    upstream_header_t headers[HTTP_MAX_HEADERS];
    size_t header_count;
    size_t header_len;
    int has_content_length;
    size_t content_length;
    int chunked;
    int connection_close;
    int connection_keep_alive;
} upstream_response_t;

void upstream_response_init(upstream_response_t *res);

/*
 * Parse an upstream response header block. Returns HTTP_PARSE_INCOMPLETE when
 * the header terminator is not present, HTTP_BAD_GATEWAY for malformed input,
 * or HTTP_OK on success.
 */
http_status_t upstream_response_parse(const char *buf, size_t len,
                                      const config_t *cfg,
                                      upstream_response_t *res);

#endif
