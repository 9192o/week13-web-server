#define _GNU_SOURCE

#include "static_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#endif
#endif

#ifndef RESOLVE_BENEATH
#define RESOLVE_BENEATH 0x0008
#endif
#ifndef RESOLVE_NO_SYMLINKS
#define RESOLVE_NO_SYMLINKS 0x0004
#endif
#ifndef RESOLVE_NO_MAGICLINKS
#define RESOLVE_NO_MAGICLINKS 0x0002
#endif
#ifndef RESOLVE_NO_XDEV
#define RESOLVE_NO_XDEV 0x0001
#endif
#ifndef SYS_openat2
#define SYS_openat2 437
#endif

#ifndef O_PATH
#define O_PATH 010000000
#endif

#define STATIC_PATH_MAX 4096
#define STATIC_NAME_MAX 256
#define STATIC_COMP_MAX 64
#define STATIC_SYMLINK_MAX 40

struct component_queue
{
    char items[STATIC_COMP_MAX][STATIC_NAME_MAX];
    size_t head;
    size_t count;
};

static void q_init(struct component_queue *q)
{
    q->head = 0;
    q->count = 0;
}

static int q_push_back(struct component_queue *q, const char *name)
{
    if (q->count >= STATIC_COMP_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t len = strlen(name);
    if (len >= STATIC_NAME_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t tail = (q->head + q->count) % STATIC_COMP_MAX;
    memcpy(q->items[tail], name, len + 1);
    q->count++;
    return 0;
}

static int q_push_front(struct component_queue *q, const char *name)
{
    if (q->count >= STATIC_COMP_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t len = strlen(name);
    if (len >= STATIC_NAME_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    q->head = (q->head + STATIC_COMP_MAX - 1) % STATIC_COMP_MAX;
    memcpy(q->items[q->head], name, len + 1);
    q->count++;
    return 0;
}

static const char *q_front(const struct component_queue *q)
{
    if (q->count == 0)
        return NULL;
    return q->items[q->head];
}

static void q_pop_front(struct component_queue *q)
{
    if (q->count == 0)
        return;
    q->head = (q->head + 1) % STATIC_COMP_MAX;
    q->count--;
}

static int open_beneath(int root_fd, const char *path, int flags)
{
    struct open_how how = {
        .flags = (uint64_t)(flags | O_CLOEXEC),
        .mode = 0,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS |
                   RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV,
    };
    return (int)syscall(SYS_openat2, root_fd, path, &how, sizeof(how));
}

static int push_path(struct component_queue *q, const char *path)
{
    const char *p = path;
    while (*p)
    {
        const char *end = p;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - p);
        if (len > 0)
        {
            if (len >= STATIC_NAME_MAX)
            {
                errno = ENAMETOOLONG;
                return -1;
            }
            char name[STATIC_NAME_MAX];
            memcpy(name, p, len);
            name[len] = '\0';
            if (q_push_back(q, name) < 0)
                return -1;
        }
        if (*end == '/')
            p = end + 1;
        else
            p = end;
    }
    return 0;
}

static int push_target(struct component_queue *q, const char *target)
{
    char comps[STATIC_COMP_MAX][STATIC_NAME_MAX];
    size_t count = 0;
    const char *p = target;
    while (*p)
    {
        const char *end = p;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - p);
        if (len > 0)
        {
            if (count >= STATIC_COMP_MAX || len >= STATIC_NAME_MAX)
            {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(comps[count], p, len);
            comps[count][len] = '\0';
            count++;
        }
        if (*end == '/')
            p = end + 1;
        else
            p = end;
    }
    for (size_t i = count; i > 0; --i)
    {
        if (q_push_front(q, comps[i - 1]) < 0)
            return -1;
    }
    return 0;
}

static void resolved_append(char *resolved, size_t *len, const char *comp)
{
    if (*len > 0)
    {
        resolved[*len] = '/';
        (*len)++;
    }
    size_t comp_len = strlen(comp);
    memcpy(resolved + *len, comp, comp_len + 1);
    *len += comp_len;
}

static void resolved_pop(char *resolved, size_t *len, size_t comp_len)
{
    if (*len <= comp_len)
    {
        resolved[0] = '\0';
        *len = 0;
        return;
    }
    resolved[*len - comp_len - 1] = '\0';
    *len = strlen(resolved);
}

static void copy_resolved(char *out, const char *in, size_t len, size_t capacity)
{
    if (capacity == 0)
        return;
    size_t copy = len < capacity ? len : capacity - 1;
    memcpy(out, in, copy);
    out[copy] = '\0';
}

static int read_root_path(int root_fd, char *path, size_t capacity)
{
    char proc_path[32];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", root_fd);
    ssize_t n = readlink(proc_path, path, capacity - 1);
    if (n < 0)
        return -1;
    path[n] = '\0';
    return (int)n;
}

static int open_regular_from_fd(int path_fd, struct stat *info)
{
    struct stat st_path;
    if (fstat(path_fd, &st_path) < 0)
        return -1;
    if (!S_ISREG(st_path.st_mode))
    {
        errno = EACCES;
        return -1;
    }

    char proc_path[32];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", path_fd);
    int rfd = open(proc_path, O_RDONLY | O_CLOEXEC);
    if (rfd < 0)
        return -1;

    struct stat st_rfd;
    if (fstat(rfd, &st_rfd) < 0)
    {
        int saved = errno;
        close(rfd);
        errno = saved;
        return -1;
    }
    if (st_rfd.st_dev != st_path.st_dev || st_rfd.st_ino != st_path.st_ino ||
        !S_ISREG(st_rfd.st_mode))
    {
        close(rfd);
        errno = EACCES;
        return -1;
    }

    *info = st_rfd;
    return rfd;
}

int static_file_open(int root_fd, const char *relative,
                     struct stat *info, char *resolved, size_t capacity)
{
    if (root_fd < 0 || relative == NULL || info == NULL || resolved == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (capacity == 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (relative[0] == '\0' || (relative[0] == '.' && relative[1] == '\0'))
    {
        int dfd = open_beneath(root_fd, ".", O_PATH | O_NOFOLLOW);
        if (dfd < 0)
            return -1;
        if (fstat(dfd, info) < 0)
        {
            int saved = errno;
            close(dfd);
            errno = saved;
            return -1;
        }
        if (!S_ISDIR(info->st_mode))
        {
            close(dfd);
            errno = ENOTDIR;
            return -1;
        }
        copy_resolved(resolved, ".", 1, capacity);
        return dfd;
    }

    char root_path[STATIC_PATH_MAX];
    int root_path_len = read_root_path(root_fd, root_path, sizeof(root_path));

    struct component_queue q;
    q_init(&q);
    if (push_path(&q, relative) < 0)
        return -1;

    char resolved_buf[STATIC_PATH_MAX];
    resolved_buf[0] = '\0';
    size_t resolved_len = 0;

    struct symlink_seen
    {
        dev_t dev;
        ino_t ino;
    } seen[STATIC_SYMLINK_MAX];
    size_t seen_count = 0;

    while (q.count > 0)
    {
        const char *comp = q_front(&q);
        q_pop_front(&q);

        if (strcmp(comp, ".") == 0)
            continue;

        if (strcmp(comp, "..") == 0)
        {
            if (resolved_len == 0)
            {
                errno = EXDEV;
                return -1;
            }
            char *last = strrchr(resolved_buf, '/');
            if (last == NULL)
            {
                resolved_buf[0] = '\0';
                resolved_len = 0;
            }
            else
            {
                *last = '\0';
                resolved_len = strlen(resolved_buf);
            }
            continue;
        }

        if (comp[0] == '.')
        {
            errno = ENOENT;
            return -1;
        }

        size_t comp_len = strlen(comp);
        if (resolved_len + 1 + comp_len >= STATIC_PATH_MAX)
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        resolved_append(resolved_buf, &resolved_len, comp);

        int path_fd = open_beneath(root_fd, resolved_buf, O_PATH | O_NOFOLLOW);
        if (path_fd < 0)
            return -1;

        struct stat st;
        if (fstat(path_fd, &st) < 0)
        {
            int saved = errno;
            close(path_fd);
            errno = saved;
            return -1;
        }

        if (S_ISLNK(st.st_mode))
        {
            for (size_t i = 0; i < seen_count; ++i)
            {
                if (seen[i].dev == st.st_dev && seen[i].ino == st.st_ino)
                {
                    close(path_fd);
                    errno = ELOOP;
                    return -1;
                }
            }
            if (seen_count >= STATIC_SYMLINK_MAX)
            {
                close(path_fd);
                errno = ELOOP;
                return -1;
            }
            seen[seen_count].dev = st.st_dev;
            seen[seen_count].ino = st.st_ino;
            seen_count++;

            char target[STATIC_PATH_MAX];
            ssize_t tlen = readlinkat(path_fd, "", target, sizeof(target) - 1);
            if (tlen < 0)
            {
                int saved = errno;
                close(path_fd);
                errno = saved;
                return -1;
            }
            target[tlen] = '\0';
            close(path_fd);

            resolved_pop(resolved_buf, &resolved_len, comp_len);

            if (target[0] == '/')
            {
                if (root_path_len < 0)
                {
                    errno = EXDEV;
                    return -1;
                }
                if ((size_t)root_path_len + 1 >= sizeof(root_path) ||
                    strncmp(target, root_path, (size_t)root_path_len) != 0 ||
                    target[root_path_len] != '/')
                {
                    if (strcmp(target, root_path) != 0)
                    {
                        errno = EXDEV;
                        return -1;
                    }
                }
                resolved_buf[0] = '\0';
                resolved_len = 0;
                if (strcmp(target, root_path) != 0)
                {
                    if (push_target(&q, target + root_path_len + 1) < 0)
                        return -1;
                }
            }
            else
            {
                if (push_target(&q, target) < 0)
                    return -1;
            }
            continue;
        }

        if (S_ISDIR(st.st_mode))
        {
            if (q.count == 0)
            {
                *info = st;
                copy_resolved(resolved, resolved_buf, resolved_len, capacity);
                return path_fd;
            }
            close(path_fd);
            continue;
        }

        if (S_ISREG(st.st_mode))
        {
            if (q.count > 0)
            {
                close(path_fd);
                errno = ENOTDIR;
                return -1;
            }
            int rfd = open_regular_from_fd(path_fd, info);
            close(path_fd);
            if (rfd < 0)
                return -1;
            copy_resolved(resolved, resolved_buf, resolved_len, capacity);
            return rfd;
        }

        close(path_fd);
        errno = EACCES;
        return -1;
    }

    int dfd = open_beneath(root_fd, ".", O_PATH | O_NOFOLLOW);
    if (dfd < 0)
        return -1;
    if (fstat(dfd, info) < 0)
    {
        int saved = errno;
        close(dfd);
        errno = saved;
        return -1;
    }
    copy_resolved(resolved, ".", 1, capacity);
    return dfd;
}
