#ifndef APP_H
#define APP_H

#include "http.h"
#include "router.h"
#include "session_app.h"

struct cgi_pool;
struct web_config {
    int root_fd;
    struct cgi_pool *cgi;
};
struct session_app web_app(const struct web_config *config);

struct router_result app_match_request(const struct http_request *request);

/*
 * Static responses/errors are prepared from head before body; dynamic handlers
 * run only after the body is complete.
 * Response borrows request and caller's location storage through transmission.
 * root_fd is an O_RDONLY | O_DIRECTORY file descriptor pinned by main.
 * Caller owns returned file_fd (if >= 0) and must close it on every path.
 */
struct http_response app_handle_request(int root_fd,
                                        const struct http_request *request,
                                        const struct router_result *route,
                                        char location[HTTP_REQUEST_LINE_MAX + 2]);

#endif
