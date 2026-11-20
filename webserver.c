#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>

#define MAX_CON 5

static volatile sig_atomic_t stop_requested = 0;

static void handle_stop(int signo);
static int init_signals(sigset_t *wait_mask);
static int init_socket(void);
static int run_server(int server_fd, const sigset_t *wait_mask);

int main(void)
{
    sigset_t wait_mask;
    if (init_signals(&wait_mask) == -1)
    {
        return EXIT_FAILURE;
    }

    int server_fd = init_socket();
    if (server_fd == -1)
    {
        return EXIT_FAILURE;
    }

    int result = run_server(server_fd, &wait_mask);
    if (close(server_fd) == -1)
    {
        perror("Closing Server Socket Failed.");
        return EXIT_FAILURE;
    }

    if (result == EXIT_SUCCESS)
    {
        puts("Server stopped.");
    }
    return result;
}

static void handle_stop(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int init_signals(sigset_t *wait_mask)
{
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);

    /* Keep stop signals pending outside pselect's atomic wait. */
    if (sigprocmask(SIG_BLOCK, &blocked, wait_mask) == -1)
    {
        perror("Blocking Stop Signals Failed.");
        return -1;
    }
    sigdelset(wait_mask, SIGINT);
    sigdelset(wait_mask, SIGTERM);

    struct sigaction action = {0};
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1)
    {
        perror("Installing Stop Handler Failed.");
        return -1;
    }

    return 0;
}

static int init_socket(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        perror("Socket Creation Failed.");
        return -1;
    }

    /* A readiness notification must not lead to a blocking accept. */
    int flags = fcntl(server_fd, F_GETFL);
    if (flags == -1 ||
        fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        perror("Setting Nonblocking Socket Failed.");
        goto fail;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);

    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
    {
        fputs("Invalid Listen Address.\n", stderr);
        goto fail;
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        perror("Binding Socket Failed.");
        goto fail;
    }

    if (listen(server_fd, MAX_CON) == -1)
    {
        perror("Listen Failed.");
        goto fail;
    }

    return server_fd;

fail:
    close(server_fd);
    return -1;
}

static int run_server(int server_fd, const sigset_t *wait_mask)
{
    if (server_fd >= FD_SETSIZE)
    {
        fputs("Server FD Exceeds pselect Capacity.\n", stderr);
        return EXIT_FAILURE;
    }

    puts("Waiting for client...");
    fflush(stdout);

    while (!stop_requested)
    {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        int ready = pselect(server_fd + 1, &read_fds,
                            NULL, NULL, NULL, wait_mask);
        if (ready == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("Waiting for Client Failed.");
            return EXIT_FAILURE;
        }

        if (stop_requested)
        {
            break;
        }

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1)
        {
            if (errno == EINTR ||
                errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            perror("Accepting Client Failed.");
            return EXIT_FAILURE;
        }

        puts("Client Connected!");
        close(client_fd);
    }

    return EXIT_SUCCESS;
}
