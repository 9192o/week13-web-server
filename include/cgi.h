#ifndef CGI_H
#define CGI_H

#include "config.h"
#include "http.h"

handler_result_t cgi_handle(int connfd, const http_request_t *req,
                            const config_t *cfg, const char *remote_addr,
                            int close_connection);

#endif
