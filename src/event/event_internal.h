#ifndef EVENT_INTERNAL_H
#define EVENT_INTERNAL_H

#include "event_backend.h"

typedef struct ev_backend_ops ev_backend_ops_t;

struct ev_loop {
    const ev_backend_ops_t *ops;
    void *state;
};

struct ev_backend_ops {
    const char *name;
    int (*add)(void *state, int fd, uint32_t events, void *ptr);
    int (*mod)(void *state, int fd, uint32_t events, void *ptr);
    int (*del)(void *state, int fd);
    int (*wait)(void *state, ev_event_t *out, int maxevents, int timeout_ms);
    void (*destroy)(void *state);
};

ev_loop_t *ev_loop_alloc(const ev_backend_ops_t *ops, void *state);

ev_loop_t *ev_poll_create(void);

#if defined(__linux__)
ev_loop_t *ev_epoll_create(void);
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
ev_loop_t *ev_kqueue_create(void);
#endif

#endif
