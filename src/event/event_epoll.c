#include "event_internal.h"

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct {
    int fd;
    void *ptr;
} ep_entry_t;

typedef struct {
    int epfd;
    ep_entry_t *entries;
    int count;
    int cap;
    struct epoll_event *evs;
    int evs_cap;
} epoll_state_t;

static int ep_find(const epoll_state_t *s, int fd) {
    for (int i = 0; i < s->count; i++) {
        if (s->entries[i].fd == fd)
            return i;
    }
    return -1;
}

static int ep_reserve(epoll_state_t *s) {
    if (s->count < s->cap)
        return 0;

    int newcap = s->cap > 0 ? s->cap * 2 : 8;
    ep_entry_t *n = malloc((size_t)newcap * sizeof *n);
    if (n == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (s->count > 0)
        memcpy(n, s->entries, (size_t)s->count * sizeof *n);

    free(s->entries);
    s->entries = n;
    s->cap = newcap;
    return 0;
}

static int ep_ensure_events(epoll_state_t *s, int maxevents) {
    if (s->evs_cap >= maxevents)
        return 0;

    int newcap = s->evs_cap > 0 ? s->evs_cap * 2 : 8;
    if (newcap < maxevents)
        newcap = maxevents;

    struct epoll_event *n = realloc(s->evs, (size_t)newcap * sizeof *n);
    if (n == NULL) {
        errno = ENOMEM;
        return -1;
    }
    s->evs = n;
    s->evs_cap = newcap;
    return 0;
}

static uint32_t ep_events(uint32_t events) {
    uint32_t e = 0;
    if (events & EV_READ)
        e |= EPOLLIN;
    if (events & EV_WRITE)
        e |= EPOLLOUT;
    return e;
}

static uint32_t event_flags(uint32_t ep_events_mask) {
    uint32_t e = 0;
    if (ep_events_mask & EPOLLIN)
        e |= EV_READ;
    if (ep_events_mask & EPOLLOUT)
        e |= EV_WRITE;
    if (ep_events_mask & (EPOLLERR | EPOLLHUP))
        e |= EV_ERR;
    return e;
}

static int ep_add(void *state, int fd, uint32_t events, void *ptr) {
    epoll_state_t *s = state;

    if (ep_find(s, fd) >= 0) {
        errno = EEXIST;
        return -1;
    }
    if (ep_reserve(s) < 0)
        return -1;

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = ep_events(events);
    ev.data.fd = fd;

    if (epoll_ctl(s->epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return -1;

    s->entries[s->count].fd = fd;
    s->entries[s->count].ptr = ptr;
    s->count++;
    return 0;
}

static int ep_mod(void *state, int fd, uint32_t events, void *ptr) {
    epoll_state_t *s = state;
    int i = ep_find(s, fd);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = ep_events(events);
    ev.data.fd = fd;

    if (epoll_ctl(s->epfd, EPOLL_CTL_MOD, fd, &ev) < 0)
        return -1;

    s->entries[i].ptr = ptr;
    return 0;
}

static int ep_del(void *state, int fd) {
    epoll_state_t *s = state;
    int i = ep_find(s, fd);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }

    if (epoll_ctl(s->epfd, EPOLL_CTL_DEL, fd, NULL) < 0)
        return -1;

    int last = s->count - 1;
    if (i != last)
        s->entries[i] = s->entries[last];
    s->count--;
    return 0;
}

static int ep_wait(void *state, ev_event_t *out, int maxevents,
                   int timeout_ms) {
    epoll_state_t *s = state;

    if (ep_ensure_events(s, maxevents) < 0)
        return -1;

    int n = epoll_wait(s->epfd, s->evs, maxevents, timeout_ms);
    if (n <= 0)
        return n;

    for (int i = 0; i < n; i++) {
        int fd = s->evs[i].data.fd;
        int idx = ep_find(s, fd);

        out[i].fd = fd;
        out[i].events = event_flags(s->evs[i].events);
        out[i].ptr = idx >= 0 ? s->entries[idx].ptr : NULL;
    }
    return n;
}

static void ep_destroy(void *state) {
    epoll_state_t *s = state;
    if (s->epfd >= 0)
        close(s->epfd);
    free(s->entries);
    free(s->evs);
    free(s);
}

static const ev_backend_ops_t epoll_ops = {
    "epoll",
    ep_add,
    ep_mod,
    ep_del,
    ep_wait,
    ep_destroy,
};

ev_loop_t *ev_epoll_create(void) {
    epoll_state_t *s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;

    s->epfd = epoll_create(1);
    if (s->epfd < 0) {
        free(s);
        return NULL;
    }
    if (fcntl(s->epfd, F_SETFD, FD_CLOEXEC) < 0) {
        close(s->epfd);
        free(s);
        return NULL;
    }

    ev_loop_t *loop = ev_loop_alloc(&epoll_ops, s);
    if (loop == NULL) {
        close(s->epfd);
        free(s);
        return NULL;
    }
    return loop;
}

#endif
