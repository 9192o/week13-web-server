#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <errno.h>

#define MAX_CON 5

static int init_socket(void);
static int run_server(int server_fd);

int main(void)
{
    int server_fd = init_socket();
    if (server_fd == -1)
    {
        return EXIT_FAILURE;
    }

    int result = run_server(server_fd);
    if (close(server_fd) == -1)
    {
        perror("Closing Server Socket Failed.");
        return EXIT_FAILURE;
    }

    return result;
}

static int init_socket(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        perror("Socket Creation Failed.");
        return -1;
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

static int run_server(int server_fd)
{
    puts("Waiting for client...");
    fflush(stdout);

    while (1)
    {
        int client_fd = accept(server_fd, NULL, NULL);
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
        close(client_fd);
    }
}
