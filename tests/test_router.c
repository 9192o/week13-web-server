#define _POSIX_C_SOURCE 200809L
#include "router.h"
#include "app.h"

#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define main app_main
#include "../app.c"
#undef main

extern int __real_openat(int dirfd, const char *pathname, int flags, ...);
extern int __real_fstat(int fd, struct stat *statbuf);

static int inject_open_failure = 0;
static int inject_fstat_failure = 0;

int __wrap_openat(int dirfd, const char *pathname, int flags, ...)
{
    if (inject_open_failure && strcmp(pathname, "index.html") == 0)
    {
        errno = EIO;
        return -1;
    }
    if (flags & O_CREAT)
    {
        va_list ap;
        va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t);
        va_end(ap);
        return __real_openat(dirfd, pathname, flags, mode);
    }
    return __real_openat(dirfd, pathname, flags);
}

int __wrap_fstat(int fd, struct stat *statbuf)
{
    if (inject_fstat_failure)
    {
        errno = EIO;
        return -1;
    }
    return __real_fstat(fd, statbuf);
}

int webserver_run(unsigned short port, int root_fd)
{
    (void)port;
    (void)root_fd;
    return 0;
}

static struct http_request request_for(const char *method, const char *path)
{
    return (struct http_request){
        .line.method = {method, strlen(method)},
        .target = {.form = HTTP_TARGET_ORIGIN, .path = {path, strlen(path)}}};
}

static void test_matches(void)
{
    const struct
    {
        const char *method;
        const char *path;
        int status;
        enum router_handler handler;
        const char *allow;
    } cases[] = {
        {"GET", "/", 200, ROUTER_INDEX, NULL},
        {"HEAD", "/index.html", 200, ROUTER_INDEX, NULL},
        {"GET", "/ind%65x.html", 200, ROUTER_INDEX, NULL},
        {"POST", "/%65cho", 200, ROUTER_ECHO, NULL},
        {"GET", "/echo", 405, ROUTER_ERROR, "POST"},
        {"HEAD", "/echo", 405, ROUTER_ERROR, "POST"},
        {"POST", "/", 405, ROUTER_ERROR, "GET, HEAD"},
        {"GET", "/missing", 404, ROUTER_ERROR, NULL},
        {"POST", "/missing", 404, ROUTER_ERROR, NULL},
        {"PUT", "/echo", 501, ROUTER_ERROR, NULL},
        {"get", "/", 501, ROUTER_ERROR, NULL},
        {"GET", "/../index.html", 404, ROUTER_ERROR, NULL},
        {"GET", "/%2e%2e/index.html", 404, ROUTER_ERROR, NULL},
        {"GET", "//index.html", 404, ROUTER_ERROR, NULL},
        {"GET", "/echo/", 404, ROUTER_ERROR, NULL},
        {"GET", "/%2565cho", 404, ROUTER_ERROR, NULL},
        {"GET", "/%00", 400, ROUTER_ERROR, NULL},
        {"GET", "/bad%", 400, ROUTER_ERROR, NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        struct http_request request = request_for(cases[i].method, cases[i].path);
        request.target.query = (struct http_slice){"ignored=1", 9};
        struct router_result result = router_match_request(&request);
        assert(result.status == cases[i].status);
        assert(result.handler == cases[i].handler);
        if (cases[i].allow)
            assert(result.allow && strcmp(result.allow, cases[i].allow) == 0);
        else
            assert(result.allow == NULL);
    }
}

static void test_echo(void)
{
    const char bytes[] = "a\0\xff\r\nHTTP/1.1 200 OK\r\nX-Fake: yes\r\n";
    struct http_request request = request_for("POST", "/echo");
    request.body = (struct http_slice){bytes, sizeof(bytes) - 1};
    struct http_response response =
        app_handle_request(-1, &request, router_match_request(&request));
    assert(response.status == 200);
    assert(response.file_fd == -1);
    assert(strcmp(response.content_type, "application/octet-stream") == 0);
    assert(response.body.data == bytes);
    assert(response.body.length == sizeof(bytes) - 1);
    assert(memcmp(response.body.data, bytes, sizeof(bytes) - 1) == 0);
}

static void test_index_files(void)
{
    int previous = open(".", O_RDONLY | O_DIRECTORY);
    assert(previous >= 0);
    char directory[] = "/tmp/webserver-router-XXXXXX";
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);
    int root_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root_fd >= 0);

    struct http_request request = request_for("GET", "/");
    struct router_result route = router_match_request(&request);
    struct http_response response = app_handle_request(root_fd, &request, route);
    assert(response.status == 404 && response.file_fd == -1);

    const char bytes[] = "<html>index\0bytes</html>";
    FILE *file = fopen("index.html", "wb");
    assert(file);
    assert(fwrite(bytes, 1, sizeof(bytes) - 1, file) == sizeof(bytes) - 1);
    assert(fclose(file) == 0);
    response = app_handle_request(root_fd, &request, route);
    assert(response.status == 200 && response.file_fd >= 0);
    assert(response.body.length == sizeof(bytes) - 1);
    assert(strcmp(response.content_type, "text/html; charset=utf-8") == 0);
    char actual[sizeof(bytes)];
    assert(read(response.file_fd, actual, sizeof(actual)) == (ssize_t)(sizeof(bytes) - 1));
    assert(memcmp(actual, bytes, sizeof(bytes) - 1) == 0);
    assert(close(response.file_fd) == 0);
    assert(unlink("index.html") == 0);

    assert(symlink("/etc/passwd", "index.html") == 0);
    response = app_handle_request(root_fd, &request, route);
    assert(response.status == 403 && response.file_fd == -1);
    assert(unlink("index.html") == 0);
    assert(mkfifo("index.html", 0600) == 0);
    response = app_handle_request(root_fd, &request, route);
    assert(response.status == 403 && response.file_fd == -1);
    assert(unlink("index.html") == 0);
    assert(mkdir("index.html", 0700) == 0);
    response = app_handle_request(root_fd, &request, route);
    assert(response.status == 403 && response.file_fd == -1);
    assert(rmdir("index.html") == 0);
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(socket_fd >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, "index.html", sizeof("index.html"));
    assert(bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    response = app_handle_request(root_fd, &request, route);
    assert(response.status == 403 && response.file_fd == -1);
    assert(close(socket_fd) == 0);
    assert(unlink("index.html") == 0);

    assert(close(root_fd) == 0);
    assert(fchdir(previous) == 0);
    assert(close(previous) == 0);
    assert(rmdir(directory) == 0);
}

static void test_internal_server_error(void)
{
    int previous = open(".", O_RDONLY | O_DIRECTORY);
    assert(previous >= 0);
    char directory[] = "/tmp/webserver-500-XXXXXX";
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);
    int root_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root_fd >= 0);

    FILE *file = fopen("index.html", "wb");
    assert(file);
    assert(fwrite("x", 1, 1, file) == 1);
    assert(fclose(file) == 0);

    struct http_request request = request_for("GET", "/");
    struct router_result route = router_match_request(&request);

    inject_open_failure = 1;
    struct http_response response = app_handle_request(root_fd, &request, route);
    inject_open_failure = 0;
    assert(response.status == 500);
    assert(response.file_fd == -1);
    assert(strcmp(response.content_type, "text/plain; charset=utf-8") == 0);
    const char *reason = http_reason_phrase(response.status);
    assert(strcmp(reason, "Internal Server Error") == 0);
    assert(response.body.length == strlen(reason));
    assert(memcmp(response.body.data, reason, response.body.length) == 0);

    inject_fstat_failure = 1;
    response = app_handle_request(root_fd, &request, route);
    inject_fstat_failure = 0;
    assert(response.status == 500);
    assert(response.file_fd == -1);
    assert(response.body.length == strlen(reason));
    assert(memcmp(response.body.data, reason, response.body.length) == 0);

    assert(unlink("index.html") == 0);
    assert(close(root_fd) == 0);
    assert(fchdir(previous) == 0);
    assert(close(previous) == 0);
    assert(rmdir(directory) == 0);
}

static void test_cli_missing_args(void)
{
    unsigned short port;
    int root_fd;
    char *argv1[] = {"webserver", NULL};
    assert(cli_parse(1, argv1, &port, &root_fd) == -1);
    char *argv2[] = {"webserver", "--port", "8080", NULL};
    assert(cli_parse(3, argv2, &port, &root_fd) == -1);
    char *argv3[] = {"webserver", "--root", "/tmp", NULL};
    assert(cli_parse(3, argv3, &port, &root_fd) == -1);
}

static void test_cli_invalid_port(void)
{
    unsigned short port;
    int root_fd;
    char *argv1[] = {"webserver", "--port", "0", "--root", "/tmp", NULL};
    assert(cli_parse(5, argv1, &port, &root_fd) == -1);
    char *argv2[] = {"webserver", "--port", "65536", "--root", "/tmp", NULL};
    assert(cli_parse(5, argv2, &port, &root_fd) == -1);
    char *argv3[] = {"webserver", "--port", "abc", "--root", "/tmp", NULL};
    assert(cli_parse(5, argv3, &port, &root_fd) == -1);
    char *argv4[] = {"webserver", "--port", "-1", "--root", "/tmp", NULL};
    assert(cli_parse(5, argv4, &port, &root_fd) == -1);
    char *argv5[] = {"webserver", "--port", "+8080", "--root", "/tmp", NULL};
    assert(cli_parse(5, argv5, &port, &root_fd) == -1);
    char *argv6[] = {"webserver", "--port", " 8080", "--root", "/tmp", NULL};
    assert(cli_parse(5, argv6, &port, &root_fd) == -1);
}

static void test_cli_duplicate_arg(void)
{
    unsigned short port;
    int root_fd;
    char *argv1[] = {"webserver", "--port", "8080", "--port", "8081", "--root", "/tmp", NULL};
    assert(cli_parse(7, argv1, &port, &root_fd) == -1);
    char *argv2[] = {"webserver", "--port", "8080", "--root", "/tmp", "--root", "/tmp", NULL};
    assert(cli_parse(7, argv2, &port, &root_fd) == -1);
}

static void test_cli_unknown_arg(void)
{
    unsigned short port;
    int root_fd;
    char *argv[] = {"webserver", "--port", "8080", "--root", "/tmp", "--verbose", NULL};
    assert(cli_parse(6, argv, &port, &root_fd) == -1);
}

static void test_cli_valid(void)
{
    unsigned short port;
    int root_fd;
    char directory[] = "/tmp/webserver-cli-XXXXXX";
    assert(mkdtemp(directory));
    char *argv[] = {"webserver", "--root", directory, "--port", "8080", NULL};
    assert(cli_parse(5, argv, &port, &root_fd) == 0);
    assert(port == 8080);
    assert(root_fd >= 0);
    assert(close(root_fd) == 0);
    assert(rmdir(directory) == 0);
}

static void test_cli_root_not_directory(void)
{
    unsigned short port;
    int root_fd;
    char path[] = "/tmp/webserver-cli-file-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    char *argv[] = {"webserver", "--port", "8080", "--root", path, NULL};
    assert(cli_parse(5, argv, &port, &root_fd) == -1);
    assert(unlink(path) == 0);
}

int main(void)
{
    test_matches();
    test_echo();
    test_index_files();
    test_internal_server_error();
    test_cli_missing_args();
    test_cli_invalid_port();
    test_cli_duplicate_arg();
    test_cli_unknown_arg();
    test_cli_valid();
    test_cli_root_not_directory();
    puts("ROUTER_TESTS_PASS");
    puts("CLI_TESTS_PASS");
    return 0;
}
