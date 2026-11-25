#ifndef PROXY_SERVER_H
#define PROXY_SERVER_H

#include "config.h"

/* Standalone proxy-only server: no static files, no CGI. */
int proxy_server_run(const config_t *cfg);

#endif
