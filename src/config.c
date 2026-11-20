#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int parse_port(const char *s, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
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

    optind = 1;   /* 여러 번 호출(테스트)해도 처음부터 다시 읽도록 "어디까지 읽었는지"를 전역변수에 저장 */
    opterr = 0;   /* getopt 자체 메시지 대신 errbuf 로 보고 */
    while ((opt = getopt(argc, argv, "p:r:")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_port(optarg, &out->port) < 0) {
                snprintf(errbuf, errlen, "invalid port: %s", optarg);
                return -1;
            }
            break;
        case 'r':
            root = optarg;
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
    if (stat(out->root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snprintf(errbuf, errlen, "root %s: not a directory", root);
        return -1;
    }
    return 0;
}

void config_usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-p port] [-r root]\n"
                    "  -p port  listen port (1-65535, default %d)\n"
                    "  -r root  static file root (default %s)\n",
            prog, CONFIG_DEFAULT_PORT, CONFIG_DEFAULT_ROOT);
}
