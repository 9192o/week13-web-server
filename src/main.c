#include "config.h"
#include "server.h"

#include <stdio.h>

int main(int argc, char **argv) {
    config_t cfg;
    config_init(&cfg);

    if (config_parse_cli(argc, argv, &cfg) < 0) {
        fprintf(stderr,
                "usage: %s <port> <docroot> [cgi_dir] "
                "[--timeout N] [--max-uri N] [--max-header N] "
                "[--max-body N] [--keepalive-idle N] "
                "[--keepalive-max N]\n",
                argv[0]);
        return 1;
    }

    if (config_validate(&cfg) < 0)
        return 1;

    return server_run(&cfg) == 0 ? 0 : 1;
}
