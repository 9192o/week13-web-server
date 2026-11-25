#ifndef CGI_H
#define CGI_H

#include "session_app.h"

struct cgi_pool;
struct cgi_pool *cgi_pool_open(const char *root);
void cgi_pool_close(struct cgi_pool *pool);
bool cgi_route(const struct http_request *request);
struct session_stream *cgi_submit(struct cgi_pool *pool,
                                  const struct http_request *request,
                                  int *error);
extern const struct session_stream_ops cgi_ops;

#endif
