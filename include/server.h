#ifndef SERVER_H
#define SERVER_H

#include "config.h"

int server_run(const config_t *cfg);
void server_request_stop(void);

#endif
