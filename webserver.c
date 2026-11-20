#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <errno.h>

#define LISTEN_BACKLOG 5

static int create_listener(void);
static int serve_clients(int listener_fd);
static void receive_client_data(int client_fd);

int main(void)
{
    int listener_fd = create_listener();
    if (listener_fd == -1)
    {
        return EXIT_FAILURE;
    }

    int exit_status = serve_clients(listener_fd);
    if (close(listener_fd) == -1)
    {
        perror("Closing Server Socket Failed.");
        return EXIT_FAILURE;
    }

    return exit_status;
}

static int create_listener(void)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd == -1)
    {
        perror("Socket Creation Failed.");
        return -1;
    }

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(8080);

    if (inet_pton(AF_INET, "127.0.0.1", &listen_addr.sin_addr) != 1)
    {
        fputs("Invalid Listen Address.\n", stderr);
        goto fail;
    }

    if (bind(listener_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) == -1)
    {
        perror("Binding Socket Failed.");
        goto fail;
    }

    if (listen(listener_fd, LISTEN_BACKLOG) == -1)
    {
        perror("Listen Failed.");
        goto fail;
    }

    return listener_fd;

fail:
    close(listener_fd);
    return -1;
}

static int serve_clients(int listener_fd)
{
    puts("Waiting for client...");
    fflush(stdout);

    while (1)
    {
        int client_fd = accept(listener_fd, NULL, NULL);
        if (client_fd == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("Accepting Client Failed.");
            return EXIT_FAILURE;
        }

        puts("Client Connected!");
        receive_client_data(client_fd);
        close(client_fd);
    }
}

static void receive_client_data(int client_fd)
{
    char receive_buffer[1024];
    ssize_t bytes_received;
    do
    {
        bytes_received = recv(client_fd, receive_buffer, sizeof(receive_buffer), 0);
    } while (bytes_received == -1 && errno == EINTR);

    if (bytes_received > 0)
    {
        printf("Received %zd bytes:\n", bytes_received);
        fwrite(receive_buffer, 1, (size_t)bytes_received, stdout);
        putchar('\n');
        fflush(stdout);
    }
    else if (bytes_received == -1)
    {
        perror("Receiving Data Failed.");
    }
}
