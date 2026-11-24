#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stddef.h>

struct session_app;
int http_server_run(unsigned short port, size_t connection_limit,
                    const struct session_app *app);

#endif
