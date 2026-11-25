#define _GNU_SOURCE
#include "cgi.h"
#include "connection.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CGI_PREFIX "/cgi-bin/"
#define CGI_QUEUE 8
#define CGI_OUTPUT (1024u * 1024u)
#define CGI_RUN_MS 5000
#define CGI_WAIT_MS 15000

struct session_stream {
    int notify, script, input, output;
    atomic_int refs;
    atomic_bool canceled, done;
    struct timespec deadline;
    char *query;
    const char *method;
    size_t body_len, output_len;
    int status;
    char type[128];
};

struct cgi_pool {
    int root;
    pthread_t worker[2];
    size_t workers, head, count;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    bool stopping;
    struct session_stream *queue[CGI_QUEUE];
};

static void job_release(struct session_stream *job)
{
    if (atomic_fetch_sub(&job->refs, 1) != 1)
        return;
    if (job->notify >= 0) close(job->notify);
    if (job->script >= 0) close(job->script);
    if (job->input >= 0) close(job->input);
    if (job->output >= 0) close(job->output);
    free(job->query);
    free(job);
}

static bool write_all(int fd, const char *p, size_t n)
{
    while (n)
    {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static int start_child(struct session_stream *job, int output, pid_t *pid)
{
    char path[64], method[32], length[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", job->script);
    snprintf(method, sizeof method, "REQUEST_METHOD=%s", job->method);
    snprintf(length, sizeof length, "CONTENT_LENGTH=%zu", job->body_len);
    char *argv[] = {path, NULL};
    char *env[] = {"PATH=/usr/bin:/bin", "GATEWAY_INTERFACE=CGI/1.1",
                   method, length, job->query, NULL};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions)) return -1;
    if (posix_spawnattr_init(&attr))
    {
        posix_spawn_file_actions_destroy(&actions);
        return -1;
    }
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    int rc = posix_spawn_file_actions_adddup2(&actions, job->script, job->script) ||
             posix_spawn_file_actions_adddup2(&actions, job->input, STDIN_FILENO) ||
             posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO) ||
             posix_spawnattr_setsigmask(&attr, &empty) ||
             posix_spawnattr_setsigdefault(&attr, &defaults) ||
             posix_spawnattr_setpgroup(&attr, 0) ||
             posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP |
                 POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (!rc) rc = posix_spawn(pid, path, &actions, &attr, argv, env);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    return rc;
}

static bool parse_output(struct session_stream *job, char *data, size_t size)
{
    const char *separator = memmem(data, size, "\r\n\r\n", 4);
    const char *lf_separator = memmem(data, size, "\n\n", 2);
    size_t sep_len = 4;
    if (lf_separator && (!separator || lf_separator < separator))
    {
        separator = lf_separator;
        sep_len = 2;
    }
    if (!separator || separator - data > 8192) return false;
    const char *p = data;
    bool has_status = false;
    int status = 200;
    while (p < separator)
    {
        const char *end = memchr(p, '\n', (size_t)(separator - p));
        if (!end) end = separator;
        const char *next = end + 1;
        if (end > p && end[-1] == '\r') --end;
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (!colon) return false;
        const char *value = colon + 1;
        while (value < end && (*value == ' ' || *value == '\t')) ++value;
        for (const char *v = value; v < end; ++v)
            if ((unsigned char)*v < 32 || (unsigned char)*v >= 127) return false;
        if (colon - p == 12 && strncasecmp(p, "Content-Type", 12) == 0)
        {
            if (job->type[0] || value == end || (size_t)(end - value) >= sizeof job->type)
                return false;
            memcpy(job->type, value, (size_t)(end - value));
        }
        else if (colon - p == 6 && strncasecmp(p, "Status", 6) == 0)
        {
            if (has_status || end - value < 3 || value[0] < '2' || value[0] > '5' ||
                value[1] < '0' || value[1] > '9' || value[2] < '0' ||
                value[2] > '9' || (end - value > 3 && value[3] != ' '))
                return false;
            status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + value[2] - '0';
            /* This adapter returns a representation, not redirects or bodyless statuses. */
            if (status == 204 || status == 205 || (status >= 300 && status < 400))
                return false;
            has_status = true;
        }
        p = next;
    }
    if (!job->type[0]) return false;
    const char *body = separator + sep_len;
    size_t length = size - (size_t)(body - data);
    job->output = memfd_create("cgi-output", MFD_CLOEXEC);
    if (job->output < 0 || !write_all(job->output, body, length) ||
        lseek(job->output, 0, SEEK_SET) < 0)
        return false;
    job->output_len = length;
    job->status = status;
    return true;
}

static void execute(struct session_stream *job)
{
    if (atomic_load(&job->canceled)) return;
    char *data = malloc(CGI_OUTPUT + 1);
    int pipefd[2];
    if (!data) return;
    if (pipe2(pipefd, O_CLOEXEC) < 0) { free(data); return; }
    pid_t pid;
    int rc = fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0 ||
             start_child(job, pipefd[1], &pid);
    close(pipefd[1]);
    if (rc) { close(pipefd[0]); free(data); return; }

    struct timespec deadline;
    bool clock_ok = connection_deadline_from_now(CGI_RUN_MS, NULL, &deadline) == 0;
    bool eof = false, reaped = false;
    size_t size = 0;
    int status = 0;
    while (clock_ok && !atomic_load(&job->canceled))
    {
        int64_t left = connection_deadline_remaining_ms(&deadline);
        if (left <= 0)
        {
            job->status = 504;
            break;
        }
        struct pollfd fd = {.fd = pipefd[0], .events = POLLIN};
        int ready = poll(&fd, eof ? 0 : 1, left < 25 ? (int)left : 25);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0)
        {
            ssize_t n = read(pipefd[0], data + size, CGI_OUTPUT + 1 - size);
            if (n == 0) eof = true;
            else if (n > 0) size += (size_t)n;
            else if (errno != EINTR && errno != EAGAIN) break;
            if (size > CGI_OUTPUT) break;
        }
        if (eof)
        {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) { reaped = true; break; }
            if (waited < 0 && errno != EINTR) break;
        }
    }
    close(pipefd[0]);
    if (!reaped)
    {
        kill(-pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    else if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
             !parse_output(job, data, size) && job->output >= 0)
    {
        close(job->output);
        job->output = -1;
    }
    free(data);
}

static void *worker(void *arg)
{
    struct cgi_pool *pool = arg;
    for (;;)
    {
        pthread_mutex_lock(&pool->lock);
        while (!pool->count && !pool->stopping)
            pthread_cond_wait(&pool->ready, &pool->lock);
        if (!pool->count)
        {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
        struct session_stream *job = pool->queue[pool->head];
        pool->head = (pool->head + 1) % CGI_QUEUE;
        --pool->count;
        pthread_mutex_unlock(&pool->lock);
        execute(job);
        atomic_store(&job->done, true);
        while (eventfd_write(job->notify, 1) < 0 && errno == EINTR) {}
        job_release(job);
    }
}

struct cgi_pool *cgi_pool_open(const char *root)
{
    struct cgi_pool *pool = calloc(1, sizeof *pool);
    if (!pool) return NULL;
    pool->root = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pool->root < 0) goto fail;
    if (pthread_mutex_init(&pool->lock, NULL)) goto fail;
    if (pthread_cond_init(&pool->ready, NULL))
    {
        pthread_mutex_destroy(&pool->lock);
        goto fail;
    }
    sigset_t signals, previous;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, &previous) == 0)
    {
        for (; pool->workers < sizeof pool->worker / sizeof *pool->worker; ++pool->workers)
            if (pthread_create(&pool->worker[pool->workers], NULL, worker, pool)) break;
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
    }
    if (pool->workers) return pool;
    cgi_pool_close(pool);
    return NULL;
fail:
    if (pool->root >= 0) close(pool->root);
    free(pool);
    return NULL;
}

void cgi_pool_close(struct cgi_pool *pool)
{
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    pool->stopping = true;
    pthread_cond_broadcast(&pool->ready);
    pthread_mutex_unlock(&pool->lock);
    for (size_t i = 0; i < pool->workers; ++i)
        pthread_join(pool->worker[i], NULL);
    pthread_cond_destroy(&pool->ready);
    pthread_mutex_destroy(&pool->lock);
    close(pool->root);
    free(pool);
}

bool cgi_route(const struct http_request *request)
{
    struct http_slice path = request->target.path;
    return path.length >= sizeof CGI_PREFIX - 1 &&
           memcmp(path.data, CGI_PREFIX, sizeof CGI_PREFIX - 1) == 0;
}

struct session_stream *cgi_submit(struct cgi_pool *pool,
                                  const struct http_request *request, int *error)
{
    struct http_slice path = request->target.path;
    size_t length = path.length - (sizeof CGI_PREFIX - 1);
    char name[128];
    *error = 404;
    if (!length || length >= sizeof name) return NULL;
    for (size_t i = 0; i < length; ++i)
    {
        char c = path.data[sizeof CGI_PREFIX - 1 + i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return NULL;
        name[i] = c;
    }
    name[length] = 0;
    int script = openat(pool->root, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (script < 0) return NULL;
    struct stat st;
    if (fstat(script, &st) < 0 || !S_ISREG(st.st_mode) || !(st.st_mode & 0111))
    {
        close(script);
        return NULL;
    }
    *error = 500;
    struct session_stream *job = calloc(1, sizeof *job);
    if (!job) { close(script); return NULL; }
    atomic_init(&job->refs, 1);
    atomic_init(&job->canceled, false);
    atomic_init(&job->done, false);
    job->script = script;
    job->output = -1;
    job->status = 500;
    job->input = memfd_create("cgi-input", MFD_CLOEXEC);
    job->notify = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    job->body_len = request->body.length;
    struct http_slice method = request->line.method;
    job->method = method.length == 4 && memcmp(method.data, "POST", 4) == 0 ? "POST" :
                  method.length == 4 && memcmp(method.data, "HEAD", 4) == 0 ? "HEAD" : "GET";
    struct http_slice query = request->target.query;
    if (job->input < 0 || job->notify < 0 ||
        asprintf(&job->query, "QUERY_STRING=%.*s", (int)query.length,
                 query.data ? query.data : "") < 0 ||
        !write_all(job->input, request->body.data, job->body_len) ||
        lseek(job->input, 0, SEEK_SET) < 0 ||
        connection_deadline_from_now(CGI_WAIT_MS, NULL, &job->deadline) < 0)
        goto fail;

    pthread_mutex_lock(&pool->lock);
    if (pool->count == CGI_QUEUE)
    {
        pthread_mutex_unlock(&pool->lock);
        *error = 503;
        goto fail;
    }
    atomic_fetch_add(&job->refs, 1);
    pool->queue[(pool->head + pool->count++) % CGI_QUEUE] = job;
    pthread_cond_signal(&pool->ready);
    pthread_mutex_unlock(&pool->lock);
    return job;
fail:
    job_release(job);
    return NULL;
}

static void cancel(struct session_stream *job)
{
    atomic_store(&job->canceled, true);
    job_release(job);
}

static int notify_fd(const struct session_stream *job) { return job->notify; }
static short notify_interest(const struct session_stream *job) { (void)job; return POLLIN; }
static int64_t remaining(const struct session_stream *job)
{
    return connection_deadline_remaining_ms(&job->deadline);
}
static void cap(struct session_stream *job, const struct timespec *deadline)
{
    if (job->deadline.tv_sec > deadline->tv_sec ||
        (job->deadline.tv_sec == deadline->tv_sec && job->deadline.tv_nsec > deadline->tv_nsec))
        job->deadline = *deadline;
}
static bool response(struct session_stream *job, struct http_response *res,
                     char *location, size_t capacity)
{
    eventfd_t ignored;
    if (eventfd_read(job->notify, &ignored) < 0 || !atomic_load(&job->done))
        return false;
    int status = job->status;
    const char *reason = http_reason_phrase(status);
    *res = (struct http_response){.status = status, .file_fd = -1,
        .content_type = "text/plain; charset=utf-8", .body = {reason, strlen(reason)}};
    if (job->output >= 0 && strlen(job->type) < capacity)
    {
        strcpy(location, job->type);
        res->content_type = location;
        res->body = (struct http_slice){NULL, job->output_len};
        res->file_fd = job->output;
        job->output = -1;
    }
    return true;
}

const struct session_stream_ops cgi_ops = {
    .destroy = cancel, .fd = notify_fd, .interest = notify_interest,
    .response = response, .remaining_ms = remaining, .cap_deadline = cap,
};
