#ifndef PROXY_INTERNAL_H
#define PROXY_INTERNAL_H

#include "config.h"

#include <stdint.h>
#include <sys/socket.h>

typedef struct proxy_idle {
    int fd;
    uint64_t idle_since_ms;
    struct proxy_idle *next;
} proxy_idle_t;

typedef struct {
    char host[MAX_BACKEND_HOST];
    int port;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint64_t down_until_ms;
    proxy_idle_t *idle_head;
    int idle_count;
} proxy_backend_t;

/* Backend keep-alive pool. */
void proxy_pool_init(int max_idle, int idle_timeout_ms);
int proxy_pool_acquire(proxy_backend_t *b);
void proxy_pool_release(proxy_backend_t *b, int fd);
void proxy_pool_close(proxy_backend_t *b);
void proxy_pool_sweep(proxy_backend_t *b, uint64_t now);
int proxy_pool_next_timeout_ms(const proxy_backend_t *b);

#endif
