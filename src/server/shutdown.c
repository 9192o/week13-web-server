#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "shutdown.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

static int g_shutdown_fd = -1;
static volatile sig_atomic_t g_shutdown_observed = 0;
static sigset_t g_old_mask;

int shutdown_init(void)
{
    if (g_shutdown_fd >= 0)
        return g_shutdown_fd;

    sigset_t mask;
    if (sigemptyset(&mask) < 0)
        return -1;
    if (sigaddset(&mask, SIGINT) < 0 || sigaddset(&mask, SIGTERM) < 0)
        return -1;

    if (sigprocmask(SIG_BLOCK, &mask, &g_old_mask) < 0)
        return -1;

    g_shutdown_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (g_shutdown_fd < 0)
    {
        int saved_errno = errno;
        sigprocmask(SIG_SETMASK, &g_old_mask, NULL);
        errno = saved_errno;
        return -1;
    }

    return g_shutdown_fd;
}

void shutdown_close(void)
{
    if (g_shutdown_fd >= 0)
    {
        close(g_shutdown_fd);
        g_shutdown_fd = -1;
    }
    g_shutdown_observed = 0;
    /* Signals remain blocked: unblocking a pending SIGINT/SIGTERM here would
       re-deliver it to the default handler and terminate the process before
       the normal return path finishes. */
}

int shutdown_fd(void)
{
    return g_shutdown_fd;
}

bool shutdown_requested(void)
{
    if (g_shutdown_observed)
        return true;
    if (g_shutdown_fd < 0)
        return false;

    struct signalfd_siginfo info;
    ssize_t r = read(g_shutdown_fd, &info, sizeof(info));
    if (r == (ssize_t)sizeof(info))
    {
        g_shutdown_observed = 1;
        return true;
    }
    return false;
}

int shutdown_grace_seconds(void)
{
    return (int)SHUTDOWN_GRACE_SECONDS;
}
