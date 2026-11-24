#include "cgi.h"
#include "http.h"
#include "util.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
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

/*
 * macOS keeps the signal mask per process rather than per thread, and fork()
 * blocks signals around the fork and restores them afterwards. Concurrent
 * forks from task-pool workers can therefore race and leave SIGINT/SIGTERM
 * blocked for the whole server. Serialize fork so each block/restore pair
 * completes before the next one starts.
 */
static pthread_mutex_t g_fork_mutex = PTHREAD_MUTEX_INITIALIZER;

static int path_within_root(const char *root, const char *resolved) {
    size_t root_len = strlen(root);
    if (strncmp(root, resolved, root_len) != 0)
        return 0;
    return resolved[root_len] == '\0' || resolved[root_len] == '/';
}

static int cgi_header_value_ok(const char *value) {
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '\t')
            continue;
        if (*p < 0x20 || *p == 0x7f)
            return 0;
    }
    return 1;
}

static int send_cgi_response(int connfd, char *output, size_t out_len,
                             int close_connection, int head) {
    char *sep = strstr(output, "\r\n\r\n");
    size_t sep_len = 4;
    if (sep == NULL) {
        sep = strstr(output, "\n\n");
        sep_len = 2;
    }
    if (sep == NULL)
        return http_response_send_error_ex(connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);

    *sep = '\0';
    char *body = sep + sep_len;
    size_t body_len = out_len - (size_t)(body - output);

    http_response_t res;
    http_response_init(&res);
    res.status = HTTP_OK;
    res.close_connection = close_connection;

    int has_content_type = 0;
    int status_seen = 0;
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
                if (status_seen)
                    return http_response_send_error_ex(
                        connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);
                status_seen = 1;

                if (strlen(value) < 3 ||
                    !isdigit((unsigned char)value[0]) ||
                    !isdigit((unsigned char)value[1]) ||
                    !isdigit((unsigned char)value[2]) ||
                    (value[3] != '\0' && value[3] != ' '))
                    return http_response_send_error_ex(
                        connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);

                int code = atoi(value);
                if (code < 200 || code > 599)
                    return http_response_send_error_ex(
                        connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);
                res.status = code;
            } else if (strcasecmp(name, "Content-Length") == 0 ||
                       strcasecmp(name, "Connection") == 0) {
                /* 서버가 직접 계산/설정한다. */
            } else if (strcasecmp(name, "Content-Type") == 0) {
                if (has_content_type || value[0] == '\0' ||
                    !cgi_header_value_ok(value))
                    return http_response_send_error_ex(
                        connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);
                if (http_response_add_header(&res, name, value) < 0)
                    return http_response_send_error_ex(
                        connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);
                has_content_type = 1;
            } else {
                if (!cgi_header_value_ok(value) ||
                    http_response_add_header(&res, name, value) < 0)
                    return http_response_send_error_ex(
                        connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);
            }
        }

        line = next;
    }

    if (body_len > 0 &&
        (res.status == 204 || res.status == 205 || res.status == 304))
        return http_response_send_error_ex(
            connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);

    int no_body_status = (res.status == 204 || res.status == 304);
    int bodyless_status = (res.status == 204 || res.status == 205 ||
                           res.status == 304);

    if (!has_content_type && !bodyless_status &&
        http_response_add_header(&res, "Content-Type", "text/plain") < 0)
        return http_response_send_error_ex(connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);

    char lenbuf[32];
    int n = snprintf(lenbuf, sizeof lenbuf, "%zu", body_len);
    if (n < 0 || (size_t)n >= sizeof lenbuf)
        return http_response_send_error_ex(connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);

    if (!no_body_status &&
        http_response_add_header(&res, "Content-Length", lenbuf) < 0)
        return http_response_send_error_ex(connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);
    if (http_response_add_header(&res, "Connection",
                                 close_connection ? "close" : "keep-alive") < 0)
        return http_response_send_error_ex(connfd, HTTP_INTERNAL_SERVER_ERROR, 1, head);

    if (http_response_send_headers(connfd, &res) < 0)
        return -1;
    if (body_len > 0 && !head && write_body(connfd, body, body_len) < 0)
        return -1;

    return 0;
}

#define CGI_ENV_MAX 16

static void free_env(char **env) {
    if (env == NULL)
        return;
    for (size_t i = 0; env[i] != NULL; i++)
        free(env[i]);
    free(env);
}

static int env_add(char **env, size_t *n, const char *name,
                   const char *value) {
    if (*n + 1 >= CGI_ENV_MAX)
        return -1;

    size_t len = strlen(name) + strlen(value) + 2;
    char *s = malloc(len);
    if (s == NULL)
        return -1;

    snprintf(s, len, "%s=%s", name, value);
    env[(*n)++] = s;
    env[*n] = NULL;
    return 0;
}

static char **build_cgi_env(const http_request_t *req, const config_t *cfg,
                            const char *remote_addr) {
    char **env = calloc(CGI_ENV_MAX, sizeof(char *));
    if (env == NULL)
        return NULL;

    size_t n = 0;
    char buf[64];

    if (env_add(env, &n, "GATEWAY_INTERFACE", "CGI/1.1") < 0 ||
        env_add(env, &n, "REQUEST_METHOD", req->method_str) < 0 ||
        env_add(env, &n, "QUERY_STRING", req->query) < 0)
        goto fail;

    snprintf(buf, sizeof buf, "%zu", req->body_len);
    if (env_add(env, &n, "CONTENT_LENGTH", buf) < 0)
        goto fail;

    size_t ct_len = 0;
    const char *ct = http_request_get_header(req, "Content-Type", &ct_len);
    char ct_buf[512];
    if (ct != NULL && ct_len < sizeof ct_buf) {
        memcpy(ct_buf, ct, ct_len);
        ct_buf[ct_len] = '\0';
    } else {
        ct_buf[0] = '\0';
    }
    if (env_add(env, &n, "CONTENT_TYPE", ct_buf) < 0)
        goto fail;

    if (env_add(env, &n, "SCRIPT_NAME", req->path) < 0)
        goto fail;

    snprintf(buf, sizeof buf, "HTTP/%d.%d",
             req->version_major, req->version_minor);
    if (env_add(env, &n, "SERVER_PROTOCOL", buf) < 0)
        goto fail;

    snprintf(buf, sizeof buf, "%d", cfg->port);
    if (env_add(env, &n, "SERVER_PORT", buf) < 0)
        goto fail;

    if (env_add(env, &n, "REMOTE_ADDR",
                remote_addr != NULL ? remote_addr : "") < 0)
        goto fail;

    /* Do not leak the server's environment to CGI programs. */
    if (env_add(env, &n, "PATH", "/usr/bin:/bin") < 0)
        goto fail;

    return env;

fail:
    free_env(env);
    return NULL;
}

static handler_result_t send_close_error(int connfd, int status, int head) {
    if (http_response_send_error_ex(connfd, status, 1, head) < 0)
        return HANDLER_ERROR;
    return HANDLER_CLOSE;
}

handler_result_t cgi_handle(int connfd, const http_request_t *req,
                            const config_t *cfg, const char *remote_addr,
                            int close_connection) {
    int head = (req->method == HTTP_HEAD);

    if (strncmp(req->path, CGI_PREFIX, strlen(CGI_PREFIX)) != 0)
        return send_close_error(connfd, HTTP_NOT_FOUND, head);

    const char *name = req->path + strlen(CGI_PREFIX);
    if (*name == '\0')
        return send_close_error(connfd, HTTP_NOT_FOUND, head);

    char cgi_root[PATH_MAX];
    if (realpath(cfg->cgi_dir, cgi_root) == NULL)
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR, head);

    char script[PATH_MAX];
    int n = snprintf(script, sizeof script, "%s/%s", cfg->cgi_dir, name);
    if (n < 0 || (size_t)n >= sizeof script)
        return send_close_error(connfd, HTTP_URI_TOO_LONG, head);

    char resolved[PATH_MAX];
    if (realpath(script, resolved) == NULL)
        return send_close_error(connfd, HTTP_NOT_FOUND, head);

    if (!path_within_root(cgi_root, resolved))
        return send_close_error(connfd, HTTP_FORBIDDEN, head);

    int script_flags = O_RDONLY;
#ifdef O_CLOEXEC
    script_flags |= O_CLOEXEC;
#endif
    int script_fd = open(resolved, script_flags);
    if (script_fd < 0)
        return send_close_error(connfd, HTTP_NOT_FOUND, head);

#ifndef O_CLOEXEC
    (void)fcntl(script_fd, F_SETFD, FD_CLOEXEC);
#endif

    struct stat st;
    if (fstat(script_fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        close(script_fd);
        return send_close_error(connfd, HTTP_NOT_FOUND, head);
    }

    char **envp = build_cgi_env(req, cfg, remote_addr);
    if (envp == NULL) {
        close(script_fd);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR, head);
    }

    int out_pipe[2];
    if (pipe(out_pipe) < 0) {
        close(script_fd);
        free_env(envp);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR, head);
    }

    FILE *body_tmp = NULL;
    int in_fd = -1;
    if (req->body_len > 0) {
        body_tmp = tmpfile();
        if (body_tmp == NULL) {
            close(script_fd);
            free_env(envp);
            close(out_pipe[0]);
            close(out_pipe[1]);
            return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR, head);
        }
        if (fwrite(req->body, 1, req->body_len, body_tmp) != req->body_len ||
            fflush(body_tmp) != 0) {
            close(script_fd);
            free_env(envp);
            fclose(body_tmp);
            close(out_pipe[0]);
            close(out_pipe[1]);
            return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR, head);
        }
        rewind(body_tmp);
        in_fd = fileno(body_tmp);
    }

    char *argv[] = { resolved, NULL };
    pthread_mutex_lock(&g_fork_mutex);
    pid_t pid = fork();
    int fork_errno = errno;
    pthread_mutex_unlock(&g_fork_mutex);
    if (pid < 0) {
        errno = fork_errno;
        close(script_fd);
        free_env(envp);
        if (body_tmp != NULL)
            fclose(body_tmp);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR, head);
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

#if defined(__linux__)
        fexecve(script_fd, argv, envp);
#else
        close(script_fd);
        execve(resolved, argv, envp);
#endif
        _exit(127);
    }

    close(script_fd);
    free_env(envp);

    close(out_pipe[1]);
    if (body_tmp != NULL)
        fclose(body_tmp);

    char *output = malloc(CGI_MAX_OUTPUT + 1);
    if (output == NULL) {
        close(out_pipe[0]);
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR, head);
    }

    size_t out_len = 0;
    int too_large = 0;
    int timed_out = 0;

    int timeout_ms = cfg->request_timeout_sec * 1000;
    if (timeout_ms <= 0)
        timeout_ms = 1000;
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;

    while (1) {
        uint64_t now = now_ms();
        if (now >= deadline) {
            timed_out = 1;
            break;
        }

        struct pollfd pfd;
        pfd.fd = out_pipe[0];
        pfd.events = POLLIN;
        pfd.revents = 0;

        uint64_t remaining = deadline - now;
        int wait_ms = remaining > INT_MAX ? INT_MAX : (int)remaining;

        int pr = poll(&pfd, 1, wait_ms);
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
        kill(pid, SIGKILL);

    int status = 0;
    pid_t reaped = -1;
    while ((reaped = waitpid(pid, &status, 0)) < 0) {
        if (errno == EINTR)
            continue;
        break;
    }

    if (timed_out) {
        free(output);
        return send_close_error(connfd, HTTP_GATEWAY_TIMEOUT, head);
    }

    if (too_large || out_len == 0 || reaped < 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(output);
        return send_close_error(connfd, HTTP_INTERNAL_SERVER_ERROR, head);
    }

    output[out_len] = '\0';
    int rc = send_cgi_response(connfd, output, out_len, close_connection, head);
    free(output);
    if (rc < 0)
        return HANDLER_ERROR;
    return close_connection ? HANDLER_CLOSE : HANDLER_OK;
}
