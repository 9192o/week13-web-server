#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s) {
    while (*s != '\0' && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return s;
}

static int parse_int_value(const char *s, int min, int max, int *out) {
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_size_value(const char *s, size_t max, size_t *out) {
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v == 0 || v > max)
        return -1;
    *out = (size_t)v;
    return 0;
}

static int parse_backend(const char *s, backend_spec_t *out) {
    const char *colon = strrchr(s, ':');
    if (colon == NULL || colon == s)
        return -1;

    size_t host_len = (size_t)(colon - s);
    if (host_len >= sizeof out->host)
        return -1;

    memcpy(out->host, s, host_len);
    out->host[host_len] = '\0';

    int port = 0;
    if (parse_int_value(colon + 1, 1, 65535, &port) < 0)
        return -1;
    out->port = port;
    return 0;
}

static int find_route(config_t *cfg, const char *prefix) {
    for (int i = 0; i < cfg->route_count; i++) {
        if (strcmp(cfg->routes[i].prefix, prefix) == 0)
            return i;
    }
    return -1;
}

static int parse_route_section(config_t *cfg, const char *path, int line_no,
                               const char *section, int *route_index) {
    char buf[256];
    if (snprintf(buf, sizeof buf, "%s", section) < 0)
        return -1;

    char *save = NULL;
    char *kind = strtok_r(buf, " \t", &save);
    char *prefix = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    if (kind == NULL || strcmp(kind, "route") != 0 ||
        prefix == NULL || extra != NULL || prefix[0] != '/') {
        fprintf(stderr, "%s:%d: invalid route section\n", path, line_no);
        return -1;
    }
    if (strlen(prefix) >= MAX_ROUTE_PREFIX) {
        fprintf(stderr, "%s:%d: route prefix too long\n", path, line_no);
        return -1;
    }
    if (find_route(cfg, prefix) >= 0) {
        fprintf(stderr, "%s:%d: duplicate route %s\n", path, line_no, prefix);
        return -1;
    }
    if (cfg->route_count >= MAX_ROUTES) {
        fprintf(stderr, "%s:%d: too many routes\n", path, line_no);
        return -1;
    }

    route_spec_t *r = &cfg->routes[cfg->route_count];
    memset(r, 0, sizeof *r);
    snprintf(r->prefix, sizeof r->prefix, "%s", prefix);
    *route_index = cfg->route_count;
    cfg->route_count++;
    return 0;
}

int config_parse_file(config_t *cfg, const char *path) {
    if (cfg == NULL || path == NULL)
        return -1;

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "config '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[1024];
    int line_no = 0;
    int route_index = -1;
    int rc = 0;

    while (fgets(line, sizeof line, f) != NULL) {
        line_no++;
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n') {
            fprintf(stderr, "%s:%d: line too long\n", path, line_no);
            rc = -1;
            break;
        }

        char *s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end == NULL || end[1] != '\0') {
                fprintf(stderr, "%s:%d: invalid section\n", path, line_no);
                rc = -1;
                break;
            }
            *end = '\0';
            if (parse_route_section(cfg, path, line_no, s + 1,
                                    &route_index) < 0) {
                rc = -1;
                break;
            }
            continue;
        }

        char *save = NULL;
        char *key = strtok_r(s, " \t", &save);
        char *arg1 = strtok_r(NULL, " \t", &save);
        char *arg2 = strtok_r(NULL, " \t", &save);
        char *extra = strtok_r(NULL, " \t", &save);

        if (key == NULL || arg1 == NULL || extra != NULL) {
            fprintf(stderr, "%s:%d: invalid line\n", path, line_no);
            rc = -1;
            break;
        }

        if (route_index >= 0) {
            route_spec_t *r = &cfg->routes[route_index];
            if (strcmp(key, "type") == 0) {
                if (arg2 != NULL) {
                    fprintf(stderr, "%s:%d: type takes one value\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
                if (strcmp(arg1, "static") == 0)
                    r->type = ROUTE_STATIC;
                else if (strcmp(arg1, "proxy") == 0)
                    r->type = ROUTE_PROXY;
                else {
                    fprintf(stderr, "%s:%d: invalid route type\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
            } else if (strcmp(key, "backend") == 0) {
                if (arg2 != NULL) {
                    fprintf(stderr, "%s:%d: backend takes one value\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
                if (r->backend_count >= MAX_BACKENDS) {
                    fprintf(stderr, "%s:%d: too many backends\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
                backend_spec_t *b = &r->backends[r->backend_count];
                if (parse_backend(arg1, b) < 0) {
                    fprintf(stderr, "%s:%d: invalid backend\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
                r->backend_count++;
            } else {
                fprintf(stderr, "%s:%d: unknown route key '%s'\n",
                        path, line_no, key);
                rc = -1;
                break;
            }
            continue;
        }

        if (strcmp(key, "listen") == 0) {
            if (parse_int_value(arg1, 1, 65535, &cfg->port) < 0) {
                fprintf(stderr, "%s:%d: invalid listen port\n", path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "docroot") == 0) {
            if (snprintf(cfg->docroot, sizeof cfg->docroot, "%s", arg1) < 0 ||
                strlen(arg1) >= sizeof cfg->docroot) {
                fprintf(stderr, "%s:%d: docroot too long\n", path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "cgi_dir") == 0) {
            if (snprintf(cfg->cgi_dir, sizeof cfg->cgi_dir, "%s", arg1) < 0 ||
                strlen(arg1) >= sizeof cfg->cgi_dir) {
                fprintf(stderr, "%s:%d: cgi_dir too long\n", path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "max_conns") == 0) {
            if (parse_int_value(arg1, 1, 65536, &cfg->max_conns) < 0) {
                fprintf(stderr, "%s:%d: invalid max_conns\n", path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "task_workers") == 0) {
            if (parse_int_value(arg1, 1, 64, &cfg->task_workers) < 0) {
                fprintf(stderr, "%s:%d: invalid task_workers\n",
                        path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "task_queue") == 0) {
            if (parse_int_value(arg1, 1, 65536, &cfg->task_queue_size) < 0) {
                fprintf(stderr, "%s:%d: invalid task_queue\n", path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "keepalive_idle") == 0) {
            if (parse_int_value(arg1, 1, 3600,
                                &cfg->keepalive_idle_sec) < 0) {
                fprintf(stderr, "%s:%d: invalid keepalive_idle\n",
                        path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "keepalive_max") == 0) {
            if (parse_int_value(arg1, 1, 1000000,
                                &cfg->keepalive_max_requests) < 0) {
                fprintf(stderr, "%s:%d: invalid keepalive_max\n",
                        path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "max_uri") == 0) {
            if (parse_size_value(arg1, 4096, &cfg->max_uri) < 0) {
                fprintf(stderr, "%s:%d: invalid max_uri\n", path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "max_header") == 0) {
            if (parse_size_value(arg1, 16384, &cfg->max_header_bytes) < 0) {
                fprintf(stderr, "%s:%d: invalid max_header\n", path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "max_body") == 0) {
            if (parse_size_value(arg1, (size_t)1024 * 1024 * 1024,
                                 &cfg->max_body) < 0) {
                fprintf(stderr, "%s:%d: invalid max_body\n", path, line_no);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "timeout") == 0) {
            if (arg2 == NULL) {
                fprintf(stderr, "%s:%d: timeout needs a value\n",
                        path, line_no);
                rc = -1;
                break;
            }
            int v = 0;
            if (strcmp(arg1, "connect") == 0) {
                if (parse_int_value(arg2, 1, 3600, &v) < 0) {
                    fprintf(stderr, "%s:%d: invalid value\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
                cfg->timeout_connect_sec = v;
            } else if (strcmp(arg1, "response") == 0) {
                if (parse_int_value(arg2, 1, 3600, &v) < 0) {
                    fprintf(stderr, "%s:%d: invalid value\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
                cfg->timeout_response_sec = v;
            } else if (strcmp(arg1, "body") == 0) {
                if (parse_int_value(arg2, 1, 3600, &v) < 0) {
                    fprintf(stderr, "%s:%d: invalid value\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
                cfg->timeout_body_sec = v;
            } else {
                fprintf(stderr, "%s:%d: unknown timeout '%s'\n",
                        path, line_no, arg1);
                rc = -1;
                break;
            }
        } else if (strcmp(key, "pool") == 0) {
            if (arg2 == NULL) {
                fprintf(stderr, "%s:%d: pool needs a value\n", path, line_no);
                rc = -1;
                break;
            }
            int v = 0;
            if (strcmp(arg1, "max_idle") == 0) {
                if (parse_int_value(arg2, 0, 1024, &v) < 0) {
                    fprintf(stderr, "%s:%d: invalid value\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
                cfg->pool_max_idle = v;
            } else if (strcmp(arg1, "idle_timeout") == 0) {
                if (parse_int_value(arg2, 1, 3600, &v) < 0) {
                    fprintf(stderr, "%s:%d: invalid value\n",
                            path, line_no);
                    rc = -1;
                    break;
                }
                cfg->pool_idle_timeout_sec = v;
            } else {
                fprintf(stderr, "%s:%d: unknown pool key '%s'\n",
                        path, line_no, arg1);
                rc = -1;
                break;
            }
        } else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, line_no, key);
            rc = -1;
            break;
        }
    }

    if (ferror(f) && rc == 0) {
        fprintf(stderr, "config '%s': read error\n", path);
        rc = -1;
    }
    fclose(f);
    return rc;
}
