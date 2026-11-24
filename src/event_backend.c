#include "event_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

ev_loop_t *ev_loop_alloc(const ev_backend_ops_t *ops, void *state) {
    if (ops == NULL || state == NULL) {
        errno = EINVAL;
        return NULL;
    }

    ev_loop_t *loop = calloc(1, sizeof *loop);
    if (loop == NULL)
        return NULL;

    loop->ops = ops;
    loop->state = state;
    return loop;
}

static int valid_events(uint32_t events) {
    return events != 0 && (events & ~(EV_READ | EV_WRITE)) == 0;
}

ev_loop_t *ev_create_backend(const char *name) {
    if (name == NULL || strcmp(name, "auto") == 0) {
#if defined(__linux__)
        ev_loop_t *loop = ev_epoll_create();
        if (loop != NULL)
            return loop;
#elif defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__OpenBSD__) || defined(__NetBSD__)
        ev_loop_t *loop = ev_kqueue_create();
        if (loop != NULL)
            return loop;
#endif
        return ev_poll_create();
    }

    if (strcmp(name, "poll") == 0)
        return ev_poll_create();

#if defined(__linux__)
    if (strcmp(name, "epoll") == 0)
        return ev_epoll_create();
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    if (strcmp(name, "kqueue") == 0)
        return ev_kqueue_create();
#endif

    errno = ENOTSUP;
    return NULL;
}

ev_loop_t *ev_create(void) {
    return ev_create_backend(NULL);
}

void ev_destroy(ev_loop_t *loop) {
    if (loop == NULL)
        return;

    if (loop->ops != NULL && loop->ops->destroy != NULL)
        loop->ops->destroy(loop->state);
    free(loop);
}

const char *ev_backend_name(void) {
#if defined(__linux__)
    return "epoll";
#elif defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__OpenBSD__) || defined(__NetBSD__)
    return "kqueue";
#else
    return "poll";
#endif
}

const char *ev_loop_backend_name(const ev_loop_t *loop) {
    if (loop == NULL || loop->ops == NULL)
        return "?";
    return loop->ops->name;
}

int ev_add(ev_loop_t *loop, int fd, uint32_t events, void *ptr) {
    if (loop == NULL || loop->ops == NULL || loop->ops->add == NULL ||
        fd < 0 || !valid_events(events)) {
        errno = EINVAL;
        return -1;
    }
    return loop->ops->add(loop->state, fd, events, ptr);
}

int ev_mod(ev_loop_t *loop, int fd, uint32_t events, void *ptr) {
    if (loop == NULL || loop->ops == NULL || loop->ops->mod == NULL ||
        fd < 0 || !valid_events(events)) {
        errno = EINVAL;
        return -1;
    }
    return loop->ops->mod(loop->state, fd, events, ptr);
}

int ev_del(ev_loop_t *loop, int fd) {
    if (loop == NULL || loop->ops == NULL || loop->ops->del == NULL ||
        fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return loop->ops->del(loop->state, fd);
}

int ev_wait(ev_loop_t *loop, ev_event_t *out, int maxevents,
            int timeout_ms) {
    if (loop == NULL || loop->ops == NULL || loop->ops->wait == NULL ||
        out == NULL || maxevents <= 0) {
        errno = EINVAL;
        return -1;
    }
    return loop->ops->wait(loop->state, out, maxevents, timeout_ms);
}
