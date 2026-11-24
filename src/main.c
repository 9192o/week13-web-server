#include "config.h"
#include "server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 종료 코드: 정상 0, 시작 실패 1 (EXIT_FAILURE), 강제 종료 2 */
#define EXIT_FORCED 2

/* 플래그만 켠다: 첫 신호 1 (정상 종료), 두 번째부터 2 (바로 강제 종료) */
static void on_stop_signal(int sig)
{
    (void)sig;
    server_stop = server_stop ? 2 : 1;
}

int main(int argc, char **argv)
{
    struct server_config cfg;
    struct sigaction sa;
    char err[512];
    int listen_fd, r;

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

    /* 정상 종료: SA_RESTART 없이 등록해야 막혀 있던 accept 가 EINTR 로 돌아온다 */
    sa.sa_handler = on_stop_signal;
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction(SIGINT/SIGTERM)");
        return EXIT_FAILURE;
    }

    listen_fd = server_listen(cfg.port);
    if (listen_fd < 0)
        return EXIT_FAILURE;

    fprintf(stderr, "listening on 0.0.0.0:%d, root=%s\n", cfg.port, cfg.root);
    for (r = 0; r < cfg.routes.count; r++) {
        const struct proxy_route *rt = &cfg.routes.routes[r];
        int b;

        fprintf(stderr, "proxy %s ->", rt->prefix);
        for (b = 0; b < rt->nbackends; b++)
            fprintf(stderr, "%s %s", b ? "," : "", rt->backends[b].name);
        fprintf(stderr, "%s\n", rt->nbackends > 1 ? " (round-robin)" : "");
    }
    r = server_run(listen_fd, cfg.root, &cfg.routes, cfg.workers,
                   cfg.queue_cap);  /* listen_fd 도 닫음 */
    if (r < 0)
        return EXIT_FAILURE;
    fprintf(stderr, "shutting down%s\n", r > 0 ? " (forced)" : "");
    return r > 0 ? EXIT_FORCED : EXIT_SUCCESS;
}
