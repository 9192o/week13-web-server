#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

static const char PAYLOAD[] =
    "Deterministic identical socketpair payload used for both LT and ET.";

static int run_mode(const char *name, uint32_t extra_flags, int *event_present)
{
    int sv[2] = {-1, -1};
    int efd = -1;
    int rc = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) < 0) {
        perror("socketpair");
        return -1;
    }

    efd = epoll_create1(EPOLL_CLOEXEC);
    if (efd < 0) {
        perror("epoll_create1");
        goto cleanup;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | extra_flags;
    ev.data.fd = sv[0];
    if (epoll_ctl(efd, EPOLL_CTL_ADD, sv[0], &ev) < 0) {
        perror("epoll_ctl ADD");
        goto cleanup;
    }

    /* Keep the peer open so neither wait can mistake a hangup for data readiness. */
    const size_t plen = sizeof(PAYLOAD) - 1;
    ssize_t nwritten = write(sv[1], PAYLOAD, plen);
    if (nwritten < 0) {
        perror("write");
        goto cleanup;
    }
    if ((size_t) nwritten != plen) {
        fprintf(stderr, "%s: short write %zd/%zu\n", name, nwritten, plen);
        goto cleanup;
    }

    struct epoll_event out;

    int first = epoll_wait(efd, &out, 1, 0);
    if (first < 0) {
        perror("epoll_wait first");
        goto cleanup;
    }
    if (first != 1 || !(out.events & EPOLLIN)) {
        fprintf(stderr, "%s: expected an initial EPOLLIN event\n", name);
        goto cleanup;
    }

    char buf[1];
    ssize_t nread = read(sv[0], buf, sizeof(buf));
    if (nread < 0) {
        perror("read");
        goto cleanup;
    }
    if (nread != 1) {
        fprintf(stderr, "%s: partial read got %zd, expected 1\n", name, nread);
        goto cleanup;
    }

    int second = epoll_wait(efd, &out, 1, 0);
    if (second < 0) {
        perror("epoll_wait second");
        goto cleanup;
    }

    if (second > 0 && !(out.events & EPOLLIN)) {
        fprintf(stderr, "%s: unexpected non-read event\n", name);
        goto cleanup;
    }
    *event_present = (second > 0);
    rc = 0;

cleanup:
    if (efd >= 0) {
        (void) close(efd);
    }
    (void) close(sv[0]);
    (void) close(sv[1]);
    return rc;
}

int main(void)
{
    int lt_present = 0;
    int et_present = 0;

    if (run_mode("LT", 0, &lt_present) != 0) {
        return EXIT_FAILURE;
    }
    if (run_mode("ET", EPOLLET, &et_present) != 0) {
        return EXIT_FAILURE;
    }

    printf("LT second wait: event %s\n", lt_present ? "present" : "absent");
    printf("ET second wait: event %s\n", et_present ? "present" : "absent");

    if (lt_present && !et_present) {
        printf("OK: LT reports ready, ET does not after partial read\n");
        return EXIT_SUCCESS;
    }

    printf("MISMATCH: expected LT present and ET absent\n");
    return EXIT_FAILURE;
}
