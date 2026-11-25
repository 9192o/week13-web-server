#include "proxy_internal.h"

#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_max_idle;
static int g_idle_timeout_ms;

void proxy_pool_init(int max_idle, int idle_timeout_ms) {
    g_max_idle = max_idle;
    g_idle_timeout_ms = idle_timeout_ms;
}

int proxy_pool_acquire(proxy_backend_t *b) {
    proxy_idle_t **pp = &b->idle_head;
    while (*pp != NULL) {
        proxy_idle_t *idle = *pp;
        char byte;
        ssize_t n = recv(idle->fd, &byte, 1, MSG_PEEK);
        if (n == 0 || n > 0 ||
            (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
             errno != EINTR)) {
            *pp = idle->next;
            b->idle_count--;
            close(idle->fd);
            free(idle);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;

        *pp = idle->next;
        b->idle_count--;
        int fd = idle->fd;
        free(idle);
        return fd;
    }
    return -1;
}

void proxy_pool_release(proxy_backend_t *b, int fd) {
    if (g_max_idle <= 0 || b->idle_count >= g_max_idle) {
        close(fd);
        return;
    }

    proxy_idle_t *idle = malloc(sizeof *idle);
    if (idle == NULL) {
        close(fd);
        return;
    }
    idle->fd = fd;
    idle->idle_since_ms = now_ms();
    idle->next = b->idle_head;
    b->idle_head = idle;
    b->idle_count++;
}

void proxy_pool_close(proxy_backend_t *b) {
    proxy_idle_t *idle = b->idle_head;
    while (idle != NULL) {
        proxy_idle_t *next = idle->next;
        close(idle->fd);
        free(idle);
        idle = next;
    }
    b->idle_head = NULL;
    b->idle_count = 0;
}

void proxy_pool_sweep(proxy_backend_t *b, uint64_t now) {
    proxy_idle_t **pp = &b->idle_head;
    while (*pp != NULL) {
        proxy_idle_t *idle = *pp;
        if (now - idle->idle_since_ms > (uint64_t)g_idle_timeout_ms) {
            *pp = idle->next;
            b->idle_count--;
            close(idle->fd);
            free(idle);
        } else {
            pp = &idle->next;
        }
    }
}

int proxy_pool_next_timeout_ms(const proxy_backend_t *b) {
    uint64_t now = now_ms();
    uint64_t min = UINT64_MAX;

    for (proxy_idle_t *idle = b->idle_head; idle != NULL; idle = idle->next) {
        uint64_t expire = idle->idle_since_ms + (uint64_t)g_idle_timeout_ms;
        if (expire < min)
            min = expire;
    }

    if (min == UINT64_MAX)
        return -1;
    if (min <= now)
        return 0;
    uint64_t d = min - now;
    return d > (uint64_t)INT_MAX ? INT_MAX : (int)d;
}
