#include "chunked.h"

#include <ctype.h>
#include <string.h>

void chunk_parser_init(chunk_parser_t *p) {
    if (p == NULL)
        return;
    memset(p, 0, sizeof *p);
    p->state = CHUNK_SIZE;
}

static void parser_error(chunk_parser_t *p) {
    p->state = CHUNK_ERROR;
}

static int parse_size_line(chunk_parser_t *p) {
    size_t n = p->line_len;
    while (n > 0 && (p->line[n - 1] == '\r' || p->line[n - 1] == '\n'))
        n--;
    if (n == 0)
        return -1;

    size_t i = 0;
    unsigned long long value = 0;
    int digits = 0;
    while (i < n && isxdigit((unsigned char)p->line[i])) {
        int d;
        char c = p->line[i];
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            d = c - 'A' + 10;
        if (value > (~0ULL >> 4))
            return -1;
        value = (value << 4) | (unsigned long long)d;
        digits++;
        i++;
    }
    if (digits == 0)
        return -1;

    while (i < n && (p->line[i] == ' ' || p->line[i] == '\t'))
        i++;
    if (i < n && p->line[i] == ';') {
        /* chunk extension: ignore to end of line */
    } else if (i != n) {
        return -1;
    }

    if (value > (unsigned long long)(size_t)-1)
        return -1;

    p->remaining = (size_t)value;
    return 0;
}

size_t chunk_parser_feed(chunk_parser_t *p, const char *buf, size_t len) {
    if (p == NULL || buf == NULL || len == 0)
        return 0;
    if (p->state == CHUNK_DONE || p->state == CHUNK_ERROR)
        return 0;

    size_t used = 0;

    while (used < len) {
        if (p->state == CHUNK_SIZE) {
            char c = buf[used++];
            if (p->line_len >= sizeof p->line - 1) {
                parser_error(p);
                return used;
            }
            p->line[p->line_len++] = c;
            if (c == '\n') {
                if (parse_size_line(p) < 0) {
                    parser_error(p);
                    return used;
                }
                p->line_len = 0;
                p->crlf_state = 0;
                p->state = p->remaining == 0 ? CHUNK_TRAILER : CHUNK_DATA;
            }
            continue;
        }

        if (p->state == CHUNK_DATA) {
            size_t avail = len - used;
            size_t take = avail < p->remaining ? avail : p->remaining;
            used += take;
            p->remaining -= take;
            if (p->remaining == 0)
                p->state = CHUNK_DATA_CRLF;
            continue;
        }

        if (p->state == CHUNK_DATA_CRLF) {
            char c = buf[used++];
            if (p->crlf_state == 0) {
                if (c != '\r') {
                    parser_error(p);
                    return used;
                }
                p->crlf_state = 1;
            } else {
                if (c != '\n') {
                    parser_error(p);
                    return used;
                }
                p->crlf_state = 0;
                p->state = CHUNK_SIZE;
            }
            continue;
        }

        if (p->state == CHUNK_TRAILER) {
            char c = buf[used++];
            if (c == '\n') {
                if (p->trailer_line_len == 1 && p->trailer_prev_cr) {
                    p->state = CHUNK_DONE;
                    return used;
                }
                p->trailer_line_len = 0;
                p->trailer_prev_cr = 0;
            } else {
                p->trailer_line_len++;
                p->trailer_prev_cr = (c == '\r');
            }
            continue;
        }

        break;
    }

    return used;
}

int chunk_parser_done(const chunk_parser_t *p) {
    return p != NULL && p->state == CHUNK_DONE;
}

int chunk_parser_error(const chunk_parser_t *p) {
    return p != NULL && p->state == CHUNK_ERROR;
}
