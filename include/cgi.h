#ifndef CGI_H
#define CGI_H

#include "config.h"
#include "http.h"

handler_result_t cgi_handle(int connfd, const http_request_t *req,
                            const config_t *cfg, const char *remote_addr,
                            int close_connection);

typedef struct {
    const config_t *cfg;
    http_method_t method;
    char method_str[HTTP_MAX_METHOD];
    char path[HTTP_MAX_URI];
    char query[HTTP_MAX_URI];
    int version_major;
    int version_minor;
    char content_type[512];
    char *body;
    size_t body_len;
    int close_connection;
    char remote_addr[64];
} cgi_job_t;

typedef struct {
    char *response;
    size_t response_len;
    int close_connection;
    int error;
} cgi_result_t;

cgi_job_t *cgi_job_create(const http_request_t *req, const config_t *cfg,
                          const char *remote_addr, int close_connection);
void cgi_job_free(cgi_job_t *job);
cgi_result_t *cgi_job_execute(const cgi_job_t *job);
void cgi_result_free(cgi_result_t *result);

#endif
