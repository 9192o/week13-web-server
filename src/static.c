#include "static.h"
#include "http_response.h"
#include "server.h"
#include "url.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm",  "text/html; charset=utf-8"},
    {"css",  "text/css; charset=utf-8"},
    {"js",   "text/javascript; charset=utf-8"},
    {"json", "application/json; charset=utf-8"},
    {"txt",  "text/plain; charset=utf-8"},
    {"png",  "image/png"},
    {"jpg",  "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif",  "image/gif"},
    {"svg",  "image/svg+xml"},
    {"ico",  "image/x-icon"},
    {"webp", "image/webp"},
    {"pdf",  "application/pdf"},
};

#define MIME_DEFAULT "application/octet-stream"

const char *mime_lookup(const char *path)
{
    const char *name = strrchr(path, '/');
    const char *dot;
    size_t i;

    name = name ? name + 1 : path;
    dot = strrchr(name, '.');
    if (dot == NULL || dot == name)      /* 확장자 없음, 또는 ".bashrc" 처럼 이름 자체가 점으로 시작 */
        return MIME_DEFAULT;
    for (i = 0; i < sizeof mime_table / sizeof mime_table[0]; i++)
        if (strcasecmp(dot + 1, mime_table[i].ext) == 0)
            return mime_table[i].type;
    return MIME_DEFAULT;
}

/* 파일 시스템 오류를 상태 코드로 (결정 6) */
static int errno_status(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        return 404;
    case EACCES:
        return 403;
    default:
        return 500;
    }
}

/* real 이 root 자신이거나 root 아래에 있는가 */
static int is_under_root(const char *root, const char *real)
{
    size_t rl = strlen(root);

    return strncmp(real, root, rl) == 0 && (real[rl] == '\0' || real[rl] == '/');
}

/*
 * candidate 를 realpath 로 풀고, root 안인지 확인한 뒤 stat 한다.
 * 반환: 0 성공 (out_real, st 채움), 그 외 상태 코드.
 */
static int locate(const char *root, const char *candidate, char *out_real,
                  struct stat *st)
{
    if (realpath(candidate, out_real) == NULL)
        return errno_status(errno);
    if (!is_under_root(root, out_real))
        return 403;                       /* 심볼릭 링크가 root 밖을 가리킴 */
    if (stat(out_real, st) < 0)
        return errno_status(errno);
    return 0;
}

int static_resolve(const char *root, const char *decoded,
                   struct static_target *out)
{
    char candidate[PATH_MAX];
    struct stat st;
    size_t dl;
    int n, rc;

    n = snprintf(candidate, sizeof candidate, "%s%s", root, decoded);
    if (n < 0 || (size_t)n >= sizeof candidate)
        return 500;                       /* 경로가 PATH_MAX 를 넘음: ENAMETOOLONG 과 같이 500 */
    rc = locate(root, candidate, out->path, &st);
    if (rc != 0)
        return rc;

    if (S_ISDIR(st.st_mode)) {
        dl = strlen(decoded);
        if (dl == 0 || decoded[dl - 1] != '/')
            return 301;                   /* 호출자가 Location: <원문 경로>/ 를 만든다 */
        n = snprintf(candidate, sizeof candidate, "%s/index.html", out->path);
        if (n < 0 || (size_t)n >= sizeof candidate)
            return 403;
        rc = locate(root, candidate, out->path, &st);
        if (rc == 404)
            return 403;                   /* S4-1: 목록은 보여주지 않음 */
        if (rc != 0)
            return rc;
        if (!S_ISREG(st.st_mode))
            return 403;                   /* index.html 이 디렉터리 등 */
    } else if (!S_ISREG(st.st_mode)) {
        return 403;                       /* FIFO, 장치 파일 등: open 전에 거른다 */
    }

    out->size = (long long)st.st_size;
    out->mime = mime_lookup(out->path);
    return 200;
}

int static_send(int fd, const struct static_target *t, int keep_alive,
                long long *sent)
{
    char chunk[STATIC_CHUNK];   /* 워커마다 따로 (스레드 스택: Linux 8MB, macOS 512KB 기본) */
    char head[512];
    struct stat st;
    long long left, deadline;
    int file, hl, sr;

    *sent = 0;
    file = open(t->path, O_RDONLY);
    if (file < 0)
        return errno_status(errno);       /* 권한 000 등: stat 은 됐지만 open 실패 */
    if (fstat(file, &st) < 0) {
        close(file);
        return 500;
    }
    if (!S_ISREG(st.st_mode)) {           /* resolve 이후 바뀐 경우 */
        close(file);
        return 403;
    }

    left = (long long)st.st_size;         /* 실제로 연 파일 기준 크기 */
    hl = http_format_head(head, sizeof head, 200, t->mime, left, NULL, keep_alive,
                          time(NULL));
    if (hl < 0) {
        close(file);
        return 500;
    }
    deadline = now_ms() + SEND_TIMEOUT_MS;   /* 헤더 + 본문 전체 */
    sr = send_all(fd, head, (size_t)hl, deadline);
    if (sr != SEND_OK) {
        close(file);
        return sr;
    }

    while (left > 0) {
        size_t want = left < (long long)sizeof chunk ? (size_t)left : sizeof chunk;
        ssize_t n = read(file, chunk, want);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {                     /* 읽기 오류 또는 파일이 줄어듦: 길이를 못 지킴 */
            close(file);
            return -1;
        }
        sr = send_all(fd, chunk, (size_t)n, deadline);
        if (sr != SEND_OK) {              /* S10: EPIPE 등, 또는 시간 초과 */
            close(file);
            return sr;
        }
        *sent += n;
        left -= n;
    }
    close(file);
    return 0;
}
