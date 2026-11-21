#define _POSIX_C_SOURCE 200809L
/* 웹 애플리케이션 */

#include "app.h"
#include "http.h"
#include "webserver.h"

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    return webserver_run();
}

struct http_response app_handle_request(const struct http_request *request, struct router_result route)
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
        /* Only this trusted literal is opened; URL paths never reach open(). */
        int fd = open("index.html", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
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
