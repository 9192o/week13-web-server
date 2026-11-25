#include "access_log.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>

static _Thread_local access_log_t *g_current;

static void copy_sanitized(char *dst, size_t dst_size, const char *src) {
    size_t j = 0;
    if (dst_size == 0)
        return;

    for (size_t i = 0; src != NULL && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        if (j + 1 >= dst_size)
            break;
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\')
            dst[j++] = '?';
        else
            dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

void access_log_init(access_log_t *log, const char *ip) {
    if (log == NULL)
        return;
    memset(log, 0, sizeof *log);
    log->active = 1;
    copy_sanitized(log->ip, sizeof log->ip, ip != NULL ? ip : "-");
}

void access_log_restart(access_log_t *log) {
    if (log == NULL)
        return;

    char ip[ACCESS_LOG_IP_MAX];
    memcpy(ip, log->ip, sizeof ip);
    ip[sizeof ip - 1] = '\0';
    access_log_init(log, ip);
}

void access_log_set_request(access_log_t *log, const char *method,
                            const char *path) {
    if (log == NULL || !log->active)
        return;
    copy_sanitized(log->method, sizeof log->method,
                   method != NULL ? method : "-");
    copy_sanitized(log->path, sizeof log->path,
                   path != NULL ? path : "-");
}

void access_log_set_status(access_log_t *log, int status) {
    if (log != NULL && log->active)
        log->status = status;
}

int access_log_get_status(const access_log_t *log) {
    return (log != NULL && log->active) ? log->status : 0;
}

void access_log_add_bytes(access_log_t *log, size_t n) {
    if (log != NULL && log->active)
        log->bytes += n;
}

void access_log_end(access_log_t *log) {
    if (log == NULL || !log->active)
        return;

    const char *method = log->method[0] != '\0' ? log->method : "-";
    const char *path = log->path[0] != '\0' ? log->path : "-";

    if (logger_running()) {
        logger_access(log->ip, method, path, log->status, log->bytes);
    } else {
        fprintf(stderr, "%s \"%s %s\" %d %zu\n",
                log->ip, method, path, log->status, log->bytes);
    }
    log->active = 0;
}

access_log_t *access_log_current(void) {
    return g_current;
}

void access_log_set_current(access_log_t *log) {
    g_current = log;
}
