#include "static_file.h"
#include "static_file_internal.h"
#include "http.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATIC_CHUNK 65536

/* Exposed for unit tests: returns -1 if the file ends before `size`. */
int send_file(int connfd, int filefd, size_t size) {
    char buf[STATIC_CHUNK];

    while (size > 0) {
        size_t want = size < sizeof buf ? size : sizeof buf;
        ssize_t n = read(filefd, buf, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1; /* file shrank after fstat: Content-Length already sent */
        if (writen(connfd, buf, (size_t)n) < 0)
            return -1;
        size -= (size_t)n;
    }

    return 0;
}

static int has_dotfile_segment(const char *path) {
    const char *p = path;

    while (*p != '\0') {
        while (*p == '/')
            p++;

        const char *start = p;
        while (*p != '\0' && *p != '/')
            p++;

        if (p > start && start[0] == '.')
            return 1;
    }

    return 0;
}

static int path_within_root(const char *root, const char *resolved) {
    size_t root_len = strlen(root);

    if (strncmp(root, resolved, root_len) != 0)
        return 0;

    return resolved[root_len] == '\0' || resolved[root_len] == '/';
}

static int status_from_errno(int err) {
    if (err == ENOENT || err == ENOTDIR)
        return HTTP_NOT_FOUND;
    if (err == EACCES)
        return HTTP_FORBIDDEN;
    return HTTP_INTERNAL_SERVER_ERROR;
}

static handler_result_t send_close_error(int connfd, int status) {
    if (http_response_send_error(connfd, status, 1) < 0)
        return HANDLER_ERROR;
    return HANDLER_CLOSE;
}

static handler_result_t send_error_for_errno(int connfd, int err) {
    return send_close_error(connfd, status_from_errno(err));
}

typedef struct {
    size_t start;
    size_t end;
} byte_range_t;

static int parse_size_value(const char *s, size_t len, size_t *out) {
    if (len == 0 || len >= 32)
        return -1;

    char buf[32];
    memcpy(buf, s, len);
    buf[len] = '\0';

    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0')
        return -1;

    *out = (size_t)value;
    return 0;
}

static int parse_range_header(const char *value, size_t value_len,
                              size_t file_size, byte_range_t *range) {
    if (file_size == 0 || value_len < 7 ||
        strncasecmp(value, "bytes=", 6) != 0)
        return -1;

    const char *p = value + 6;
    const char *end = value + value_len;

    if (memchr(p, ',', (size_t)(end - p)) != NULL)
        return -1; /* multipart ranges 미지원 */

    const char *dash = memchr(p, '-', (size_t)(end - p));
    if (dash == NULL)
        return -1;

    size_t start_len = (size_t)(dash - p);
    size_t end_len = (size_t)(end - dash - 1);

    if (start_len == 0) {
        size_t suffix = 0;
        if (end_len == 0 || parse_size_value(dash + 1, end_len, &suffix) < 0)
            return -1;
        if (suffix == 0)
            return -1;
        if (suffix > file_size)
            suffix = file_size;
        range->start = file_size - suffix;
        range->end = file_size - 1;
        return 0;
    }

    size_t start = 0;
    if (parse_size_value(p, start_len, &start) < 0 || start >= file_size)
        return -1;

    size_t last = file_size - 1;
    if (end_len > 0) {
        if (parse_size_value(dash + 1, end_len, &last) < 0)
            return -1;
        if (last >= file_size)
            last = file_size - 1;
    }

    if (last < start)
        return -1;

    range->start = start;
    range->end = last;
    return 0;
}

static int send_range_error(int connfd, size_t file_size,
                            int close_connection) {
    http_response_t res;
    http_response_init(&res);
    res.status = 416;
    res.close_connection = close_connection;

    char content_range[64];
    int n = snprintf(content_range, sizeof content_range,
                     "bytes */%zu", file_size);
    if (n < 0 || (size_t)n >= sizeof content_range)
        return -1;

    if (http_response_add_header(&res, "Content-Range", content_range) < 0 ||
        http_response_add_header(&res, "Content-Length", "0") < 0 ||
        http_response_add_header(&res, "Accept-Ranges", "bytes") < 0 ||
        http_response_add_header(&res, "Connection",
                                 close_connection ? "close" : "keep-alive") < 0)
        return -1;

    return http_response_send_headers(connfd, &res);
}

handler_result_t static_file_handle(int connfd, const http_request_t *req,
                                    const config_t *cfg, int close_connection) {
    if (req->method != HTTP_GET && req->method != HTTP_HEAD)
        return send_close_error(connfd, HTTP_METHOD_NOT_ALLOWED);

    if (has_dotfile_segment(req->path))
        return send_close_error(connfd, HTTP_NOT_FOUND);

    char root[PATH_MAX];
    if (realpath(cfg->docroot, root) == NULL)
        return send_error_for_errno(connfd, errno);

    const char *path = req->path;
    if (strcmp(path, "/") == 0)
        path = "/index.html";

    char filepath[PATH_MAX];
    int n = snprintf(filepath, sizeof filepath, "%s%s", cfg->docroot, path);
    if (n < 0 || (size_t)n >= sizeof filepath)
        return send_close_error(connfd, HTTP_URI_TOO_LONG);

    struct stat st;
    if (stat(filepath, &st) < 0)
        return send_error_for_errno(connfd, errno);

    if (S_ISDIR(st.st_mode)) {
        size_t path_len = strlen(req->path);

        if (path_len == 0 || req->path[path_len - 1] != '/') {
            char location[HTTP_MAX_URI * 2];
            int m;
            if (req->query[0] != '\0')
                m = snprintf(location, sizeof location, "%s/?%s",
                             req->path, req->query);
            else
                m = snprintf(location, sizeof location, "%s/", req->path);

            if (m < 0 || (size_t)m >= sizeof location)
                return send_close_error(connfd, HTTP_URI_TOO_LONG);

            if (http_response_send_redirect(connfd, location,
                                            close_connection) < 0)
                return HANDLER_ERROR;
            return close_connection ? HANDLER_CLOSE : HANDLER_OK;
        }

        n = snprintf(filepath, sizeof filepath, "%s%sindex.html",
                     cfg->docroot, req->path);
        if (n < 0 || (size_t)n >= sizeof filepath)
            return send_close_error(connfd, HTTP_URI_TOO_LONG);

        if (stat(filepath, &st) < 0)
            return send_error_for_errno(connfd, errno);
        if (!S_ISREG(st.st_mode))
            return send_close_error(connfd, HTTP_NOT_FOUND);
    } else if (!S_ISREG(st.st_mode)) {
        return send_close_error(connfd, HTTP_NOT_FOUND);
    }

    char resolved[PATH_MAX];
    if (realpath(filepath, resolved) == NULL)
        return send_error_for_errno(connfd, errno);

    if (!path_within_root(root, resolved))
        return send_close_error(connfd, HTTP_FORBIDDEN);
    if (has_dotfile_segment(resolved))
        return send_close_error(connfd, HTTP_NOT_FOUND);

    int filefd = open(resolved, O_RDONLY);
    if (filefd < 0)
        return send_error_for_errno(connfd, errno);

    if (fstat(filefd, &st) < 0) {
        close(filefd);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);
    }
    if (!S_ISREG(st.st_mode)) {
        close(filefd);
        return send_close_error(connfd, HTTP_NOT_FOUND);
    }

    char last_modified[64];
    if (http_date_format(st.st_mtime, last_modified,
                         sizeof last_modified) < 0) {
        close(filefd);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);
    }

    size_t ims_len = 0;
    const char *ims = http_request_get_header(req, "If-Modified-Since",
                                              &ims_len);
    if (ims != NULL && ims_len < 128) {
        char ims_buf[128];
        memcpy(ims_buf, ims, ims_len);
        ims_buf[ims_len] = '\0';

        time_t ims_time;
        if (http_date_parse(ims_buf, &ims_time) == 0 &&
            ims_time >= st.st_mtime) {
            close(filefd);

            http_response_t not_modified;
            http_response_init(&not_modified);
            not_modified.status = 304;
            not_modified.close_connection = close_connection;

            if (http_response_add_header(&not_modified, "Last-Modified",
                                         last_modified) < 0 ||
                http_response_add_header(
                    &not_modified, "Connection",
                    close_connection ? "close" : "keep-alive") < 0)
                return send_close_error(
                    connfd, HTTP_INTERNAL_SERVER_ERROR);

            if (http_response_send_headers(connfd, &not_modified) < 0)
                return HANDLER_ERROR;
            return close_connection ? HANDLER_CLOSE : HANDLER_OK;
        }
    }

    size_t file_size = (size_t)st.st_size;
    byte_range_t range = {0, 0};
    int has_range = 0;

    size_t range_value_len = 0;
    const char *range_value = http_request_get_header(req, "Range",
                                                      &range_value_len);
    if (range_value != NULL) {
        if (range_value_len >= 128) {
            close(filefd);
            if (send_range_error(connfd, file_size, close_connection) < 0)
                return HANDLER_ERROR;
            return close_connection ? HANDLER_CLOSE : HANDLER_OK;
        }

        char range_buf[128];
        memcpy(range_buf, range_value, range_value_len);
        range_buf[range_value_len] = '\0';

        if (parse_range_header(range_buf, range_value_len,
                               file_size, &range) < 0) {
            close(filefd);
            if (send_range_error(connfd, file_size, close_connection) < 0)
                return HANDLER_ERROR;
            return close_connection ? HANDLER_CLOSE : HANDLER_OK;
        }
        has_range = 1;
    }

    size_t content_len = has_range
                             ? range.end - range.start + 1
                             : file_size;

    http_response_t res;
    http_response_init(&res);
    res.status = has_range ? 206 : 200;
    res.close_connection = close_connection;

    char lenbuf[32];
    int m = snprintf(lenbuf, sizeof lenbuf, "%zu", content_len);
    if (m < 0 || (size_t)m >= sizeof lenbuf) {
        close(filefd);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);
    }

    char content_range[64];
    content_range[0] = '\0';
    if (has_range) {
        m = snprintf(content_range, sizeof content_range,
                     "bytes %zu-%zu/%zu",
                     range.start, range.end, file_size);
        if (m < 0 || (size_t)m >= sizeof content_range) {
            close(filefd);
            return send_close_error(
                connfd, HTTP_INTERNAL_SERVER_ERROR);
        }
    }

    if (http_response_add_header(&res, "Content-Type",
                                 mime_type(resolved)) < 0 ||
        http_response_add_header(&res, "Content-Length", lenbuf) < 0 ||
        (has_range &&
         http_response_add_header(&res, "Content-Range", content_range) < 0) ||
        http_response_add_header(&res, "Accept-Ranges", "bytes") < 0 ||
        http_response_add_header(&res, "Last-Modified", last_modified) < 0 ||
        http_response_add_header(&res, "Connection",
                                 close_connection ? "close" : "keep-alive") < 0) {
        close(filefd);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);
    }

    if (http_response_send_headers(connfd, &res) < 0) {
        close(filefd);
        return HANDLER_ERROR;
    }

    if (req->method == HTTP_GET) {
        if (has_range &&
            lseek(filefd, (off_t)range.start, SEEK_SET) < 0) {
            close(filefd);
            return HANDLER_ERROR;
        }
        if (send_file(connfd, filefd, content_len) < 0) {
            close(filefd);
            return HANDLER_ERROR;
        }
    }

    close(filefd);
    return HANDLER_OK;
}
