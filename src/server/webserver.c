#define _GNU_SOURCE

#include "webserver.h"
#include "connection.h"
#include "epoll_loop.h"
#include "shutdown.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int create_listener(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0)
    {
        perror("Creating listener failed");
        return -1;
    }
    const int enable = 1;
    const struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, SOMAXCONN) < 0)
    {
        perror("Configuring listener failed");
        close(fd);
        return -1;
    }
    return fd;
}

int http_server_run(unsigned short port, size_t connection_limit,
                    const struct session_app *app)
{
    struct sigaction ignored = {.sa_handler = SIG_IGN};
    if (sigemptyset(&ignored.sa_mask) < 0 || sigaction(SIGPIPE, &ignored, NULL) < 0)
    {
        perror("Setting SIGPIPE policy failed");
        return EXIT_FAILURE;
    }
    int signal_fd = shutdown_init();
    if (signal_fd < 0)
    {
        perror("Shutdown initialization failed");
        return EXIT_FAILURE;
    }
    int result = -1;
    int listener_fd = create_listener(port);
    if (listener_fd >= 0)
    {
        int log_fd = connection_log_fd_prepare(STDOUT_FILENO);
        if (log_fd < 0)
        {
            perror("Preparing access log failed");
            close(listener_fd);
        }
        else
        {
            result = epoll_loop_run(listener_fd, signal_fd, log_fd,
                                    port, connection_limit, app);
            close(log_fd);
        }
    }
    shutdown_close();
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
