#ifndef STATIC_RESPONSE_H
#define STATIC_RESPONSE_H

#include "http.h"

/* location storage survives through response serialization. */
struct http_response static_response(int root_fd, const struct http_request *request,
                                     const char *decoded_path, char location[HTTP_REQUEST_LINE_MAX + 2]);

#endif
