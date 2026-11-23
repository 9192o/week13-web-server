#define _POSIX_C_SOURCE 200809L

#include "webserver.h"
#include "app.h"
#include "connection.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>

#include <errno.h>

#define LISTEN_BACKLOG 5
#define BUF_SIZE 1024
#define LISTEN_IP "127.0.0.1"

static int create_listener(unsigned short port);
static int serve_clients(int listener_fd, unsigned short port, int root_fd);
static void handle_client(int client_fd, int root_fd, int log_fd);
static int send_application_response(int client_fd, const struct http_response *response,
                                     int head, conn_access_record_t *access);

int webserver_run(unsigned short port, int root_fd)
{
    struct sigaction ignored = {.sa_handler = SIG_IGN};
    if (sigemptyset(&ignored.sa_mask) < 0 || sigaction(SIGPIPE, &ignored, NULL) < 0)
    {
        perror("Setting SIGPIPE policy failed");
        return EXIT_FAILURE;
    }
    int listener_fd = create_listener(port);
    if (listener_fd == -1)
    {
        return EXIT_FAILURE;
    }

    int exit_status = serve_clients(listener_fd, port, root_fd);
    if (close(listener_fd) == -1)
    {
        perror("Closing Server Socket Failed.");
        return EXIT_FAILURE;
    }

    return exit_status;
}

static int create_listener(unsigned short port)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd == -1)
    {
        perror("Socket Creation Failed.");
        return -1;
    }

    int opt = 1;
    if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        perror("Setting Socket Option Failed.");
        goto fail;
    }

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, LISTEN_IP, &listen_addr.sin_addr) != 1)
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

static int serve_clients(int listener_fd, unsigned short port, int root_fd)
{
    int log_fd = connection_log_fd_prepare(STDOUT_FILENO);
    if (log_fd < 0)
    {
        perror("Opening access log failed");
        return EXIT_FAILURE;
    }
    if (connection_log_ready(log_fd, port) < 0)
    {
        close(log_fd);
        return EXIT_FAILURE;
    }

    while (1)
    {
        int client_fd = accept(listener_fd, NULL, NULL);
        if (client_fd == -1)
        {
            if (errno == EINTR)
                continue;

            perror("Accepting Client Failed.");
            close(log_fd);
            return EXIT_FAILURE;
        }

        handle_client(client_fd, root_fd, log_fd);
    }
}

static void handle_client(int client_fd, int root_fd, int log_fd)
{
    char receive_buffer[HTTP_REQUEST_HEAD_MAX];
    size_t total_received = 0;
    struct http_request_line_result line = {.status = HTTP_PARSE_NEED_MORE};
    struct http_headers_result headers;
    struct http_body_parser body = {0};
    struct http_request request;
    struct http_response app_response = {.status = 200, .file_fd = -1};
    char location[HTTP_REQUEST_LINE_MAX + 2];
    char method_log[HTTP_REQUEST_LINE_MAX];
    char path_log[HTTP_REQUEST_LINE_MAX];
    conn_access_record_t access = {.status = -1};
    struct timespec accepted;
    int head = 0;
    int error_status = 0;
    if (connection_deadline_now(&accepted) < 0)
    {
        if (log_fd >= 0)
            close(client_fd);
        return;
    }
    struct timespec total_deadline, header_deadline;
    connection_deadline_add(&accepted, PHASE1_TOTAL_MS, NULL, &total_deadline);
    connection_deadline_add(&accepted, PHASE1_HEADER_MS, &total_deadline, &header_deadline);

    while (1)
    {
        size_t receive_length = sizeof(receive_buffer) - total_received;
        if (receive_length > BUF_SIZE)
            receive_length = BUF_SIZE;
        size_t bytes_received;
        conn_result_t io = connection_recv_some(client_fd, receive_buffer + total_received,
                                                receive_length, &header_deadline, &bytes_received);
        if (io == CONN_RESULT_TIMEOUT || io == CONN_RESULT_EOF)
        {
            error_status = io == CONN_RESULT_TIMEOUT ? 408 : 400;
            goto respond_error;
        }
        if (io != CONN_RESULT_OK)
            goto cleanup;

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
        if (!access.method)
        {
            memcpy(method_log, line.request.method.data, line.request.method.length);
            method_log[line.request.method.length] = '\0';
            access.method = method_log;
        }
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
    if (target.path.length)
    {
        memcpy(path_log, target.path.data, target.path.length);
        path_log[target.path.length] = '\0';
        access.path = path_log;
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
    struct router_result route = app_match_request(&request);
    if (!route.handler)
    {
        app_response = app_handle_request(root_fd, &request, &route, location);
        if (app_response.status != 200)
            goto respond_app;
    }
    size_t body_offset = line.consumed + headers.consumed;
    struct http_body_result result =
        http_parse_body(&body, receive_buffer + body_offset, total_received - body_offset);
    int sent_continue = 0;
    struct timespec body_deadline;
    if (connection_deadline_from_now(PHASE1_IDLE_MS, &total_deadline, &body_deadline) < 0)
        goto cleanup;
    while (result.status == HTTP_PARSE_NEED_MORE)
    {
        if (body.expect_continue && !sent_continue)
        {
            size_t ignored;
            if (connection_send_all(client_fd, HTTP_CONTINUE_RESPONSE, strlen(HTTP_CONTINUE_RESPONSE),
                                    &body_deadline, &ignored) != CONN_RESULT_OK)
                goto cleanup;
            sent_continue = 1;
        }
        char buffer[BUF_SIZE];
        size_t received;
        conn_result_t io = connection_recv_some(client_fd, buffer, sizeof(buffer),
                                                &body_deadline, &received);
        if (io == CONN_RESULT_TIMEOUT || io == CONN_RESULT_EOF)
        {
            error_status = io == CONN_RESULT_TIMEOUT ? 408 : 400;
            goto respond_error;
        }
        if (io != CONN_RESULT_OK ||
            connection_deadline_from_now(PHASE1_IDLE_MS, &total_deadline, &body_deadline) < 0)
            goto cleanup;
        result = http_parse_body(&body, buffer, (size_t)received);
    }
    if (result.status == HTTP_PARSE_ERROR)
    {
        error_status = result.error_status;
        goto respond_error;
    }
    request.body = (struct http_slice){body.data, body.length};
    if (route.handler)
        app_response = app_handle_request(root_fd, &request, &route, location);

respond_app:
    access.status = app_response.status;
    access.complete = send_application_response(client_fd, &app_response, head, &access) == 0;
    goto cleanup;

respond_error:
    {
        const char *reason = http_reason_phrase(error_status);
        struct http_response error_response = {
            .status = error_status,
            .content_type = "text/plain; charset=utf-8",
            .body = {reason, strlen(reason)},
            .file_fd = -1};
        access.status = error_status;
        access.complete = send_application_response(client_fd, &error_response, head, &access) == 0;
    }
cleanup:
    if (app_response.file_fd >= 0)
        close(app_response.file_fd);
    http_body_destroy(&body);
    if (log_fd >= 0)
    {
        if (access.complete)
        {
            size_t discarded;
            shutdown(client_fd, SHUT_WR);
            connection_drain(client_fd, HTTP_REQUEST_HEAD_MAX, &discarded);
        }
        close(client_fd);
        access.duration_ms = connection_stopwatch_elapsed_ms(&accepted);
        (void)connection_log_access(log_fd, &access);
    }
}

static int send_bytes(int fd, const char *data, size_t length,
                      const struct timespec *deadline, conn_access_record_t *access)
{
    size_t submitted = 0;
    if (!length)
        return 0;
    conn_result_t result = connection_send_all(fd, data, length, deadline, &submitted);
    access->bytes_sent += submitted;
    return result == CONN_RESULT_OK ? 0 : -1;
}

static int send_application_response(int client_fd, const struct http_response *response,
                                     int head, conn_access_record_t *access)
{
    struct timespec deadline;
    if (connection_deadline_from_now(PHASE1_SEND_MS, NULL, &deadline) < 0)
        return -1;
    char header[HTTP_REQUEST_LINE_MAX + 512];
    size_t length = http_format_response_head(response, header, sizeof(header));
    if (!length || send_bytes(client_fd, header, length, &deadline, access) < 0)
        return -1;
    if (head)
        return 0;
    if (response->file_fd < 0)
        return send_bytes(client_fd, response->body.data, response->body.length, &deadline, access);

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
        if (send_bytes(client_fd, buffer, (size_t)received, &deadline, access) < 0)
            return -1;
        remaining -= (size_t)received;
    }
    return 0;
}
