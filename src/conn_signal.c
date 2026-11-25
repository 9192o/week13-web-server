#include "conn_common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_signal_count = 0;
static int g_signal_pipe[2] = {-1, -1};

static void on_signal(int sig) {
    (void)sig;
    if (++g_signal_count >= 2)
        _exit(1);
    g_stop = 1;
    if (g_signal_pipe[1] >= 0) {
        char byte = 'x';
        ssize_t n = write(g_signal_pipe[1], &byte, 1);
        (void)n;
    }
}

int conn_common_setup_signals(void) {
    if (pipe(g_signal_pipe) < 0)
        return -1;
    if (conn_common_set_nonblocking(g_signal_pipe[0]) < 0 ||
        conn_common_set_nonblocking(g_signal_pipe[1]) < 0 ||
        conn_common_set_cloexec(g_signal_pipe[0]) < 0 ||
        conn_common_set_cloexec(g_signal_pipe[1]) < 0) {
        conn_common_close_signals();
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0 ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        conn_common_close_signals();
        return -1;
    }
    return 0;
}

int conn_common_signal_fd(void) {
    return g_signal_pipe[0];
}

void conn_common_close_signals(void) {
    if (g_signal_pipe[0] >= 0)
        close(g_signal_pipe[0]);
    if (g_signal_pipe[1] >= 0)
        close(g_signal_pipe[1]);
    g_signal_pipe[0] = g_signal_pipe[1] = -1;
}

int conn_common_stopping(void) {
    return g_stop != 0;
}

int server_is_stopping(void) {
    return g_stop != 0;
}
