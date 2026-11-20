#define _POSIX_C_SOURCE 200809L

#include "webserver.h"
#include "app.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <errno.h>

#define LISTEN_BACKLOG 5
#define BUF_SIZE 1024
#define DEFAULT_PORT 8080
#define DEFAULT_IP "127.0.0.1"

static int create_listener(void);
static int serve_clients(int listener_fd);
static void handle_client(int client_fd);

int webserver_run(void)
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
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd == -1)
    {
        perror("Socket Creation Failed.");
        return -1;
    }

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(DEFAULT_PORT);

    if (inet_pton(AF_INET, DEFAULT_IP, &listen_addr.sin_addr) != 1)
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
                continue;

            perror("Accepting Client Failed.");
            return EXIT_FAILURE;
        }

        puts("Client Connected!");
        handle_client(client_fd);
        close(client_fd);
    }
}

static void handle_client(int client_fd)
{
    char receive_buffer[HTTP_REQUEST_LINE_MAX];
    size_t total_received = 0;
    const char *response;
    while (1)
    {
        size_t receive_length = sizeof(receive_buffer) - total_received;
        if (receive_length > BUF_SIZE)
            receive_length = BUF_SIZE;

        ssize_t bytes_received = recv(client_fd, receive_buffer + total_received, receive_length, 0);
        if (bytes_received == -1)
        {
            if (errno == EINTR)
                continue;
            perror("Receiving Data Failed.");
            return;
        }
        if (bytes_received == 0)
        {
            response = HTTP_BAD_REQUEST_RESPONSE;
            break;
        }

        total_received += (size_t)bytes_received;
        struct http_request_line_result result =
            http_parse_request_line(receive_buffer, total_received);
        if (result.status == HTTP_PARSE_NEED_MORE)
            continue;
        if (result.status == HTTP_PARSE_ERROR)
        {
            response = result.error_status == 414
                           ? HTTP_URI_TOO_LONG_RESPONSE
                           : HTTP_BAD_REQUEST_RESPONSE;
            break;
        }

        response = app_handle_data(receive_buffer, result.consumed);
        break;
    }

    size_t response_length = strlen(response);
    size_t total_sent = 0;
    while (total_sent < response_length)
    {
        ssize_t bytes_sent = send(client_fd, response + total_sent, response_length - total_sent, MSG_NOSIGNAL);
        if (bytes_sent == -1)
        {
            if (errno == EINTR)
                continue;

            perror("Sending Data Failed.");
            return;
        }
        if (bytes_sent == 0)
        {
            fputs("Sending Data Made No Progress.\n", stderr);
            return;
        }
        total_sent += (size_t)bytes_sent;
    }
}
