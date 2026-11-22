#define _POSIX_C_SOURCE 200809L
/* 웹 애플리케이션 */

#include "app.h"
#include "http.h"
#include "static_file.h"
#include "static_response.h"
#include "webserver.h"

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
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

    struct stat root_info;
    char resolved[2];
    int probe_fd = static_file_open(root_fd, ".", &root_info, resolved, sizeof(resolved));
    if (probe_fd < 0)
    {
        perror("Static root capability check failed");
        close(root_fd);
        return EXIT_FAILURE;
    }
    close(probe_fd);
    int status = webserver_run(port, root_fd);
    close(root_fd);
    return status;
}

struct http_response app_handle_request(int root_fd,
                                        const struct http_request *request,
                                        const struct router_result *route,
                                        char location[HTTP_REQUEST_LINE_MAX + 2])
{
    struct http_response response = {
        .status = route->status,
        .content_type = "text/plain; charset=utf-8",
        .allow = route->allow,
        .file_fd = -1};
    if (route->handler == ROUTER_ECHO)
    {
        response.content_type = "application/octet-stream";
        response.body = request->body;
        return response;
    }
    if (route->handler == ROUTER_STATIC)
        response = static_response(root_fd, request, route->path, location);
    if (response.status < 400)
        return response;
    const char *message = http_reason_phrase(response.status);
    response.body = (struct http_slice){message, strlen(message)};
    return response;
}
