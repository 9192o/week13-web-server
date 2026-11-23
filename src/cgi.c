#include "cgi.h"
#include "http.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CGI_MAX_OUTPUT (1024 * 1024)
#define CGI_PREFIX "/cgi-bin/"

extern char **environ;

static int path_within_root(const char *root, const char *resolved) {
    size_t root_len = strlen(root);
    if (strncmp(root, resolved, root_len) != 0)
        return 0;
    return resolved[root_len] == '\0' || resolved[root_len] == '/';
}

static int send_cgi_response(int connfd, char *output, size_t out_len,
                             int close_connection) {
    char *sep = strstr(output, "\r\n\r\n");
    size_t sep_len = 4;
    if (sep == NULL) {
        sep = strstr(output, "\n\n");
        sep_len = 2;
    }
    if (sep == NULL)
        return http_response_send_error(
            connfd, HTTP_INTERNAL_SERVER_ERROR, 1);

    *sep = '\0';
    char *body = sep + sep_len;
    size_t body_len = out_len - (size_t)(body - output);

    http_response_t res;
    http_response_init(&res);
    res.status = HTTP_OK;
    res.close_connection = close_connection;

    int has_content_type = 0;
    char *line = output;

    while (line != NULL && *line != '\0') {
        char *next = strstr(line, "\r\n");
        if (next != NULL) {
            *next = '\0';
            next += 2;
        } else {
            next = strchr(line, '\n');
            if (next != NULL) {
                *next = '\0';
                next += 1;
            }
        }

        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *name = line;
            char *value = colon + 1;

            while (*value == ' ' || *value == '\t')
                value++;

            char *end = value + strlen(value);
            while (end > value &&
                   (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
                *--end = '\0';

            if (strcasecmp(name, "Status") == 0) {
                int code = atoi(value);
                if (code >= 100 && code <= 599)
                    res.status = code;
            } else if (strcasecmp(name, "Content-Length") == 0 ||
                       strcasecmp(name, "Connection") == 0) {
                /* 서버가 직접 계산/설정한다. */
            } else {
                if (http_response_add_header(&res, name, value) < 0)
                    return http_response_send_error(
                        connfd, HTTP_INTERNAL_SERVER_ERROR, 1);
                if (strcasecmp(name, "Content-Type") == 0)
                    has_content_type = 1;
            }
        }

        line = next;
    }

    if (!has_content_type &&
        http_response_add_header(&res, "Content-Type", "text/plain") < 0)
        return http_response_send_error(
            connfd, HTTP_INTERNAL_SERVER_ERROR, 1);

    char lenbuf[32];
    int n = snprintf(lenbuf, sizeof lenbuf, "%zu", body_len);
    if (n < 0 || (size_t)n >= sizeof lenbuf)
        return http_response_send_error(
            connfd, HTTP_INTERNAL_SERVER_ERROR, 1);

    if (http_response_add_header(&res, "Content-Length", lenbuf) < 0 ||
        http_response_add_header(&res, "Connection",
                                 close_connection ? "close" : "keep-alive") < 0)
        return http_response_send_error(
            connfd, HTTP_INTERNAL_SERVER_ERROR, 1);

    if (http_response_send_headers(connfd, &res) < 0)
        return -1;
    if (body_len > 0 && writen(connfd, body, body_len) < 0)
        return -1;

    return 0;
}

static void set_cgi_env(const http_request_t *req, const config_t *cfg,
                        const char *remote_addr) {
    char buf[64];

    setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
    setenv("REQUEST_METHOD", req->method_str, 1);
    setenv("QUERY_STRING", req->query, 1);

    snprintf(buf, sizeof buf, "%zu", req->body_len);
    setenv("CONTENT_LENGTH", buf, 1);

    size_t ct_len = 0;
    const char *ct = http_request_get_header(req, "Content-Type", &ct_len);
    if (ct != NULL && ct_len < 512) {
        char ct_buf[512];
        memcpy(ct_buf, ct, ct_len);
        ct_buf[ct_len] = '\0';
        setenv("CONTENT_TYPE", ct_buf, 1);
    } else {
        setenv("CONTENT_TYPE", "", 1);
    }

    setenv("SCRIPT_NAME", req->path, 1);

    snprintf(buf, sizeof buf, "HTTP/%d.%d",
             req->version_major, req->version_minor);
    setenv("SERVER_PROTOCOL", buf, 1);

    snprintf(buf, sizeof buf, "%d", cfg->port);
    setenv("SERVER_PORT", buf, 1);
    setenv("REMOTE_ADDR", remote_addr != NULL ? remote_addr : "", 1);
}

static handler_result_t send_close_error(int connfd, int status) {
    if (http_response_send_error(connfd, status, 1) < 0)
        return HANDLER_ERROR;
    return HANDLER_CLOSE;
}

handler_result_t cgi_handle(int connfd, const http_request_t *req,
                            const config_t *cfg, const char *remote_addr,
                            int close_connection) {
    if (strncmp(req->path, CGI_PREFIX, strlen(CGI_PREFIX)) != 0)
        return send_close_error(connfd, HTTP_NOT_FOUND);

    const char *name = req->path + strlen(CGI_PREFIX);
    if (*name == '\0')
        return send_close_error(connfd, HTTP_NOT_FOUND);

    char cgi_root[PATH_MAX];
    if (realpath(cfg->cgi_dir, cgi_root) == NULL)
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);

    char script[PATH_MAX];
    int n = snprintf(script, sizeof script, "%s/%s", cfg->cgi_dir, name);
    if (n < 0 || (size_t)n >= sizeof script)
        return send_close_error(connfd, HTTP_URI_TOO_LONG);

    char resolved[PATH_MAX];
    if (realpath(script, resolved) == NULL)
        return send_close_error(connfd, HTTP_NOT_FOUND);

    if (!path_within_root(cgi_root, resolved))
        return send_close_error(connfd, HTTP_FORBIDDEN);

    if (access(resolved, X_OK) < 0)
        return send_close_error(connfd, HTTP_NOT_FOUND);

    int out_pipe[2];
    if (pipe(out_pipe) < 0)
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);

    FILE *body_tmp = NULL;
    int in_fd = -1;
    if (req->body_len > 0) {
        body_tmp = tmpfile();
        if (body_tmp == NULL) {
            close(out_pipe[0]);
            close(out_pipe[1]);
            return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);
        }
        if (fwrite(req->body, 1, req->body_len, body_tmp) != req->body_len ||
            fflush(body_tmp) != 0) {
            fclose(body_tmp);
            close(out_pipe[0]);
            close(out_pipe[1]);
            return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);
        }
        rewind(body_tmp);
        in_fd = fileno(body_tmp);
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (body_tmp != NULL)
            fclose(body_tmp);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);
    }

    if (pid == 0) {
        if (in_fd >= 0) {
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }

        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(connfd);

        set_cgi_env(req, cfg, remote_addr);

        char *argv[] = { resolved, NULL };
        execve(resolved, argv, environ);
        _exit(127);
    }

    close(out_pipe[1]);
    if (body_tmp != NULL)
        fclose(body_tmp);

    char *output = malloc(CGI_MAX_OUTPUT + 1);
    if (output == NULL) {
        close(out_pipe[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);
    }

    size_t out_len = 0;
    int too_large = 0;
    int timed_out = 0;

    while (1) {
        struct pollfd pfd;
        pfd.fd = out_pipe[0];
        pfd.events = POLLIN;
        pfd.revents = 0;

        int timeout_ms = cfg->request_timeout_sec * 1000;
        if (timeout_ms <= 0)
            timeout_ms = 1000;

        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            timed_out = 1;
            break;
        }
        if (pfd.revents & (POLLERR | POLLNVAL))
            break;
        if (!(pfd.revents & POLLIN))
            break;

        char buf[4096];
        ssize_t r = read(out_pipe[0], buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            break;
        if (out_len + (size_t)r > CGI_MAX_OUTPUT) {
            too_large = 1;
            break;
        }
        memcpy(output + out_len, buf, (size_t)r);
        out_len += (size_t)r;
    }

    close(out_pipe[0]);

    if (too_large || timed_out)
        kill(pid, SIGTERM);

    int status = 0;
    waitpid(pid, &status, 0);

    if (timed_out) {
        free(output);
        return send_close_error(connfd, HTTP_GATEWAY_TIMEOUT);
    }

    if (too_large || out_len == 0) {
        free(output);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR);
    }

    output[out_len] = '\0';
    int rc = send_cgi_response(connfd, output, out_len, close_connection);
    free(output);
    if (rc < 0)
        return HANDLER_ERROR;
    return close_connection ? HANDLER_CLOSE : HANDLER_OK;
}
