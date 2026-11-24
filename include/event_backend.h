#ifndef EVENT_BACKEND_H
#define EVENT_BACKEND_H

#include <stdint.h>

/*
 * Event backend abstraction.
 *
 * - epoll: Linux
 * - kqueue: macOS/BSD
 * - poll: POSIX fallback
 *
 * The event loop owns connections and calls ev_add/ev_mod/ev_del.
 * ev_wait() returns only ready events.
 */

typedef struct ev_loop ev_loop_t;

enum ev_flags {
    EV_READ  = 1u << 0,
    EV_WRITE = 1u << 1,
    EV_ERR   = 1u << 2   /* backend error/hangup; close the connection */
};

typedef struct {
    int      fd;
    uint32_t events;     /* EV_READ / EV_WRITE / EV_ERR */
    void    *ptr;        /* user data, usually conn_t * */
} ev_event_t;

/* Create/destroy the backend loop. Returns NULL on failure. */
ev_loop_t *ev_create(void);

/*
 * Create a specific backend: "poll", "epoll", "kqueue", or NULL/"auto".
 * Returns NULL with errno=ENOTSUP when the backend is not available.
 */
ev_loop_t *ev_create_backend(const char *name);
void       ev_destroy(ev_loop_t *loop);

/*
 * Register/modify/remove an fd.
 * ev_add/ev_mod return 0 on success, -1 on error.
 * ev_del returns 0 on success, -1 on error.
 *
 * Always ev_del() before close(fd) to avoid fd-reuse races.
 */
int ev_add(ev_loop_t *loop, int fd, uint32_t events, void *ptr);
int ev_mod(ev_loop_t *loop, int fd, uint32_t events, void *ptr);
int ev_del(ev_loop_t *loop, int fd);

/*
 * Wait for ready events.
 * Returns the number of events written to out, 0 on timeout, -1 on error.
 * EINTR is returned as -1 with errno == EINTR; the caller should retry.
 */
int ev_wait(ev_loop_t *loop, ev_event_t *out, int maxevents, int timeout_ms);

/* "epoll", "kqueue", or "poll" for startup logs. */
const char *ev_backend_name(void);

/* Actual backend name for a loop created by ev_create_backend(). */
const char *ev_loop_backend_name(const ev_loop_t *loop);

#endif
