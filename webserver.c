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
static int send_response(int client_fd, const char *response);
static int send_bytes(int client_fd, const char *data, size_t length);
static int send_application_response(int client_fd, const struct http_response *response, int head);

static int send_error_response(int client_fd, int status, int head)
{
    const char *reason = http_reason_phrase(status);
    struct http_response response = {
        .status = status,
        .content_type = "text/plain; charset=utf-8",
        .body = {reason, strlen(reason)},
        .file_fd = -1};
    return send_application_response(client_fd, &response, head);
}

static ssize_t receive_data(int client_fd, char *buffer, size_t length)
{
    ssize_t received;
    do
    {
        received = recv(client_fd, buffer, length, 0);
    } while (received == -1 && errno == EINTR);
    if (received == -1)
        perror("Receiving Data Failed.");
    return received;
}

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
    char receive_buffer[HTTP_REQUEST_HEAD_MAX];
    size_t total_received = 0;
    struct http_request_line_result line = {.status = HTTP_PARSE_NEED_MORE};
    struct http_headers_result headers;
    struct http_body_parser body = {0};
    struct http_request request;
    struct http_response app_response = {.status = 200, .file_fd = -1};
    int head = 0;
    int error_status = 0;

    while (1)
    {
        size_t receive_length = sizeof(receive_buffer) - total_received;
        if (receive_length > BUF_SIZE)
            receive_length = BUF_SIZE;
        ssize_t bytes_received = receive_data(client_fd, receive_buffer + total_received, receive_length);
        if (bytes_received < 0)
            goto cleanup;
        if (bytes_received == 0)
        {
            error_status = 400;
            goto respond_error;
        }

        total_received += (size_t)bytes_received;
        if (line.status == HTTP_PARSE_NEED_MORE)
            line = http_parse_request_line(receive_buffer, total_received);
        if (line.status == HTTP_PARSE_NEED_MORE)
            continue;
        if (line.status == HTTP_PARSE_ERROR)
        {
            error_status = line.error_status;
            goto respond_error;
        }
        head = line.request.method.length == 4 &&
               memcmp(line.request.method.data, "HEAD", 4) == 0;
        headers = http_parse_headers(receive_buffer + line.consumed,
                                     total_received - line.consumed, line.consumed);
        if (headers.status == HTTP_PARSE_NEED_MORE)
            continue;
        if (headers.status == HTTP_PARSE_ERROR)
        {
            error_status = headers.error_status;
            goto respond_error;
        }
        break;
    }

    struct http_target target;
    int error = http_validate_request_head(&line.request, &headers.headers, &target);
    if (error)
    {
        error_status = error;
        goto respond_error;
    }
    error = http_body_init(&body, &headers.headers);
    if (error)
    {
        error_status = error;
        goto respond_error;
    }
    request = (struct http_request){
        .line = line.request,
        .headers = headers.headers,
        .target = target};
    struct router_result route = router_match_request(&request);
    if (route.handler != ROUTER_ECHO)
    {
        app_response = app_handle_request(&request, route);
        if (app_response.status != 200)
            goto respond_app;
    }
    size_t body_offset = line.consumed + headers.consumed;
    struct http_body_result result =
        http_parse_body(&body, receive_buffer + body_offset, total_received - body_offset);
    int sent_continue = 0;
    while (result.status == HTTP_PARSE_NEED_MORE)
    {
        if (body.expect_continue && !sent_continue)
        {
            if (send_response(client_fd, HTTP_CONTINUE_RESPONSE) == -1)
                goto cleanup;
            sent_continue = 1;
        }
        char buffer[BUF_SIZE];
        ssize_t received = receive_data(client_fd, buffer, sizeof(buffer));
        if (received < 0)
            goto cleanup;
        if (received == 0)
        {
            error_status = 400;
            goto respond_error;
        }
        result = http_parse_body(&body, buffer, (size_t)received);
    }
    if (result.status == HTTP_PARSE_ERROR)
    {
        error_status = result.error_status;
        goto respond_error;
    }
    request.body = (struct http_slice){body.data, body.length};
    if (route.handler == ROUTER_ECHO)
        app_response = app_handle_request(&request, route);

respond_app:
    send_application_response(client_fd, &app_response, head);
    goto cleanup;

respond_error:
    send_error_response(client_fd, error_status, head);
cleanup:
    if (app_response.file_fd >= 0)
        close(app_response.file_fd);
    http_body_destroy(&body);
}

static int send_response(int client_fd, const char *response)
{
    return send_bytes(client_fd, response, strlen(response));
}

static int send_bytes(int client_fd, const char *data, size_t length)
{
    size_t total_sent = 0;
    while (total_sent < length)
    {
        ssize_t bytes_sent = send(client_fd, data + total_sent, length - total_sent, MSG_NOSIGNAL);
        if (bytes_sent == -1)
        {
            if (errno == EINTR)
                continue;

            perror("Sending Data Failed.");
            return -1;
        }
        if (bytes_sent == 0)
        {
            fputs("Sending Data Made No Progress.\n", stderr);
            return -1;
        }
        total_sent += (size_t)bytes_sent;
    }
    return 0;
}

static int send_application_response(int client_fd, const struct http_response *response, int head)
{
    char header[512];
    size_t length = http_format_response_head(response, header, sizeof(header));
    if (!length || send_bytes(client_fd, header, length) < 0)
        return -1;
    if (head)
        return 0;
    if (response->file_fd < 0)
        return send_bytes(client_fd, response->body.data, response->body.length);

    char buffer[BUF_SIZE];
    size_t remaining = response->body.length;
    while (remaining)
    {
        size_t count = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t received = read(response->file_fd, buffer, count);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
        {
            fputs("Reading Response File Failed.\n", stderr);
            return -1;
        }
        if (send_bytes(client_fd, buffer, (size_t)received) < 0)
            return -1;
        remaining -= (size_t)received;
    }
    return 0;
}
