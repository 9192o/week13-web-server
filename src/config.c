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
    cfg->task_workers = DEFAULT_TASK_WORKERS;
    cfg->task_queue_size = DEFAULT_TASK_QUEUE;
    cfg->max_conns = DEFAULT_MAX_CONNS;
    cfg->max_upstreams = 512;

    cfg->route_count = 0;
    cfg->timeout_connect_sec = 2;
    cfg->timeout_response_sec = 10;
    cfg->timeout_body_sec = 10;
    cfg->pool_max_idle = 8;
    cfg->pool_idle_timeout_sec = 30;
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
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            if (config_path != NULL || i + 1 >= argc)
                return -1;
            config_path = argv[++i];
        }
    }

    int file_loaded = 0;
    if (config_path != NULL) {
        if (config_parse_file(cfg, config_path) < 0)
            return -1;
        file_loaded = 1;
    }

    int pos = 0;
    int port_set = 0;
    int docroot_set = 0;
    int cgi_set = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            i++;
            continue;
        }

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
        } else if (strcmp(argv[i], "--task-workers") == 0) {
            i++;
            if (i >= argc ||
                parse_int_arg(argv[i], 1, 64, &cfg->task_workers) < 0)
                return -1;
        } else if (strcmp(argv[i], "--task-queue") == 0) {
            i++;
            if (i >= argc ||
                parse_int_arg(argv[i], 1, 65536,
                              &cfg->task_queue_size) < 0)
                return -1;
        } else if (strcmp(argv[i], "--max-conns") == 0) {
            i++;
            if (i >= argc ||
                parse_int_arg(argv[i], 1, 65536, &cfg->max_conns) < 0)
                return -1;
        } else if (strcmp(argv[i], "--max-upstreams") == 0) {
            i++;
            if (i >= argc ||
                parse_int_arg(argv[i], 1, 65536, &cfg->max_upstreams) < 0)
                return -1;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            return -1;
        } else if (pos == 0) {
            int port = 0;
            if (parse_int_arg(argv[i], 1, 65535, &port) < 0)
                return -1;
            cfg->port = port;
            port_set = 1;
            pos = 1;
        } else if (pos == 1) {
            if (copy_path(cfg->docroot, sizeof cfg->docroot, argv[i]) < 0)
                return -1;
            docroot_set = 1;
            pos = 2;
        } else if (pos == 2) {
            if (copy_path(cfg->cgi_dir, sizeof cfg->cgi_dir, argv[i]) < 0)
                return -1;
            cgi_set = 1;
            pos = 3;
        } else {
            return -1;
        }
    }

    if (!file_loaded) {
        if (!port_set || !docroot_set)
            return -1;
        if (!cgi_set)
            cfg->cgi_dir[0] = '\0';
    }

    return 0;
}

static int path_within(const char *root, const char *path) {
    size_t root_len = strlen(root);
    if (strncmp(root, path, root_len) != 0)
        return 0;
    return path[root_len] == '\0' || path[root_len] == '/';
}

route_spec_t *config_find_route(config_t *cfg, const char *path) {
    if (cfg == NULL || path == NULL)
        return NULL;

    route_spec_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < cfg->route_count; i++) {
        route_spec_t *r = &cfg->routes[i];
        size_t len = strlen(r->prefix);
        if (len == 0 || len < best_len)
            continue;
        if (strncmp(path, r->prefix, len) != 0)
            continue;
        if (r->prefix[len - 1] != '/' &&
            path[len] != '\0' && path[len] != '/')
            continue;
        best = r;
        best_len = len;
    }
    return best;
}

int config_validate(const config_t *cfg) {
    if (cfg->port < 1 || cfg->port > 65535) {
        fprintf(stderr, "invalid port: %d\n", cfg->port);
        return -1;
    }
    if (cfg->timeout_connect_sec < 1 || cfg->timeout_connect_sec > 3600 ||
        cfg->timeout_response_sec < 1 || cfg->timeout_response_sec > 3600 ||
        cfg->timeout_body_sec < 1 || cfg->timeout_body_sec > 3600 ||
        cfg->request_timeout_sec < 1 || cfg->request_timeout_sec > 3600) {
        fprintf(stderr, "invalid proxy timeout\n");
        return -1;
    }
    if (cfg->pool_max_idle < 0 || cfg->pool_max_idle > 1024 ||
        cfg->pool_idle_timeout_sec < 1 ||
        cfg->pool_idle_timeout_sec > 3600) {
        fprintf(stderr, "invalid pool setting\n");
        return -1;
    }
    if (cfg->route_count < 0 || cfg->route_count > MAX_ROUTES) {
        fprintf(stderr, "invalid route count\n");
        return -1;
    }
    for (int i = 0; i < cfg->route_count; i++) {
        const route_spec_t *r = &cfg->routes[i];
        if (r->prefix[0] != '/') {
            fprintf(stderr, "invalid route prefix: %s\n", r->prefix);
            return -1;
        }
        if (r->type != ROUTE_STATIC && r->type != ROUTE_PROXY) {
            fprintf(stderr, "invalid route type: %s\n", r->prefix);
            return -1;
        }
        if (cfg->proxy_only && r->type != ROUTE_PROXY) {
            fprintf(stderr,
                    "proxy-only server cannot serve static route: %s\n",
                    r->prefix);
            return -1;
        }
        if (r->backend_count < 0 || r->backend_count > MAX_BACKENDS) {
            fprintf(stderr, "invalid backend count: %s\n", r->prefix);
            return -1;
        }
        if (r->type == ROUTE_PROXY && r->backend_count == 0) {
            fprintf(stderr, "proxy route has no backend: %s\n", r->prefix);
            return -1;
        }
        if (r->type == ROUTE_STATIC && r->backend_count > 0) {
            fprintf(stderr, "static route has backend: %s\n", r->prefix);
            return -1;
        }
    }
    if (cfg->task_workers < 1 || cfg->task_workers > 64) {
        fprintf(stderr, "invalid task workers: %d\n", cfg->task_workers);
        return -1;
    }
    if (cfg->task_queue_size < 1 || cfg->task_queue_size > 65536) {
        fprintf(stderr, "invalid task queue size: %d\n",
                cfg->task_queue_size);
        return -1;
    }
    if (cfg->max_conns < 1 || cfg->max_conns > 65536) {
        fprintf(stderr, "invalid max conns: %d\n", cfg->max_conns);
        return -1;
    }
    if (cfg->max_upstreams < 1 || cfg->max_upstreams > 65536) {
        fprintf(stderr, "invalid max upstreams: %d\n", cfg->max_upstreams);
        return -1;
    }

    if (cfg->proxy_only) {
        if (cfg->route_count == 0) {
            fprintf(stderr, "proxy-only server needs at least one route\n");
            return -1;
        }
        return 0;
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

    char root[PATH_MAX];
    if (realpath(cfg->docroot, root) == NULL) {
        fprintf(stderr, "docroot '%s': %s\n", cfg->docroot, strerror(errno));
        return -1;
    }
    if (strcmp(root, "/") == 0) {
        fprintf(stderr, "docroot must not be '/'\n");
        return -1;
    }

    if (cfg->cgi_dir[0] != '\0') {
        char cgi_root[PATH_MAX];
        if (realpath(cfg->cgi_dir, cgi_root) == NULL) {
            fprintf(stderr, "cgi_dir '%s': %s\n",
                    cfg->cgi_dir, strerror(errno));
            return -1;
        }
        if (strcmp(cgi_root, "/") == 0 ||
            path_within(root, cgi_root) ||
            path_within(cgi_root, root)) {
            fprintf(stderr,
                    "docroot and cgi_dir must not contain each other\n");
            return -1;
        }
    }

    return 0;
}
