#ifndef ACCESS_LOG_H
#define ACCESS_LOG_H

#include <stddef.h>

#define ACCESS_LOG_IP_MAX 64
#define ACCESS_LOG_METHOD_MAX 16
#define ACCESS_LOG_PATH_MAX 256

typedef struct {
    char ip[ACCESS_LOG_IP_MAX];
    char method[ACCESS_LOG_METHOD_MAX];
    char path[ACCESS_LOG_PATH_MAX];
    int status;
    size_t bytes;
    int active;
} access_log_t;

void access_log_init(access_log_t *log, const char *ip);
void access_log_set_request(access_log_t *log, const char *method,
                            const char *path);
void access_log_set_status(access_log_t *log, int status);
int access_log_get_status(const access_log_t *log);
void access_log_add_bytes(access_log_t *log, size_t n);
void access_log_end(access_log_t *log);

/* Current request context for writen()/write_body()/response helpers. */
access_log_t *access_log_current(void);
void access_log_set_current(access_log_t *log);

#endif
