#ifndef CLIENT_SESSION_H
#define CLIENT_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/poll.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque per-client state machine.  Owns the socket fd, the decoded body,
 * any opened response file descriptor, and the access-log record.
 */
struct client_session;

/*
 * Take ownership of a non-blocking-ready client socket.  On failure the
 * fd is left untouched and the caller must close it.
 */
struct client_session *client_session_create(int fd, int root_fd, int log_fd);

/*
 * Close the socket, free all resources, and emit exactly one access-log
 * record if a log fd was supplied.  Safe on NULL.
 */
void client_session_destroy(struct client_session *client);

/*
 * Return the events the scheduler should wait for: POLLIN, POLLOUT, or 0
 * when the session is terminal.
 */
short client_session_interest(const struct client_session *client);

/*
 * Drive the state machine.  Both booleans may be true at once (e.g. epoll
 * reported readability and writability).  May be called with both false to
 * process deadlines only.  Returns true while the session is still alive.
 */
bool client_session_advance(struct client_session *client, bool readable, bool writable);

/*
 * Milliseconds until the nearest active deadline, or -1 when terminal.
 * Schedulers should wake the session at least this often.
 */
int64_t client_session_remaining_ms(const struct client_session *client);

/*
 * Signal an in-progress shutdown.  Returns false for unvalidated clients
 * (caller should destroy/close immediately).  For validated clients the
 * supplied deadline is applied persistently to all stage deadlines.
 */
bool client_session_begin_shutdown(struct client_session *client,
                                   const struct timespec *shutdown_deadline);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_SESSION_H */
