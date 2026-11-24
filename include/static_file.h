#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include "config.h"
#include "http.h"

#include <sys/types.h>

handler_result_t static_file_handle(int fd, const http_request_t *req,
                                    const config_t *cfg, int close_connection);

typedef struct {
    http_response_t res;
    int file_fd;
    off_t file_offset;
    size_t content_length;
    int head_only;
    char body[512];
    size_t body_len;
} static_result_t;

void static_result_init(static_result_t *r);
handler_result_t static_file_prepare(const http_request_t *req,
                                     const config_t *cfg,
                                     int close_connection,
                                     static_result_t *out);

#endif
