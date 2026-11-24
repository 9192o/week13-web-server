#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* s 가 lo~hi 사이의 10진 정수 전체면 *out 에 넣고 0, 아니면 -1 */
static int parse_int(const char *s, long lo, long hi, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

int config_parse(int argc, char **argv, struct server_config *out,
                 char *errbuf, size_t errlen)
{
    const char *root = CONFIG_DEFAULT_ROOT;
    struct stat st;
    int opt;

    out->port = CONFIG_DEFAULT_PORT;
    out->workers = CONFIG_DEFAULT_WORKERS;
    out->queue_cap = CONFIG_DEFAULT_QUEUE;

    optind = 1;   /* 여러 번 호출(테스트)해도 처음부터 다시 읽도록 "어디까지 읽었는지"를 전역변수에 저장 */
    opterr = 0;   /* getopt 자체 메시지 대신 errbuf 로 보고 */
    while ((opt = getopt(argc, argv, "p:r:w:q:")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_int(optarg, 1, 65535, &out->port) < 0) {
                snprintf(errbuf, errlen, "invalid port: %s", optarg);
                return -1;
            }
            break;
        case 'r':
            root = optarg;
            break;
        case 'w':
            if (parse_int(optarg, 1, CONFIG_MAX_WORKERS, &out->workers) < 0) {
                snprintf(errbuf, errlen, "invalid workers: %s (1-%d)", optarg,
                         CONFIG_MAX_WORKERS);
                return -1;
            }
            break;
        case 'q':
            if (parse_int(optarg, 1, CONFIG_MAX_QUEUE, &out->queue_cap) < 0) {
                snprintf(errbuf, errlen, "invalid queue size: %s (1-%d)", optarg,
                         CONFIG_MAX_QUEUE);
                return -1;
            }
            break;
        default:
            snprintf(errbuf, errlen, "unknown option or missing value: -%c",
                     optopt);
            return -1;
        }
    }
    if (optind < argc) {
        snprintf(errbuf, errlen, "unexpected argument: %s", argv[optind]);
        return -1;
    }

    if (realpath(root, out->root) == NULL) {
        snprintf(errbuf, errlen, "root %s: %s", root, strerror(errno));
        return -1;
    }
    if (strcmp(out->root, "/") == 0) {   /* 파일 시스템 전체 노출 방지 (-r /.. 등도 여기서 걸림) */
        snprintf(errbuf, errlen, "root must not be \"/\"");
        return -1;
    }
    if (stat(out->root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snprintf(errbuf, errlen, "root %s: not a directory", root);
        return -1;
    }
    return 0;
}

void config_usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-p port] [-r root] [-w workers] [-q queue]\n"
                    "  -p port     listen port (1-65535, default %d)\n"
                    "  -r root     static file root (default %s)\n"
                    "  -w workers  worker threads (1-%d, default %d)\n"
                    "  -q queue    pending connection queue size (1-%d, default %d)\n",
            prog, CONFIG_DEFAULT_PORT, CONFIG_DEFAULT_ROOT,
            CONFIG_MAX_WORKERS, CONFIG_DEFAULT_WORKERS,
            CONFIG_MAX_QUEUE, CONFIG_DEFAULT_QUEUE);
}
