#define _POSIX_C_SOURCE 200809L
#include "static_response.h"
#include "static_file.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int extension_is(const char *extension, const char *expected)
{
    while (*extension && *expected)
    {
        unsigned char c = (unsigned char)*extension++;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + 'a' - 'A');
        if (c != (unsigned char)*expected++)
            return 0;
    }
    return *extension == *expected;
}

static const char *public_content_type(const char *path)
{
    const char *name = strrchr(path, '/');
    const char *ext = strrchr(name ? name + 1 : path, '.');
    if (!ext)
        return NULL;
    if (extension_is(ext, ".html"))
        return "text/html; charset=utf-8";
    if (extension_is(ext, ".css"))
        return "text/css; charset=utf-8";
    if (extension_is(ext, ".js"))
        return "text/javascript; charset=utf-8";
    if (extension_is(ext, ".png"))
        return "image/png";
    if (extension_is(ext, ".gif"))
        return "image/gif";
    if (extension_is(ext, ".jpg") || extension_is(ext, ".jpeg"))
        return "image/jpeg";
    return NULL;
}

static int static_error(int error)
{
    switch (error)
    {
    case ENOENT:
    case ENOTDIR:
    case ENAMETOOLONG:
        return 404;
    case EPERM:
    case EACCES:
    case ELOOP:
    case EXDEV:
    case ENXIO:
    case ENODEV:
        return 403;
    default:
        fputs("Static file lookup failed.\n", stderr);
        return 500;
    }
}

struct http_response static_response(int root_fd, const struct http_request *request,
                                     const char *decoded_path, char location[HTTP_REQUEST_LINE_MAX + 2])
{
    struct http_response response = {
        .status = 200, .content_type = "text/plain; charset=utf-8", .file_fd = -1};
    size_t length = strlen(decoded_path);
    int trailing_slash = decoded_path[length - 1] == '/';
    char relative[HTTP_REQUEST_LINE_MAX + sizeof("index.html")];
    char resolved[HTTP_REQUEST_LINE_MAX];
    size_t relative_length = length - 1 - (trailing_slash && length > 1);
    if (relative_length)
        memcpy(relative, decoded_path + 1, relative_length);
    else
        relative[relative_length++] = '.';
    relative[relative_length] = '\0';

    struct stat info;
    int fd = static_file_open(root_fd, relative, &info, resolved, sizeof(resolved));
    if (fd < 0)
    {
        response.status = static_error(errno);
        return response;
    }
    int directory = S_ISDIR(info.st_mode);
    if (directory)
    {
        close(fd);
        if (length == 1)
            memcpy(relative, "index.html", sizeof("index.html"));
        else
            memcpy(relative + relative_length, "/index.html", sizeof("/index.html"));
        fd = static_file_open(root_fd, relative, &info, resolved, sizeof(resolved));
        if (fd < 0)
        {
            response.status = static_error(errno);
            return response;
        }
    }
    const char *mime = public_content_type(resolved);
    if (directory && S_ISDIR(info.st_mode))
        response.status = 404;
    else if (!S_ISREG(info.st_mode) || !public_content_type(relative) || !mime)
        response.status = 403;
    else if (trailing_slash && !directory)
        response.status = 404;
    else if (request->line.method.length == 4 &&
             memcmp(request->line.method.data, "POST", 4) == 0)
    {
        response.status = 405;
        response.allow = "GET, HEAD";
    }
    else if (directory && !trailing_slash)
    {
        size_t length = request->target.path.length;
        memcpy(location, request->target.path.data, length);
        location[length] = '/';
        location[length + 1] = '\0';
        response.status = 308;
        response.location = location;
    }
    else if (info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX)
        response.status = 500;
    else
    {
        response.file_fd = fd;
        response.content_type = mime;
        response.body.length = (size_t)info.st_size;
        return response;
    }
    close(fd);
    return response;
}
