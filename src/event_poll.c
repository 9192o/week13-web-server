#include "event_internal.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct pollfd *fds;
    void **ptrs;
    int count;
    int cap;
} poll_state_t;

static int poll_find(const poll_state_t *s, int fd) {
    for (int i = 0; i < s->count; i++) {
        if (s->fds[i].fd == fd)
            return i;
    }
    return -1;
}

static int poll_reserve(poll_state_t *s) {
    if (s->count < s->cap)
        return 0;

    int newcap = s->cap > 0 ? s->cap * 2 : 8;
    struct pollfd *nfds = malloc((size_t)newcap * sizeof *nfds);
    void **nptrs = malloc((size_t)newcap * sizeof *nptrs);
    if (nfds == NULL || nptrs == NULL) {
        free(nfds);
        free(nptrs);
        errno = ENOMEM;
        return -1;
    }

    if (s->count > 0) {
        memcpy(nfds, s->fds, (size_t)s->count * sizeof *nfds);
        memcpy(nptrs, s->ptrs, (size_t)s->count * sizeof *nptrs);
    }

    free(s->fds);
    free(s->ptrs);
    s->fds = nfds;
    s->ptrs = nptrs;
    s->cap = newcap;
    return 0;
}

static short poll_events(uint32_t events) {
    short e = 0;
    if (events & EV_READ)
        e |= POLLIN;
    if (events & EV_WRITE)
        e |= POLLOUT;
    return e;
}

static uint32_t event_flags(short revents) {
    uint32_t e = 0;
    if (revents & POLLIN)
        e |= EV_READ;
    if (revents & POLLOUT)
        e |= EV_WRITE;
    if (revents & (POLLERR | POLLHUP | POLLNVAL))
        e |= EV_ERR;
    return e;
}

static int poll_add(void *state, int fd, uint32_t events, void *ptr) {
    poll_state_t *s = state;

    if (poll_find(s, fd) >= 0) {
        errno = EEXIST;
        return -1;
    }
    if (poll_reserve(s) < 0)
        return -1;

    s->fds[s->count].fd = fd;
    s->fds[s->count].events = poll_events(events);
    s->fds[s->count].revents = 0;
    s->ptrs[s->count] = ptr;
    s->count++;
    return 0;
}

static int poll_mod(void *state, int fd, uint32_t events, void *ptr) {
    poll_state_t *s = state;
    int i = poll_find(s, fd);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }

    s->fds[i].events = poll_events(events);
    s->fds[i].revents = 0;
    s->ptrs[i] = ptr;
    return 0;
}

static int poll_del(void *state, int fd) {
    poll_state_t *s = state;
    int i = poll_find(s, fd);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }

    int last = s->count - 1;
    if (i != last) {
        s->fds[i] = s->fds[last];
        s->ptrs[i] = s->ptrs[last];
    }
    s->count--;
    return 0;
}

static int poll_wait(void *state, ev_event_t *out, int maxevents,
                     int timeout_ms) {
    poll_state_t *s = state;

    int n = poll(s->fds, (nfds_t)s->count, timeout_ms);
    if (n <= 0)
        return n;

    int written = 0;
    for (int i = 0; i < s->count && written < maxevents; i++) {
        if (s->fds[i].revents == 0)
            continue;

        out[written].fd = s->fds[i].fd;
        out[written].events = event_flags(s->fds[i].revents);
        out[written].ptr = s->ptrs[i];
        written++;
    }
    return written;
}

static void poll_destroy(void *state) {
    poll_state_t *s = state;
    free(s->fds);
    free(s->ptrs);
    free(s);
}

static const ev_backend_ops_t poll_ops = {
    "poll",
    poll_add,
    poll_mod,
    poll_del,
    poll_wait,
    poll_destroy,
};

ev_loop_t *ev_poll_create(void) {
    poll_state_t *s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;

    ev_loop_t *loop = ev_loop_alloc(&poll_ops, s);
    if (loop == NULL) {
        free(s);
        return NULL;
    }
    return loop;
}
