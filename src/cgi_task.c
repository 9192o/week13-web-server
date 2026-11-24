#include "cgi.h"

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cgi_job_t *cgi_job_create(const http_request_t *req, const config_t *cfg,
                          const char *remote_addr, int close_connection) {
    if (req == NULL || cfg == NULL)
        return NULL;

    cgi_job_t *job = calloc(1, sizeof *job);
    if (job == NULL)
        return NULL;

    job->cfg = cfg;
    job->method = req->method;
    job->version_major = req->version_major;
    job->version_minor = req->version_minor;
    job->close_connection = close_connection;

    snprintf(job->method_str, sizeof job->method_str, "%s", req->method_str);
    snprintf(job->path, sizeof job->path, "%s", req->path);
    snprintf(job->query, sizeof job->query, "%s", req->query);
    snprintf(job->remote_addr, sizeof job->remote_addr, "%s",
             remote_addr != NULL ? remote_addr : "");

    size_t ct_len = 0;
    const char *ct = http_request_get_header(req, "Content-Type", &ct_len);
    if (ct != NULL && ct_len < sizeof job->content_type) {
        memcpy(job->content_type, ct, ct_len);
        job->content_type[ct_len] = '\0';
    }

    if (req->body_len > 0) {
        job->body = malloc(req->body_len);
        if (job->body == NULL) {
            free(job);
            return NULL;
        }
        memcpy(job->body, req->body, req->body_len);
        job->body_len = req->body_len;
    }

    return job;
}

void cgi_job_free(cgi_job_t *job) {
    if (job == NULL)
        return;
    free(job->body);
    free(job);
}

static cgi_result_t *result_fail(cgi_result_t *res, FILE *tmp) {
    res->error = 1;
    free(res->response);
    res->response = NULL;
    res->response_len = 0;
    if (tmp != NULL)
        fclose(tmp);
    return res;
}

cgi_result_t *cgi_job_execute(const cgi_job_t *job) {
    if (job == NULL || job->cfg == NULL)
        return NULL;

    cgi_result_t *res = calloc(1, sizeof *res);
    if (res == NULL)
        return NULL;

    FILE *tmp = tmpfile();
    if (tmp == NULL)
        return result_fail(res, NULL);

    int fd = fileno(tmp);

    http_request_t req;
    http_request_init(&req);
    req.method = job->method;
    req.version_major = job->version_major;
    req.version_minor = job->version_minor;
    snprintf(req.method_str, sizeof req.method_str, "%s", job->method_str);
    snprintf(req.path, sizeof req.path, "%s", job->path);
    snprintf(req.query, sizeof req.query, "%s", job->query);

    req.content_length = job->body_len;
    req.has_content_length = job->body_len > 0;
    req.body = job->body;
    req.body_len = job->body_len;

    if (job->content_type[0] != '\0') {
        req.headers[0].name = "Content-Type";
        req.headers[0].name_len = strlen("Content-Type");
        req.headers[0].value = job->content_type;
        req.headers[0].value_len = strlen(job->content_type);
        req.header_count = 1;
    }

    handler_result_t rc = cgi_handle(fd, &req, job->cfg,
                                     job->remote_addr,
                                     job->close_connection);
    if (rc == HANDLER_ERROR)
        return result_fail(res, tmp);

    if (fflush(tmp) != 0 || fseek(tmp, 0, SEEK_END) != 0)
        return result_fail(res, tmp);

    long size = ftell(tmp);
    if (size <= 0 || fseek(tmp, 0, SEEK_SET) != 0)
        return result_fail(res, tmp);

    res->response = malloc((size_t)size);
    if (res->response == NULL)
        return result_fail(res, tmp);

    if (fread(res->response, 1, (size_t)size, tmp) != (size_t)size)
        return result_fail(res, tmp);

    res->response_len = (size_t)size;
    res->close_connection = (rc == HANDLER_CLOSE) ? 1 : 0;
    fclose(tmp);
    return res;
}

void cgi_result_free(cgi_result_t *result) {
    if (result == NULL)
        return;
    free(result->response);
    free(result);
}
