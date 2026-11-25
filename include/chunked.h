#ifndef CHUNKED_H
#define CHUNKED_H

#include <stddef.h>

typedef enum {
    CHUNK_SIZE,
    CHUNK_DATA,
    CHUNK_DATA_CRLF,
    CHUNK_TRAILER,
    CHUNK_DONE,
    CHUNK_ERROR
} chunk_state_t;

typedef struct {
    chunk_state_t state;
    size_t remaining;
    char line[64];
    size_t line_len;
    int crlf_state;
    int trailer_line_len;
    int trailer_prev_cr;
    size_t data_bytes;
} chunk_parser_t;

void chunk_parser_init(chunk_parser_t *p);

/*
 * Consume bytes from `buf`. The caller forwards the same consumed prefix
 * unchanged. Returns the number of bytes consumed (0..len).
 */
size_t chunk_parser_feed(chunk_parser_t *p, const char *buf, size_t len);

int chunk_parser_done(const chunk_parser_t *p);
int chunk_parser_error(const chunk_parser_t *p);

#endif
