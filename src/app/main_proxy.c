#include "config.h"
#include "proxy_server.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    config_t cfg;
    config_init(&cfg);
    cfg.mode = SERVER_MODE_PROXY_ONLY;

    int has_config = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0) {
            fprintf(stderr, "%s: --backend is for server, not reverse-proxy\n",
                    argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--config") == 0)
            has_config = 1;
    }
    if (!has_config) {
        fprintf(stderr,
                "usage: %s --config FILE [--max-conns N] "
                "[--max-upstreams N] [--timeout N]\n",
                argv[0]);
        return 1;
    }

    if (config_parse_cli(argc, argv, &cfg) < 0) {
        fprintf(stderr, "invalid proxy configuration\n");
        return 1;
    }
    if (config_validate(&cfg) < 0)
        return 1;

    return proxy_server_run(&cfg) == 0 ? 0 : 1;
}
