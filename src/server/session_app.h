#ifndef SESSION_APP_H
#define SESSION_APP_H

#include "http.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct session_stream;

/* Optional response stream. It borrows the request until destroy(). */
struct session_stream_ops {
    void (*destroy)(struct session_stream *);
    int (*fd)(const struct session_stream *);
    short (*interest)(const struct session_stream *);
    bool (*has_output)(const struct session_stream *);
    int (*status)(const struct session_stream *);
    bool (*started)(const struct session_stream *);
    /* 0 pending, 1 complete, HTTP error before commit, or -1 to abort. */
    int (*pump)(struct session_stream *, bool, bool, int, bool, size_t *);
    int64_t (*remaining_ms)(const struct session_stream *);
    void (*cap_deadline)(struct session_stream *, const struct timespec *);
};

/* The process owns this configuration for the lifetime of its event loop. */
struct session_app {
    const void *config;
    /* True: response ready at the head. False: defer until body completion. */
    bool (*prepare)(const void *, const struct http_request *, char *,
                    struct http_response *);
    struct session_stream *(*finish)(const void *, const struct http_request *,
                                     bool, char *, struct http_response *);
    const struct session_stream_ops *stream_ops;
    bool close_after_response;
};

#endif
