#ifndef EPOLL_LOOP_H
#define EPOLL_LOOP_H

#include <stddef.h>

struct session_app;
/* Takes ownership of the nonblocking listener; borrows the other descriptors. */
int epoll_loop_run(int listener_fd, int signal_fd, int log_fd,
                   unsigned short port, size_t connection_limit,
                   const struct session_app *app);

#endif
