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
#define DEFAULT_KEEPALIVE_IDLE   10
#define DEFAULT_KEEPALIVE_MAX    100
#define DEFAULT_TASK_WORKERS     4
#define DEFAULT_TASK_QUEUE       64
#define DEFAULT_MAX_CONNS        1024

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
} config_t;

void config_init(config_t *cfg);
int config_parse_cli(int argc, char **argv, config_t *cfg);
int config_validate(const config_t *cfg);

#endif
