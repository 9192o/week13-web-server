#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stddef.h>

int webserver_run(unsigned short port, int root_fd, size_t connection_limit);

#endif
