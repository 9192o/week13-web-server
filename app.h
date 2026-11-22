#ifndef APP_H
#define APP_H

#include "http.h"
#include "router.h"

/*
 * Static responses/errors are prepared from head before body; echo after body.
 * Response borrows request and caller's location storage through transmission.
 * root_fd is an O_RDONLY | O_DIRECTORY file descriptor pinned by main.
 * Caller owns returned file_fd (if >= 0) and must close it on every path.
 */
struct http_response app_handle_request(int root_fd,
                                        const struct http_request *request,
                                        const struct router_result *route,
                                        char location[HTTP_REQUEST_LINE_MAX + 2]);

#endif
