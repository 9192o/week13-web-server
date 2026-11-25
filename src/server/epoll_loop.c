#define _GNU_SOURCE
#include "epoll_loop.h"

#include "client_session.h"
#include "connection.h"
#include "session_app.h"
#include "shutdown.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define EVENT_BATCH 64
#define ACCEPT_BATCH 32

struct loop_client;
struct loop_endpoint
{
    struct loop_client *client;
    bool upstream;
};

struct loop_client
{
    struct loop_client *next;
    struct client_session *session;
    int fd;
    uint32_t interest;
    int upstream_fd;
    short upstream_interest;
    struct loop_endpoint downstream_event;
    struct loop_endpoint upstream_event;
    bool dead;
};

struct event_loop
{
    int fd;
    int listener_fd;
    int signal_fd;
    int log_fd;
    size_t limit;
    size_t active;
    const struct session_app *app;
    struct loop_client *clients;
    bool listening;
    bool stopping;
};

static int set_listener_interest(struct event_loop *loop)
{
    bool want = !loop->stopping && loop->active < loop->limit;
    if (want == loop->listening)
        return 0;

    struct epoll_event event = {.events = EPOLLIN, .data.ptr = &loop->listener_fd};
    if (epoll_ctl(loop->fd, want ? EPOLL_CTL_ADD : EPOLL_CTL_DEL,
                  loop->listener_fd, want ? &event : NULL) < 0)
        return -1;
    loop->listening = want;
    return 0;
}

static int update_upstream_interest(struct event_loop *loop, struct loop_client *client)
{
    const int fd = client_session_upstream_fd(client->session);
    const short want = client_session_upstream_interest(client->session);
    if (fd != client->upstream_fd)
    {
        /* CGI's worker may still hold its completion FD after the session leaves. */
        if (client->upstream_interest && loop->app->stream_ops &&
            loop->app->stream_ops->response)
            (void)epoll_ctl(loop->fd, EPOLL_CTL_DEL, client->upstream_fd, NULL);
        client->upstream_fd = fd;
        client->upstream_interest = 0;
    }
    if (want == client->upstream_interest)
        return 0;
    int op = want == 0 ? EPOLL_CTL_DEL :
             client->upstream_interest == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    struct epoll_event event = {
        .events = (want & POLLIN ? EPOLLIN : 0) | (want & POLLOUT ? EPOLLOUT : 0),
        .data.ptr = &client->upstream_event,
    };
    if (epoll_ctl(loop->fd, op, fd, want ? &event : NULL) < 0)
        return -1;
    client->upstream_interest = want;
    return 0;
}

static int update_client_interest(struct event_loop *loop, struct loop_client *client)
{
    short want = client_session_interest(client->session);
    uint32_t interest = EPOLLERR;
    if (want & POLLIN)
        interest |= EPOLLIN | EPOLLRDHUP;
    if (want & POLLOUT)
        interest |= EPOLLOUT;
    if (interest != client->interest)
    {
        struct epoll_event event = {.events = interest, .data.ptr = &client->downstream_event};
        if (epoll_ctl(loop->fd, client->interest ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
                      client->fd, &event) < 0)
            return -1;
        client->interest = interest;
    }
    return update_upstream_interest(loop, client);
}

static int accept_ready_clients(struct event_loop *loop)
{
    for (int i = 0; i < ACCEPT_BATCH && loop->active < loop->limit; ++i)
    {
        int fd = accept4(loop->listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            return -1;
        }
        struct loop_client *client = calloc(1, sizeof(*client));
        if (client == NULL)
        {
            close(fd);
            return -1;
        }
        client->session = client_session_create(fd, loop->log_fd, loop->app);
        if (client->session == NULL)
        {
            free(client);
            close(fd);
            return -1;
        }
        client->fd = fd;
        client->upstream_fd = -1;
        client->downstream_event.client = client;
        client->upstream_event = (struct loop_endpoint){client, true};
        client->next = loop->clients;
        loop->clients = client;
        ++loop->active;
        if (update_client_interest(loop, client) < 0)
            return -1;
    }
    return set_listener_interest(loop);
}

static void reap_clients(struct event_loop *loop, bool all)
{
    struct loop_client **link = &loop->clients;
    while (*link != NULL)
    {
        struct loop_client *client = *link;
        if (!all && !client->dead)
        {
            link = &client->next;
            continue;
        }
        *link = client->next;
        if (client->upstream_interest)
            (void)epoll_ctl(loop->fd, EPOLL_CTL_DEL, client->upstream_fd, NULL);
        client_session_destroy(client->session);
        free(client);
        --loop->active;
    }
}

static int begin_shutdown(struct event_loop *loop)
{
    struct timespec deadline;
    if (connection_deadline_from_now((uint32_t)shutdown_grace_seconds() * 1000u,
                                     NULL, &deadline) < 0)
        return -1;
    loop->stopping = true;
    if (set_listener_interest(loop) < 0 ||
        epoll_ctl(loop->fd, EPOLL_CTL_DEL, loop->signal_fd, NULL) < 0)
        return -1;
    close(loop->listener_fd);
    loop->listener_fd = -1;
    /* Events already copied out by epoll_wait can still refer to these clients. */
    for (struct loop_client *client = loop->clients; client; client = client->next)
        if (!client_session_begin_shutdown(client->session, &deadline))
            client->dead = true;
    return 0;
}

static int update_deadlines(struct event_loop *loop, int *timeout)
{
    *timeout = loop->app->expire_idle ? loop->app->expire_idle(loop->app->config) : -1;
    for (struct loop_client *client = loop->clients; client; client = client->next)
    {
        int64_t remaining = client_session_remaining_ms(client->session);
        if (remaining == 0)
        {
            if (!client_session_advance(client->session, false, false))
            {
                client->dead = true;
                continue;
            }
            if (update_client_interest(loop, client) < 0)
                return -1;
            remaining = client_session_remaining_ms(client->session);
        }
        if (remaining < 0)
            return -1;
        if (remaining > INT_MAX)
            remaining = INT_MAX;
        if (*timeout < 0 || remaining < *timeout)
            *timeout = (int)remaining;
    }
    return 0;
}

static int dispatch_events(struct event_loop *loop, struct epoll_event *events, int count)
{
    bool accept_ready = false;
    for (int i = 0; i < count; ++i)
    {
        /* A pending signal can be outside this batch if the ready set is large. */
        if (!loop->stopping && shutdown_requested() && begin_shutdown(loop) < 0)
            return -1;
        void *key = events[i].data.ptr;
        if (key == &loop->signal_fd)
            continue;
        if (key == &loop->listener_fd)
        {
            accept_ready = true;
            continue;
        }

        struct loop_endpoint *endpoint = key;
        struct loop_client *client = endpoint->client;
        if (client->dead)
            continue;
        const uint32_t bits = events[i].events;
        bool readable = (bits & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0;
        bool writable = (bits & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0;
        bool alive = endpoint->upstream
            ? client_session_advance_upstream(client->session, readable, writable)
            : client_session_advance(client->session, readable, writable);
        if (!alive || (!endpoint->upstream && (bits & (EPOLLHUP | EPOLLERR))))
            client->dead = true;
        else if (update_client_interest(loop, client) < 0)
            return -1;
    }
    if (!loop->stopping && shutdown_requested() && begin_shutdown(loop) < 0)
        return -1;
    reap_clients(loop, false);
    if (set_listener_interest(loop) < 0)
        return -1;
    if (accept_ready && loop->listening)
        return accept_ready_clients(loop);
    return 0;
}

int epoll_loop_run(int listener_fd, int signal_fd, int log_fd,
                   unsigned short port, size_t connection_limit,
                   const struct session_app *app)
{
    struct event_loop loop = {
        .fd = epoll_create1(EPOLL_CLOEXEC),
        .listener_fd = listener_fd, .signal_fd = signal_fd,
        .log_fd = log_fd, .limit = connection_limit, .app = app};
    if (loop.fd < 0)
    {
        perror("Creating epoll Failed");
        close(listener_fd);
        return -1;
    }
    int result = -1;
    struct epoll_event signals = {.events = EPOLLIN, .data.ptr = &loop.signal_fd};
    if (epoll_ctl(loop.fd, EPOLL_CTL_ADD, signal_fd, &signals) < 0 ||
        set_listener_interest(&loop) < 0 || connection_log_ready(log_fd, port) < 0)
        goto cleanup;

    struct epoll_event events[EVENT_BATCH];
    for (;;)
    {
        int timeout;
        if (update_deadlines(&loop, &timeout) < 0)
            break;
        reap_clients(&loop, false);
        if (loop.stopping && loop.active == 0)
        {
            result = 0;
            break;
        }
        if (set_listener_interest(&loop) < 0)
            break;
        int count = epoll_wait(loop.fd, events, EVENT_BATCH, timeout);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 || dispatch_events(&loop, events, count) < 0)
            break;
    }

cleanup:
    if (result != 0)
        perror("Running epoll loop Failed");
    reap_clients(&loop, true);
    if (loop.listener_fd >= 0)
        close(loop.listener_fd);
    close(loop.fd);
    return result;
}
