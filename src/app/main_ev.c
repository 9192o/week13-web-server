#include "config.h"
#include "server_ev.h"

#include <stdio.h>

int main(int argc, char **argv) {
    config_t cfg;
    config_init(&cfg);

    if (config_parse_cli(argc, argv, &cfg) < 0) {
        fprintf(stderr,
                "usage: %s <port> <docroot> [cgi_dir] "
                "[--config FILE] [--timeout N] [--max-uri N] "
                "[--max-header N] [--max-body N] "
                "[--keepalive-idle N] [--keepalive-max N] "
                "[--task-workers N] [--task-queue N] [--max-conns N] "
                "[--max-upstreams N] [--backend]\n",
                argv[0]);
        return 1;
    }

    if (config_validate(&cfg) < 0)
        return 1;

    return server_ev_run(&cfg) == 0 ? 0 : 1;
}
