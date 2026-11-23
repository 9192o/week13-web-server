#include "cgi.h"
#include "http_response.h"
#include "server.h"
#include "url.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CGI_ENV_MAX 20

/* 파일 시스템 오류 → 상태 코드 (정적 서빙과 같은 규칙) */
static int errno_status(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        return 404;
    case EACCES:
        return 403;
    default:
        return 500;
    }
}

int cgi_resolve(const char *root, const char *decoded, char *out_path)
{
    const char *name = decoded + strlen(URL_CGI_PREFIX);
    char dir[PATH_MAX], realdir[PATH_MAX], cand[PATH_MAX];
    struct stat st;
    size_t dl;
    int n;

    /* "/cgi-bin/<이름>" 한 단계만: "/cgi-bin", "/cgi-bin/", 하위 경로는 404 */
    if (name[0] != '/' || name[1] == '\0' || strchr(name + 1, '/') != NULL)
        return 404;

    n = snprintf(dir, sizeof dir, "%s%s", root, URL_CGI_PREFIX);
    if (n < 0 || (size_t)n >= sizeof dir)
        return 500;
    if (realpath(dir, realdir) == NULL)
        return errno_status(errno);

    n = snprintf(cand, sizeof cand, "%s%s", root, decoded);
    if (n < 0 || (size_t)n >= sizeof cand)
        return 500;
    if (realpath(cand, out_path) == NULL)
        return errno_status(errno);

    dl = strlen(realdir);
    if (strncmp(out_path, realdir, dl) != 0 || out_path[dl] != '/')
        return 403;                       /* 심볼릭 링크가 cgi-bin 밖을 가리킴 */
    if (stat(out_path, &st) < 0)
        return errno_status(errno);
    if (!S_ISREG(st.st_mode))
        return 403;
    if (access(out_path, X_OK) < 0)
        return errno == EACCES ? 403 : errno_status(errno);
    return 0;
}

static int is_ows(char c)
{
    return c == ' ' || c == '\t';
}

int cgi_parse_output(const char *out, size_t len, int *status,
                     char *content_type, size_t ct_cap, size_t *body_off)
{
    size_t pos = 0;
    int have_ct = 0, have_status = 0;

    *status = 200;
    for (;;) {
        const char *line = out + pos;
        const char *nl = memchr(line, '\n', len - pos);
        const char *colon, *v, *ve;
        size_t line_len, name_len, vlen, i;

        if (nl == NULL)
            return -1;                    /* 헤더 끝 빈 줄이 없음 */
        line_len = (size_t)(nl - line);
        if (line_len > 0 && line[line_len - 1] == '\r')
            line_len--;
        if (line_len == 0) {              /* 빈 줄: 헤더 끝 */
            *body_off = (size_t)(nl - out) + 1;
            break;
        }

        colon = memchr(line, ':', line_len);
        if (colon == NULL || colon == line)
            return -1;
        name_len = (size_t)(colon - line);
        for (i = 0; i < name_len; i++)
            if (is_ows(line[i]))
                return -1;

        v = colon + 1;
        ve = line + line_len;
        while (v < ve && is_ows(*v))
            v++;
        while (ve > v && is_ows(ve[-1]))
            ve--;
        vlen = (size_t)(ve - v);
        for (i = 0; i < vlen; i++) {      /* CR 등 제어 문자로 헤더를 끼워 넣지 못하게 */
            unsigned char c = (unsigned char)v[i];
            if ((c < 0x20 && c != '\t') || c == 0x7f)
                return -1;
        }

        if (name_len == 12 && strncasecmp(line, "Content-Type", 12) == 0) {
            if (have_ct || vlen == 0 || vlen > CGI_CONTENT_TYPE_MAX || vlen >= ct_cap)
                return -1;
            memcpy(content_type, v, vlen);
            content_type[vlen] = '\0';
            have_ct = 1;
        } else if (name_len == 6 && strncasecmp(line, "Status", 6) == 0) {
            int code;
            if (have_status || vlen < 3)
                return -1;
            for (i = 0; i < 3; i++)
                if (v[i] < '0' || v[i] > '9')
                    return -1;
            if (vlen > 3 && !is_ows(v[3]))
                return -1;                /* "2000" 처럼 네 자리 이상 */
            code = (v[0] - '0') * 100 + (v[1] - '0') * 10 + (v[2] - '0');
            /* 1xx 는 최종 응답이 될 수 없고, 204/205/304 는 본문을 가질 수 없어 미지원 */
            if (code < 200 || code > 599 || code == 204 || code == 205 || code == 304)
                return -1;
            *status = code;               /* CGI 가 보낸 문구는 무시 */
            have_status = 1;
        }
        /* 그 외 헤더는 버린다 */
        pos = (size_t)(nl - out) + 1;
    }
    return have_ct ? 0 : -1;
}

static int env_add(char **env, int *n, const char *name, const char *value)
{
    size_t len = strlen(name) + strlen(value) + 2;
    char *s;

    if (*n >= CGI_ENV_MAX)
        return -1;
    s = malloc(len);
    if (s == NULL)
        return -1;
    snprintf(s, len, "%s=%s", name, value);
    env[(*n)++] = s;
    env[*n] = NULL;
    return 0;
}

static int env_add_num(char **env, int *n, const char *name, long long value)
{
    char num[32];
    snprintf(num, sizeof num, "%lld", value);
    return env_add(env, n, name, num);
}

static void env_free(char **env, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(env[i]);
}

/* 서버의 환경은 물려주지 않고 CGI/1.1 메타 변수만 새로 만든다 */
static int build_env(const struct cgi_request *req, char **env, int *n)
{
    *n = 0;
    env[0] = NULL;
    if (env_add(env, n, "GATEWAY_INTERFACE", "CGI/1.1") < 0 ||
        env_add(env, n, "SERVER_PROTOCOL", req->protocol) < 0 ||
        env_add(env, n, "SERVER_SOFTWARE", HTTP_SERVER_NAME) < 0 ||
        env_add(env, n, "SERVER_NAME", req->server_name) < 0 ||
        env_add_num(env, n, "SERVER_PORT", req->server_port) < 0 ||
        env_add(env, n, "REQUEST_METHOD", req->method) < 0 ||
        env_add(env, n, "QUERY_STRING", req->query ? req->query : "") < 0 ||
        env_add(env, n, "SCRIPT_NAME", req->script_name) < 0 ||
        env_add(env, n, "REMOTE_ADDR", req->remote_addr) < 0 ||
        env_add_num(env, n, "REMOTE_PORT", req->remote_port) < 0 ||
        env_add(env, n, "PATH", "/usr/bin:/bin") < 0)
        return -1;
    if (req->content_length >= 0 &&
        env_add_num(env, n, "CONTENT_LENGTH", req->content_length) < 0)
        return -1;
    if (req->content_type != NULL &&
        env_add(env, n, "CONTENT_TYPE", req->content_type) < 0)
        return -1;
    return 0;
}

static int set_cloexec(int fd)
{
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static void close_if_open(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void kill_and_reap(pid_t pid)
{
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

static void child_exec(const struct cgi_request *req, char **env, int in_r, int out_w)
{
    char dir[PATH_MAX];
    char *argv[2];
    char *slash;
    struct sigaction sa;

    /* 서버가 무시하도록 한 SIGPIPE 는 exec 뒤에도 남으므로 기본값으로 되돌린다 */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);

    if (dup2(in_r, STDIN_FILENO) < 0 || dup2(out_w, STDOUT_FILENO) < 0)
        _exit(127);

    snprintf(dir, sizeof dir, "%s", req->script_path);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (chdir(dir[0] ? dir : "/") < 0)
            _exit(127);
    }

    argv[0] = (char *)req->script_path;
    argv[1] = NULL;
    execve(req->script_path, argv, env);
    _exit(127);                           /* exec 실패 */
}

void cgi_run(const struct cgi_request *req, int timeout_ms, struct cgi_result *res)
{
    char *env[CGI_ENV_MAX + 1];
    int env_n = 0;
    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1};
    size_t written = 0, total = 0;
    long long deadline;
    pid_t pid = -1;
    int wst = 0;

    memset(res, 0, sizeof *res);
    res->fail = CGI_FAIL_INTERNAL;

    res->output = malloc(CGI_OUTPUT_MAX + 1);     /* +1: 상한 초과 감지용 */
    if (res->output == NULL || build_env(req, env, &env_n) < 0)
        goto fail_env;
    if (pipe(in_p) < 0 || pipe(out_p) < 0)
        goto fail_pipes;
    /* CGI 가 상대편 끝을 물려받지 않게 (dup2 로 만든 0/1 은 CLOEXEC 가 풀림) */
    if (set_cloexec(in_p[0]) < 0 || set_cloexec(in_p[1]) < 0 ||
        set_cloexec(out_p[0]) < 0 || set_cloexec(out_p[1]) < 0)
        goto fail_pipes;

    deadline = now_ms() + timeout_ms;
    pid = fork();
    if (pid < 0)
        goto fail_pipes;
    if (pid == 0)
        child_exec(req, env, in_p[0], out_p[1]);

    env_free(env, env_n);
    env_n = 0;
    close_if_open(&in_p[0]);
    close_if_open(&out_p[1]);
    if (req->body_len == 0)
        close_if_open(&in_p[1]);          /* GET 등: 빈 stdin */
    else if (fcntl(in_p[1], F_SETFL, O_NONBLOCK) < 0)
        goto fail_child;

    /* stdin 쓰기와 stdout 읽기를 동시에: 파이프가 차서 서로 기다리는 교착 방지 */
    while (out_p[0] >= 0) {
        struct pollfd pf[2];
        int nf = 0, r;
        long long left = deadline - now_ms();

        if (left <= 0) {
            res->fail = CGI_FAIL_TIMEOUT;
            goto fail_child;
        }
        pf[nf].fd = out_p[0];
        pf[nf].events = POLLIN;
        pf[nf++].revents = 0;
        if (in_p[1] >= 0) {
            pf[nf].fd = in_p[1];
            pf[nf].events = POLLOUT;
            pf[nf++].revents = 0;
        }
        r = poll(pf, (nfds_t)nf, (int)left);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            goto fail_child;
        }

        if (nf == 2 && pf[1].revents != 0) {
            ssize_t w = write(in_p[1], req->body + written, req->body_len - written);
            if (w > 0) {
                written += (size_t)w;
                if (written == req->body_len)
                    close_if_open(&in_p[1]);
            } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                close_if_open(&in_p[1]);  /* EPIPE: CGI 가 stdin 을 다 안 읽음. 오류 아님 */
            }
        }
        if (pf[0].revents != 0) {
            ssize_t n = read(out_p[0], res->output + total, CGI_OUTPUT_MAX + 1 - total);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                goto fail_child;
            }
            if (n == 0) {
                close_if_open(&out_p[0]);  /* EOF */
            } else {
                total += (size_t)n;
                if (total > CGI_OUTPUT_MAX) {
                    res->fail = CGI_FAIL_TOO_LARGE;
                    goto fail_child;
                }
            }
        }
    }
    close_if_open(&in_p[1]);

    /* stdout 이 닫힌 뒤에도 데드라인 안에 끝나야 한다 */
    for (;;) {
        struct timespec tick = {0, 5 * 1000 * 1000};
        pid_t w = waitpid(pid, &wst, WNOHANG);

        if (w == pid)
            break;
        if (w < 0 && errno != EINTR)
            goto fail_child;
        if (now_ms() >= deadline) {
            res->fail = CGI_FAIL_TIMEOUT;
            goto fail_child;
        }
        nanosleep(&tick, NULL);
    }

    res->output_len = total;
    if (WIFSIGNALED(wst)) {
        res->fail = CGI_FAIL_SIGNAL;
        res->detail = WTERMSIG(wst);
    } else if (WIFEXITED(wst) && WEXITSTATUS(wst) != 0) {
        res->fail = CGI_FAIL_EXIT;
        res->detail = WEXITSTATUS(wst);
    } else if (cgi_parse_output(res->output, total, &res->status,
                                res->content_type, sizeof res->content_type,
                                &res->body_off) < 0) {
        res->fail = CGI_FAIL_BAD_HEADER;
    } else {
        res->fail = CGI_OK;
        return;
    }
    free(res->output);                    /* S11: 실패한 CGI 의 출력은 버림 */
    res->output = NULL;
    return;

fail_child:
    kill_and_reap(pid);
fail_pipes:
    close_if_open(&in_p[0]);
    close_if_open(&in_p[1]);
    close_if_open(&out_p[0]);
    close_if_open(&out_p[1]);
fail_env:
    env_free(env, env_n);
    free(res->output);
    res->output = NULL;
}

void cgi_result_free(struct cgi_result *res)
{
    free(res->output);
    res->output = NULL;
}
