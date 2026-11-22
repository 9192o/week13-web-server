#ifndef APP_H
#define APP_H

#include "http.h"
#include "router.h"

/*
 * INDEX/errors are prepared from head before body reception; ECHO after body.
 * Response bytes borrow request/static storage through transmission.
 * root_fd is an O_RDONLY | O_DIRECTORY file descriptor pinned by main.
 * Caller owns returned file_fd (if >= 0) and must close it on every path.
 */
struct http_response app_handle_request(int root_fd,
                                        const struct http_request *request,
                                        struct router_result route);

#endif
