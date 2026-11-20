#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <stddef.h>

#define CONFIG_DEFAULT_PORT 8080
#define CONFIG_DEFAULT_ROOT "./www"

struct server_config {
    int  port;             /*서버가 listen할 포트*/
    char root[PATH_MAX];   /* 정적 파일을 서빙할 루트 디렉터리 (절대경로) */
};

/*
 * 명령행 인자를 해석한다: [-p port] [-r root]
 *  - port 는 1~65535 정수만 허용
 *  - root 는 파일 루트. 기본값은 ./www임. 존재하는 디렉터리여야 하며 절대경로로 바꿔 root에 저장
 * 성공하면 0, 실패하면 errbuf 에 이유를 쓰고 -1.
 */
int config_parse(int argc, char **argv, struct server_config *out,
                 char *errbuf, size_t errlen);

void config_usage(const char *prog);

#endif
