#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_MAX_URI          4096
#define DEFAULT_MAX_HEADER       16384
#define DEFAULT_MAX_HEADERS      100
#define DEFAULT_MAX_BODY         (8 * 1024 * 1024)
#define DEFAULT_REQUEST_TIMEOUT  20
#define DEFAULT_KEEPALIVE_IDLE   5
#define DEFAULT_KEEPALIVE_MAX    100
#define DEFAULT_TASK_WORKERS     4
#define DEFAULT_TASK_QUEUE       64
#define DEFAULT_MAX_CONNS        1024

#define MAX_ROUTES               32
#define MAX_BACKENDS             16
#define MAX_BACKEND_HOST         256
#define MAX_ROUTE_PREFIX         256

typedef enum {
    ROUTE_STATIC = 1,
    ROUTE_PROXY = 2
} route_type_t;

typedef struct {
    char host[MAX_BACKEND_HOST];
    int port;
} backend_spec_t;

typedef struct {
    char prefix[MAX_ROUTE_PREFIX];
    route_type_t type;
    int strip_prefix;
    backend_spec_t backends[MAX_BACKENDS];
    int backend_count;
    int rr_index; /* runtime round-robin cursor */
} route_spec_t;

typedef struct {
    int port;
    char docroot[PATH_MAX];
    char cgi_dir[PATH_MAX];

    size_t max_uri;
    size_t max_header_bytes;
    size_t max_headers;
    size_t max_body;

    int request_timeout_sec;
    int keepalive_idle_sec;
    int keepalive_max_requests;

    int task_workers;
    int task_queue_size;
    int max_conns;
    int max_upstreams;

    route_spec_t routes[MAX_ROUTES];
    int route_count;

    int timeout_connect_sec;
    int timeout_response_sec;
    int timeout_body_sec;
    int pool_max_idle;
    int pool_idle_timeout_sec;
} config_t;

void config_init(config_t *cfg);
int config_parse_cli(int argc, char **argv, config_t *cfg);
int config_parse_file(config_t *cfg, const char *path);
int config_validate(const config_t *cfg);
route_spec_t *config_find_route(config_t *cfg, const char *path);

#endif
