#include "event_internal.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint32_t events;
    void *ptr;
} kq_entry_t;

typedef struct {
    int kq;
    kq_entry_t *entries;
    int count;
    int cap;
    struct kevent *evs;
    int evs_cap;
} kqueue_state_t;

static int kq_find(const kqueue_state_t *s, int fd) {
    for (int i = 0; i < s->count; i++) {
        if (s->entries[i].fd == fd)
            return i;
    }
    return -1;
}

static int kq_reserve(kqueue_state_t *s) {
    if (s->count < s->cap)
        return 0;

    int newcap = s->cap > 0 ? s->cap * 2 : 8;
    kq_entry_t *n = malloc((size_t)newcap * sizeof *n);
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

static int kq_ensure_events(kqueue_state_t *s, int maxevents) {
    if (s->evs_cap >= maxevents)
        return 0;

    int newcap = s->evs_cap > 0 ? s->evs_cap * 2 : 8;
    if (newcap < maxevents)
        newcap = maxevents;

    struct kevent *n = realloc(s->evs, (size_t)newcap * sizeof *n);
    if (n == NULL) {
        errno = ENOMEM;
        return -1;
    }
    s->evs = n;
    s->evs_cap = newcap;
    return 0;
}

static int kq_update(kqueue_state_t *s, int fd, int16_t filter, int add,
                     void *ptr) {
    struct kevent ch;
    if (add)
        EV_SET(&ch, (uintptr_t)fd, filter, EV_ADD | EV_ENABLE, 0, 0, ptr);
    else
        EV_SET(&ch, (uintptr_t)fd, filter, EV_DELETE, 0, 0, NULL);

    if (kevent(s->kq, &ch, 1, NULL, 0, NULL) < 0) {
        if (!add && errno == ENOENT)
            return 0;
        return -1;
    }
    return 0;
}

static int kq_apply(kqueue_state_t *s, int fd, uint32_t old_events,
                    uint32_t new_events, void *ptr) {
    if (new_events & EV_READ) {
        if (kq_update(s, fd, EVFILT_READ, 1, ptr) < 0)
            return -1;
    } else if (old_events & EV_READ) {
        if (kq_update(s, fd, EVFILT_READ, 0, NULL) < 0)
            return -1;
    }

    if (new_events & EV_WRITE) {
        if (kq_update(s, fd, EVFILT_WRITE, 1, ptr) < 0)
            return -1;
    } else if (old_events & EV_WRITE) {
        if (kq_update(s, fd, EVFILT_WRITE, 0, NULL) < 0)
            return -1;
    }
    return 0;
}

static int kq_add(void *state, int fd, uint32_t events, void *ptr) {
    kqueue_state_t *s = state;

    if (kq_find(s, fd) >= 0) {
        errno = EEXIST;
        return -1;
    }
    if (kq_reserve(s) < 0)
        return -1;
    if (kq_apply(s, fd, 0, events, ptr) < 0)
        return -1;

    s->entries[s->count].fd = fd;
    s->entries[s->count].events = events;
    s->entries[s->count].ptr = ptr;
    s->count++;
    return 0;
}

static int kq_mod(void *state, int fd, uint32_t events, void *ptr) {
    kqueue_state_t *s = state;
    int i = kq_find(s, fd);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }

    if (kq_apply(s, fd, s->entries[i].events, events, ptr) < 0)
        return -1;

    s->entries[i].events = events;
    s->entries[i].ptr = ptr;
    return 0;
}

static int kq_del(void *state, int fd) {
    kqueue_state_t *s = state;
    int i = kq_find(s, fd);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }

    int rc = kq_apply(s, fd, s->entries[i].events, 0, NULL);

    int last = s->count - 1;
    if (i != last)
        s->entries[i] = s->entries[last];
    s->count--;
    return rc;
}

static int kq_wait(void *state, ev_event_t *out, int maxevents,
                   int timeout_ms) {
    kqueue_state_t *s = state;

    if (kq_ensure_events(s, maxevents) < 0)
        return -1;

    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(s->kq, NULL, 0, s->evs, maxevents, tsp);
    if (n <= 0)
        return n;

    for (int i = 0; i < n; i++) {
        uint32_t events = 0;
        if (s->evs[i].filter == EVFILT_READ)
            events |= EV_READ;
        else if (s->evs[i].filter == EVFILT_WRITE)
            events |= EV_WRITE;

        if (s->evs[i].flags & EV_ERROR)
            events |= EV_ERR;
        if (s->evs[i].flags & EV_EOF)
            events |= EV_ERR;

        out[i].fd = (int)s->evs[i].ident;
        out[i].events = events;
        out[i].ptr = s->evs[i].udata;
    }
    return n;
}

static void kq_destroy(void *state) {
    kqueue_state_t *s = state;
    if (s->kq >= 0)
        close(s->kq);
    free(s->entries);
    free(s->evs);
    free(s);
}

static const ev_backend_ops_t kqueue_ops = {
    "kqueue",
    kq_add,
    kq_mod,
    kq_del,
    kq_wait,
    kq_destroy,
};

ev_loop_t *ev_kqueue_create(void) {
    kqueue_state_t *s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;

    s->kq = kqueue();
    if (s->kq < 0) {
        free(s);
        return NULL;
    }
    if (fcntl(s->kq, F_SETFD, FD_CLOEXEC) < 0) {
        close(s->kq);
        free(s);
        return NULL;
    }

    ev_loop_t *loop = ev_loop_alloc(&kqueue_ops, s);
    if (loop == NULL) {
        close(s->kq);
        free(s);
        return NULL;
    }
    return loop;
}

#endif
