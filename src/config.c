#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void config_init(config_t *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->port = 8080;
    snprintf(cfg->docroot, sizeof cfg->docroot, "%s", "./www");
    cfg->cgi_dir[0] = '\0';

    cfg->max_uri = DEFAULT_MAX_URI;
    cfg->max_header_bytes = DEFAULT_MAX_HEADER;
    cfg->max_headers = DEFAULT_MAX_HEADERS;
    cfg->max_body = DEFAULT_MAX_BODY;
    cfg->request_timeout_sec = DEFAULT_REQUEST_TIMEOUT;
    cfg->keepalive_idle_sec = DEFAULT_KEEPALIVE_IDLE;
    cfg->keepalive_max_requests = DEFAULT_KEEPALIVE_MAX;
}

static int copy_path(char *dst, size_t dst_size, const char *src) {
    int n = snprintf(dst, dst_size, "%s", src);
    return (n < 0 || (size_t)n >= dst_size) ? -1 : 0;
}

static int parse_int_arg(const char *s, int min, int max, int *out) {
    errno = 0;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value < min || value > max)
        return -1;
    *out = (int)value;
    return 0;
}

static int parse_size_arg(const char *s, size_t max, size_t *out) {
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value == 0 || value > max)
        return -1;
    *out = (size_t)value;
    return 0;
}

int config_parse_cli(int argc, char **argv, config_t *cfg) {
    if (argc < 3)
        return -1;

    int port = 0;
    if (parse_int_arg(argv[1], 1, 65535, &port) < 0)
        return -1;
    cfg->port = port;

    if (copy_path(cfg->docroot, sizeof cfg->docroot, argv[2]) < 0)
        return -1;

    cfg->cgi_dir[0] = '\0';

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--timeout") == 0) {
            i++;
            if (i >= argc ||
                parse_int_arg(argv[i], 1, 3600,
                              &cfg->request_timeout_sec) < 0)
                return -1;
        } else if (strcmp(argv[i], "--max-uri") == 0) {
            i++;
            if (i >= argc || parse_size_arg(argv[i], 4096, &cfg->max_uri) < 0)
                return -1;
        } else if (strcmp(argv[i], "--max-header") == 0) {
            i++;
            if (i >= argc ||
                parse_size_arg(argv[i], 16384,
                               &cfg->max_header_bytes) < 0)
                return -1;
        } else if (strcmp(argv[i], "--max-body") == 0) {
            i++;
            if (i >= argc ||
                parse_size_arg(argv[i], (size_t)1024 * 1024 * 1024,
                               &cfg->max_body) < 0)
                return -1;
        } else if (strcmp(argv[i], "--keepalive-idle") == 0) {
            i++;
            if (i >= argc ||
                parse_int_arg(argv[i], 1, 3600,
                              &cfg->keepalive_idle_sec) < 0)
                return -1;
        } else if (strcmp(argv[i], "--keepalive-max") == 0) {
            i++;
            if (i >= argc ||
                parse_int_arg(argv[i], 1, 1000000,
                              &cfg->keepalive_max_requests) < 0)
                return -1;
        } else if (cfg->cgi_dir[0] == '\0') {
            if (copy_path(cfg->cgi_dir, sizeof cfg->cgi_dir, argv[i]) < 0)
                return -1;
        } else {
            return -1;
        }
    }

    return 0;
}

int config_validate(const config_t *cfg) {
    if (cfg->port < 1 || cfg->port > 65535) {
        fprintf(stderr, "invalid port: %d\n", cfg->port);
        return -1;
    }

    struct stat st;
    if (stat(cfg->docroot, &st) < 0) {
        fprintf(stderr, "docroot '%s': %s\n", cfg->docroot, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "docroot '%s' is not a directory\n", cfg->docroot);
        return -1;
    }

    if (cfg->cgi_dir[0] != '\0') {
        if (stat(cfg->cgi_dir, &st) < 0) {
            fprintf(stderr, "cgi_dir '%s': %s\n", cfg->cgi_dir, strerror(errno));
            return -1;
        }
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "cgi_dir '%s' is not a directory\n", cfg->cgi_dir);
            return -1;
        }
    }

    return 0;
}
