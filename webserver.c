#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define MAX_CON 5

int init_socket(void);
void run_server(int server_fd);

int main(void)
{
    int server_fd;

    server_fd = init_socket();
    run_server(server_fd);

    return 1;
}

int init_socket(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        perror("Socket Creation Failed.");
        exit(1);
    }

    static struct sockaddr_in addr = {
        0,
    };

    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);

    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
    {
        perror("inet_pton Failed.");
        close(server_fd);
        exit(1);
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        perror("Binding Socket Failed.");
        close(server_fd);
        exit(1);
    }

    if (listen(server_fd, MAX_CON) == -1)
    {
        perror("Listen Failed.");
        close(server_fd);
        exit(1);
    }

    puts("Waiting for client...");

    return server_fd;
}

void run_server(int server_fd)
{
    while (1)
    {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1)
        {
            perror("Accepting Client Failed.");
            close(server_fd);
            exit(1);
        }

        puts("Client Connected!");
        close(client_fd);
    }
}