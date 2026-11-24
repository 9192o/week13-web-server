#ifndef PROXY_EXCHANGE_H
#define PROXY_EXCHANGE_H

#include "http.h"
#include <stdbool.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/poll.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-backend connection state machine for the reverse proxy.
 *
 * The exchange owns the upstream (backend) socket, sends the reconstructed
 * request head and body, parses the response head, and relays the response
 * body to the downstream client socket supplied to proxy_exchange_pump().
 *
 * All timeouts are handled by the caller: the exchange stores one deadline
 * and exposes the remaining milliseconds so the scheduler can wake it.
 */
struct session_stream;

/*
 * Create a new exchange for a single request/response pair.
 *
 * addr    - backend IPv4 address and port (caller selects via proxy_route).
 * host    - value to use for the outgoing Host header.
 * req     - fully decoded request; its body storage must outlive the exchange.
 * is_head - true if the original method was HEAD; passed to proxy_response_head.
 *
 * Returns a new exchange, or NULL if the upstream socket could not be created
 * or the request head could not be serialized.
 */
struct session_stream *proxy_exchange_create(const struct sockaddr_in *addr,
                                             const char *host,
                                             const struct http_request *req,
                                             bool is_head);

/* Close the upstream socket and free all resources.  Safe on NULL. */
void proxy_exchange_destroy(struct session_stream *e);

/* Upstream file descriptor, or -1 if terminal. */
int proxy_exchange_fd(const struct session_stream *e);

/*
 * Events the scheduler should wait for on the upstream socket.
 * Returns POLLIN, POLLOUT, POLLIN | POLLOUT, or 0 when no upstream I/O is
 * pending. Upstream reads are paused while downstream output is staged.
 */
short proxy_exchange_interest(const struct session_stream *e);

/*
 * True when the exchange has buffered downstream output and the scheduler
 * should wait for downstream writability.  The caller supplies the downstream
 * fd to proxy_exchange_pump().
 */
bool proxy_exchange_has_output(const struct session_stream *e);

/*
 * HTTP status code parsed from the backend response, or 0 if the response
 * head has not been received yet.
 */
int proxy_exchange_status(const struct session_stream *e);

/*
 * True if any bytes (response head or body) have already been written to the
 * downstream socket.  Used by the caller to decide whether a late upstream
 * failure can still be turned into a 502 response page.
 */
bool proxy_exchange_started(const struct session_stream *e);

/*
 * Drive the exchange state machine.
 *
 * upstream_readable / upstream_writable - current readiness of the backend
 *     socket.  The caller translates epoll HUP/ERR to readable/writable as
 *     appropriate before calling.
 * downstream_fd - client socket; the exchange only writes to it.
 * downstream_writable - whether downstream_fd can accept writes right now.
 * body_sent - receives the number of body bytes written to the downstream
 *     socket during this call (decoded bytes for chunked/EOF framing).
 *
 * Returns:
 *   0  - exchange is still alive; caller should continue polling per
 *        proxy_exchange_interest() / proxy_exchange_has_output().
 *   1  - response was delivered completely and the downstream half-close has
 *        been initiated by the caller after this return.
 *   502 - upstream failure (connect, invalid response, short body, malformed
 *        chunked) before delivery completed.
 *   -1 - downstream write failure; caller should close the client.
 */
int proxy_exchange_pump(struct session_stream *e,
                        bool upstream_readable,
                        bool upstream_writable,
                        int downstream_fd,
                        bool downstream_writable,
                        size_t *body_sent);

/* Milliseconds until the active stage deadline. */
int64_t proxy_exchange_remaining_ms(const struct session_stream *e);

/*
 * Cap the internal deadline at the supplied time.  Used by the caller to
 * apply a persistent shutdown deadline without restarting the exchange.
 */
void proxy_exchange_cap_deadline(struct session_stream *e,
                                 const struct timespec *cap);

#ifdef __cplusplus
}
#endif

#endif /* PROXY_EXCHANGE_H */
