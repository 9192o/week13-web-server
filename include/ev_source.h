#ifndef EV_SOURCE_H
#define EV_SOURCE_H

/*
 * Tag for pointers stored in ev_event_t.ptr. The event loop uses this to
 * distinguish a client connection from a proxy upstream connection.
 */
enum {
    EV_SRC_CLIENT = 1,
    EV_SRC_UPSTREAM = 2
};

typedef struct {
    int kind;
} ev_source_t;

#endif
