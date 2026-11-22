#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include "config.h"
#include "http.h"

int static_file_handle(int fd, const http_request_t *req, const config_t *cfg,
                       int close_connection);

#endif
