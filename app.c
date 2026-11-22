#define _POSIX_C_SOURCE 200809L
/* 웹 애플리케이션 */

#include "app.h"
#include "http.h"
#include "webserver.h"

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_usage(void)
{
    fputs("Usage: ./webserver --port <1-65535> --root <directory>\n", stderr);
}

static int cli_parse(int argc, char **argv, unsigned short *port, int *root_fd)
{
    const char *port_arg = NULL;
    const char *root_arg = NULL;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--port") == 0)
        {
            if (++i >= argc || port_arg != NULL)
            {
                print_usage();
                return -1;
            }
            port_arg = argv[i];
        }
        else if (strcmp(argv[i], "--root") == 0)
        {
            if (++i >= argc || root_arg != NULL)
            {
                print_usage();
                return -1;
            }
            root_arg = argv[i];
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage();
            return -1;
        }
    }

    if (port_arg == NULL || root_arg == NULL)
    {
        print_usage();
        return -1;
    }

    for (const char *digit = port_arg; *digit; ++digit)
    {
        if (*digit < '0' || *digit > '9')
        {
            fputs("Port must contain only decimal digits.\n", stderr);
            return -1;
        }
    }
    char *end;
    errno = 0;
    long port_val = strtol(port_arg, &end, 10);
    if (errno != 0 || *end != '\0' || port_val < 1 || port_val > 65535)
    {
        fprintf(stderr, "Invalid port: %s\n", port_arg);
        return -1;
    }
    *port = (unsigned short)port_val;

    int fd = open(root_arg, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "Cannot open root directory: %s\n", root_arg);
        return -1;
    }
    *root_fd = fd;
    return 0;
}

int main(int argc, char **argv)
{
    unsigned short port;
    int root_fd;

    if (cli_parse(argc, argv, &port, &root_fd) < 0)
        return EXIT_FAILURE;

    int status = webserver_run(port, root_fd);
    close(root_fd);
    return status;
}

struct http_response app_handle_request(int root_fd,
                                        const struct http_request *request,
                                        struct router_result route)
{
    struct http_response response = {
        .status = route.status,
        .content_type = "text/plain; charset=utf-8",
        .allow = route.allow,
        .file_fd = -1};
    if (route.handler == ROUTER_ECHO)
    {
        response.content_type = "application/octet-stream";
        response.body = request->body;
        return response;
    }
    if (route.handler == ROUTER_INDEX)
    {
        int fd = openat(root_fd, "index.html",
                        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
        {
            if (errno == ENOENT)
                response.status = 404;
            else if (errno == EACCES || errno == EPERM || errno == ELOOP ||
                     errno == ENXIO || errno == ENODEV)
                response.status = 403;
            else
            {
                perror("Opening index file failed");
                response.status = 500;
            }
        }
        else
        {
            struct stat info;
            if (fstat(fd, &info) < 0)
            {
                perror("Statting index file failed");
                response.status = 500;
            }
            else if (!S_ISREG(info.st_mode))
                response.status = 403;
            else
            {
                response.content_type = "text/html; charset=utf-8";
                response.body.length = (size_t)info.st_size;
                response.file_fd = fd;
                return response;
            }
            close(fd);
        }
    }
    const char *message = http_reason_phrase(response.status);
    response.body = (struct http_slice){message, strlen(message)};
    return response;
}
