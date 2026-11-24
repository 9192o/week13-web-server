#ifndef SHUTDOWN_H
#define SHUTDOWN_H

#include <stdbool.h>

#ifndef SHUTDOWN_GRACE_SECONDS
#define SHUTDOWN_GRACE_SECONDS 30
#endif

int  shutdown_init(void);
void shutdown_close(void);
int  shutdown_fd(void);
bool shutdown_requested(void);
int  shutdown_grace_seconds(void);

#endif
