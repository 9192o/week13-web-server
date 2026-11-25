#ifndef CONN_INTERNAL_H
#define CONN_INTERNAL_H

#include "conn_common.h"

void conn_common_hook_detach(conn_t *c);
int conn_common_hook_next_timeout_ms(void);

#endif
