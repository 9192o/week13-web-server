#include "config.h"
#include "server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct server_config cfg;
    struct sigaction sa;
    char err[512];
    int listen_fd;

    if (config_parse(argc, argv, &cfg, err, sizeof err) < 0) {
        fprintf(stderr, "%s: %s\n", argv[0], err);
        config_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* 쓰는 도중 클라이언트가 끊겨도 프로세스가 죽지 않고 write 가 EPIPE 를 돌려주게 */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        perror("sigaction(SIGPIPE)");
        return EXIT_FAILURE;
    }

    listen_fd = server_listen(cfg.port);
    if (listen_fd < 0)
        return EXIT_FAILURE;

    fprintf(stderr, "listening on 0.0.0.0:%d, root=%s\n", cfg.port, cfg.root);
    server_run(listen_fd, cfg.root);
    return EXIT_SUCCESS;
}
