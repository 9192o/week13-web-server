#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "static_file.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

static int previous_dir = -1;
static char temp_directory[] = "/tmp/webserver-static-XXXXXX";
static int root_fd = -1;

static void setup(void)
{
    previous_dir = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(previous_dir >= 0);
    assert(mkdtemp(temp_directory));
    assert(chdir(temp_directory) == 0);
    root_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root_fd >= 0);
}

static void teardown(void)
{
    if (root_fd >= 0)
    {
        close(root_fd);
        root_fd = -1;
    }
    if (previous_dir >= 0)
    {
        assert(fchdir(previous_dir) == 0);
        assert(close(previous_dir) == 0);
        previous_dir = -1;
    }
    assert(rmdir(temp_directory) == 0);
}

static void reset_root(void)
{
    assert(close(root_fd) == 0);
    root_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root_fd >= 0);
}

static void test_root_dot(void)
{
    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, ".", &info, resolved, sizeof(resolved));
    assert(fd >= 0);
    assert(S_ISDIR(info.st_mode));
    assert(strcmp(resolved, ".") == 0);
    assert(close(fd) == 0);
}

static void test_simple_file(void)
{
    FILE *file = fopen("hello.txt", "wbe");
    assert(file);
    const char data[] = "Hello, world!";
    assert(fwrite(data, 1, sizeof(data) - 1, file) == sizeof(data) - 1);
    assert(fclose(file) == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "hello.txt", &info, resolved, sizeof(resolved));
    assert(fd >= 0);
    assert(S_ISREG(info.st_mode));
    assert(strcmp(resolved, "hello.txt") == 0);

    char buffer[64];
    ssize_t n = read(fd, buffer, sizeof(buffer));
    assert(n == (ssize_t)(sizeof(data) - 1));
    assert(memcmp(buffer, data, (size_t)n) == 0);
    assert(close(fd) == 0);
    assert(unlink("hello.txt") == 0);
}

static void test_directory(void)
{
    assert(mkdir("adir", 0700) == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "adir", &info, resolved, sizeof(resolved));
    assert(fd >= 0);
    assert(S_ISDIR(info.st_mode));
    assert(strcmp(resolved, "adir") == 0);
    assert(close(fd) == 0);

    fd = static_file_open(root_fd, "adir/", &info, resolved, sizeof(resolved));
    assert(fd >= 0);
    assert(S_ISDIR(info.st_mode));
    assert(strcmp(resolved, "adir") == 0);
    assert(close(fd) == 0);

    assert(rmdir("adir") == 0);
}

static void test_nested_file(void)
{
    assert(mkdir("nest", 0700) == 0);
    FILE *file = fopen("nest/inner.txt", "wbe");
    assert(file);
    const char data[] = "nested";
    assert(fwrite(data, 1, sizeof(data) - 1, file) == sizeof(data) - 1);
    assert(fclose(file) == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "nest/inner.txt", &info, resolved, sizeof(resolved));
    assert(fd >= 0);
    assert(S_ISREG(info.st_mode));
    assert(strcmp(resolved, "nest/inner.txt") == 0);

    char buffer[64];
    assert(read(fd, buffer, sizeof(buffer)) == (ssize_t)(sizeof(data) - 1));
    assert(close(fd) == 0);
    assert(unlink("nest/inner.txt") == 0);
    assert(rmdir("nest") == 0);
}

static void test_hidden_rejected(void)
{
    FILE *file = fopen(".secret", "wbe");
    assert(file);
    assert(fwrite("x", 1, 1, file) == 1);
    assert(fclose(file) == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, ".secret", &info, resolved, sizeof(resolved));
    assert(fd == -1);
    assert(errno == ENOENT);
    assert(unlink(".secret") == 0);
}

static void test_dot_dot_boundary(void)
{
    assert(mkdir("dir", 0700) == 0);
    struct stat info;
    char resolved[256];

    int fd = static_file_open(root_fd, "..", &info, resolved, sizeof(resolved));
    assert(fd == -1);
    assert(errno == EXDEV);

    fd = static_file_open(root_fd, "dir/../../x", &info, resolved, sizeof(resolved));
    assert(fd == -1);
    assert(errno == EXDEV);

    assert(rmdir("dir") == 0);
}

static void test_file_as_directory(void)
{
    FILE *file = fopen("plain.txt", "wbe");
    assert(file);
    assert(fwrite("x", 1, 1, file) == 1);
    assert(fclose(file) == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "plain.txt/more", &info, resolved, sizeof(resolved));
    assert(fd == -1);
    assert(errno == ENOTDIR);
    assert(unlink("plain.txt") == 0);
}

static void test_symlink_to_file(void)
{
    FILE *file = fopen("target.txt", "wbe");
    assert(file);
    const char data[] = "through symlink";
    assert(fwrite(data, 1, sizeof(data) - 1, file) == sizeof(data) - 1);
    assert(fclose(file) == 0);
    assert(symlink("target.txt", "link.txt") == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "link.txt", &info, resolved, sizeof(resolved));
    assert(fd >= 0);
    assert(S_ISREG(info.st_mode));
    assert(strcmp(resolved, "target.txt") == 0);

    char buffer[64];
    assert(read(fd, buffer, sizeof(buffer)) == (ssize_t)(sizeof(data) - 1));
    assert(close(fd) == 0);
    assert(unlink("link.txt") == 0);
    assert(unlink("target.txt") == 0);
}

static void test_symlink_to_directory(void)
{
    assert(mkdir("realdir", 0700) == 0);
    FILE *file = fopen("realdir/file.txt", "wbe");
    assert(file);
    const char data[] = "inside";
    assert(fwrite(data, 1, sizeof(data) - 1, file) == sizeof(data) - 1);
    assert(fclose(file) == 0);
    assert(symlink("realdir", "linkdir") == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "linkdir/file.txt", &info, resolved, sizeof(resolved));
    assert(fd >= 0);
    assert(S_ISREG(info.st_mode));
    assert(strcmp(resolved, "realdir/file.txt") == 0);

    char buffer[64];
    assert(read(fd, buffer, sizeof(buffer)) == (ssize_t)(sizeof(data) - 1));
    assert(close(fd) == 0);
    assert(unlink("linkdir") == 0);
    assert(unlink("realdir/file.txt") == 0);
    assert(rmdir("realdir") == 0);
}

static void test_symlink_absolute_inside_root(void)
{
    char root_path[4096];
    ssize_t n = readlinkat(root_fd, "", root_path, sizeof(root_path) - 1);
    if (n < 0)
    {
        assert(errno == EINVAL || errno == ENOENT);
        return;
    }
    root_path[n] = '\0';

    FILE *file = fopen("abs.txt", "wbe");
    assert(file);
    const char data[] = "absolute";
    assert(fwrite(data, 1, sizeof(data) - 1, file) == sizeof(data) - 1);
    assert(fclose(file) == 0);

    char target[4352];
    assert(snprintf(target, sizeof(target), "%s/abs.txt", root_path) < (int)sizeof(target));
    assert(symlink(target, "abspath") == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "abspath", &info, resolved, sizeof(resolved));
    assert(fd >= 0);
    assert(S_ISREG(info.st_mode));
    assert(strcmp(resolved, "abs.txt") == 0);

    char buffer[64];
    assert(read(fd, buffer, sizeof(buffer)) == (ssize_t)(sizeof(data) - 1));
    assert(close(fd) == 0);
    assert(unlink("abspath") == 0);
    assert(unlink("abs.txt") == 0);
}

static void test_symlink_absolute_outside_root(void)
{
    char root_path[4096];
    ssize_t n = readlinkat(root_fd, "", root_path, sizeof(root_path) - 1);
    if (n < 0)
        return;
    root_path[n] = '\0';

    char target[4352];
    assert(snprintf(target, sizeof(target), "%s/../outside.txt", root_path) < (int)sizeof(target));
    assert(symlink(target, "escape") == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "escape", &info, resolved, sizeof(resolved));
    assert(fd == -1);
    assert(errno == EXDEV);
    assert(unlink("escape") == 0);
}

static void test_symlink_loop(void)
{
    assert(symlink("loop2", "loop1") == 0);
    assert(symlink("loop1", "loop2") == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "loop1", &info, resolved, sizeof(resolved));
    assert(fd == -1);
    assert(errno == ELOOP);
    assert(unlink("loop1") == 0);
    assert(unlink("loop2") == 0);
}

static void test_special_files_rejected(void)
{
    assert(mkfifo("fifo", 0600) == 0);
    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "fifo", &info, resolved, sizeof(resolved));
    assert(fd == -1);
    assert(errno == EACCES);
    assert(unlink("fifo") == 0);

    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(socket_fd >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, "sock", sizeof("sock"));
    assert(bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    fd = static_file_open(root_fd, "sock", &info, resolved, sizeof(resolved));
    assert(fd == -1);
    assert(errno == EACCES);
    assert(close(socket_fd) == 0);
    assert(unlink("sock") == 0);
}

static void test_missing_file(void)
{
    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "missing.txt", &info, resolved, sizeof(resolved));
    assert(fd == -1);
    assert(errno == ENOENT);
}

static void test_dot_component(void)
{
    FILE *file = fopen("page.html", "wbe");
    assert(file);
    assert(fwrite("x", 1, 1, file) == 1);
    assert(fclose(file) == 0);

    struct stat info;
    char resolved[256];
    int fd = static_file_open(root_fd, "././page.html", &info, resolved, sizeof(resolved));
    assert(fd >= 0);
    assert(S_ISREG(info.st_mode));
    assert(strcmp(resolved, "page.html") == 0);
    assert(close(fd) == 0);
    assert(unlink("page.html") == 0);
}

int main(void)
{
    setup();

    test_root_dot();
    reset_root();
    test_simple_file();
    reset_root();
    test_directory();
    reset_root();
    test_nested_file();
    reset_root();
    test_hidden_rejected();
    reset_root();
    test_dot_dot_boundary();
    reset_root();
    test_file_as_directory();
    reset_root();
    test_symlink_to_file();
    reset_root();
    test_symlink_to_directory();
    reset_root();
    test_symlink_absolute_inside_root();
    reset_root();
    test_symlink_absolute_outside_root();
    reset_root();
    test_symlink_loop();
    reset_root();
    test_special_files_rejected();
    reset_root();
    test_missing_file();
    reset_root();
    test_dot_component();

    teardown();
    printf("STATIC_FILE_TESTS_PASS\n");
    return 0;
}
