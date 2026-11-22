#include "static.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * 임시 디렉터리 구조:
 *   base/secret.txt              root 밖의 파일
 *   base/www/                    root
 *     index.html  ("hi")
 *     cat.png
 *     .env
 *     empty/                     index.html 없는 디렉터리
 *     docs/index.html
 *     cgi-bin/adder
 *     escape -> ../secret.txt    root 밖을 가리키는 심볼릭 링크
 *     inside -> index.html       root 안을 가리키는 심볼릭 링크
 */
static char base[64];
static char root[PATH_MAX];
static struct static_target t;

static void path_of(char *out, const char *rel)
{
    snprintf(out, PATH_MAX, "%s/%s", base, rel);
}

static void make_file(const char *rel, const char *content)
{
    char p[PATH_MAX];
    FILE *f;

    path_of(p, rel);
    f = fopen(p, "w");
    if (f == NULL)
        abort();
    fputs(content, f);
    fclose(f);
}

static void make_dir(const char *rel)
{
    char p[PATH_MAX];
    path_of(p, rel);
    if (mkdir(p, 0755) < 0)
        abort();
}

static void make_link(const char *target, const char *rel)
{
    char p[PATH_MAX];
    path_of(p, rel);
    if (symlink(target, p) < 0)
        abort();
}

static void setup(void)
{
    char www[PATH_MAX];

    snprintf(base, sizeof base, "/tmp/ws-static-XXXXXX");
    if (mkdtemp(base) == NULL)
        abort();
    make_file("secret.txt", "secret");
    make_dir("www");
    make_file("www/index.html", "hi");
    make_file("www/cat.png", "\x89PNG");
    make_file("www/.env", "KEY=1");
    make_dir("www/empty");
    make_dir("www/docs");
    make_file("www/docs/index.html", "docs");
    make_dir("www/cgi-bin");
    make_file("www/cgi-bin/adder", "#!/bin/sh");
    make_link("../secret.txt", "www/escape");
    make_link("index.html", "www/inside");

    path_of(www, "www");
    if (realpath(www, root) == NULL)       /* config_parse 와 같은 조건 */
        abort();
}

static void teardown(void)
{
    static const char *files[] = {
        "www/inside", "www/escape", "www/cgi-bin/adder", "www/docs/index.html",
        "www/.env", "www/cat.png", "www/index.html", "secret.txt",
    };
    static const char *dirs[] = {"www/cgi-bin", "www/docs", "www/empty", "www"};
    char p[PATH_MAX];
    size_t i;

    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        path_of(p, files[i]);
        unlink(p);
    }
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        path_of(p, dirs[i]);
        rmdir(p);
    }
    rmdir(base);
}

static int resolve(const char *path)
{
    return static_resolve(root, path, HTTP_GET, &t);
}

static void test_regular_file_200(void)
{
    char want[PATH_MAX + 16];

    CHECK_EQ_INT(resolve("/index.html"), 200);
    snprintf(want, sizeof want, "%s/index.html", root);
    CHECK_EQ_STR(t.path, want);
    CHECK_EQ_INT(t.size, 2);
    CHECK_EQ_STR(t.mime, "text/html; charset=utf-8");

    CHECK_EQ_INT(resolve("/cat.png"), 200);                 /* S3 */
    CHECK_EQ_STR(t.mime, "image/png");

    CHECK_EQ_INT(resolve("/%69ndex.html"), 200);            /* 디코딩 후 찾음 */
    CHECK_EQ_INT(resolve("/inside"), 200);                  /* root 안 링크는 허용 */
    CHECK_EQ_STR(t.path, want);
}

static void test_directory(void)
{
    CHECK_EQ_INT(resolve("/"), 200);                        /* root 의 index.html */
    CHECK_EQ_INT(t.size, 2);
    CHECK_EQ_INT(resolve("/docs/"), 200);
    CHECK_EQ_INT(t.size, 4);
    CHECK_EQ_INT(resolve("/docs"), 301);                    /* 끝 '/' 없음 */
    CHECK_EQ_INT(resolve("/empty/"), 403);                  /* S4-1 */
    CHECK_EQ_INT(resolve("/empty"), 301);                   /* 먼저 리다이렉트 */
}

static void test_not_found(void)
{
    CHECK_EQ_INT(resolve("/nope.html"), 404);               /* S2 */
    CHECK_EQ_INT(resolve("/index.html/x"), 404);            /* ENOTDIR */
}

static void test_dotfile_404(void)
{
    CHECK_EQ_INT(resolve("/.env"), 404);                    /* S4-2: 실제로 있어도 404 */
    CHECK_EQ_INT(resolve("/%2eenv"), 404);
    CHECK_EQ_INT(resolve("/.nothing"), 404);                /* 없어도 같은 응답 */
}

static void test_dotdot_400(void)
{
    CHECK_EQ_INT(resolve("/../secret.txt"), 400);
    CHECK_EQ_INT(resolve("/%2e%2e/secret.txt"), 400);
    CHECK_EQ_INT(resolve("/docs/..%2f..%2fsecret.txt"), 400);
    CHECK_EQ_INT(resolve("/bad%zz"), 400);
    CHECK_EQ_INT(resolve("/a%00.html"), 400);
}

static void test_symlink_outside_root_403(void)
{
    CHECK_EQ_INT(resolve("/escape"), 403);
}

static void test_cgi_bin_reserved_404(void)
{
    CHECK_EQ_INT(resolve("/cgi-bin/adder"), 404);           /* 스크립트 소스 노출 방지 */
    CHECK_EQ_INT(resolve("/cgi-bin"), 404);
    CHECK_EQ_INT(resolve("/cgi%2Dbin/adder"), 404);
    CHECK_EQ_INT(resolve("/cgi-binx"), 404);                /* 그냥 없는 파일 */
}

static void test_post_405(void)
{
    CHECK_EQ_INT(static_resolve(root, "/index.html", HTTP_POST, &t), 405);
    CHECK_EQ_INT(static_resolve(root, "/nope", HTTP_POST, &t), 405);
    CHECK_EQ_INT(static_resolve(root, "/.env", HTTP_POST, &t), 404);   /* 점 파일 검사가 먼저 */
}

static void test_mime(void)
{
    CHECK_EQ_STR(mime_lookup("/a/index.html"), "text/html; charset=utf-8");
    CHECK_EQ_STR(mime_lookup("/a/INDEX.HTM"), "text/html; charset=utf-8");
    CHECK_EQ_STR(mime_lookup("style.css"), "text/css; charset=utf-8");
    CHECK_EQ_STR(mime_lookup("app.js"), "text/javascript; charset=utf-8");
    CHECK_EQ_STR(mime_lookup("x.json"), "application/json");
    CHECK_EQ_STR(mime_lookup("x.txt"), "text/plain; charset=utf-8");
    CHECK_EQ_STR(mime_lookup("x.Jpeg"), "image/jpeg");
    CHECK_EQ_STR(mime_lookup("x.jpg"), "image/jpeg");
    CHECK_EQ_STR(mime_lookup("x.gif"), "image/gif");
    CHECK_EQ_STR(mime_lookup("x.svg"), "image/svg+xml");
    CHECK_EQ_STR(mime_lookup("x.ico"), "image/x-icon");
    CHECK_EQ_STR(mime_lookup("x.webp"), "image/webp");
    CHECK_EQ_STR(mime_lookup("x.pdf"), "application/pdf");
    CHECK_EQ_STR(mime_lookup("archive.tar.gz"), "application/octet-stream");
    CHECK_EQ_STR(mime_lookup("/a/README"), "application/octet-stream");
    CHECK_EQ_STR(mime_lookup("/a.d/README"), "application/octet-stream"); /* 디렉터리의 점 무시 */
    CHECK_EQ_STR(mime_lookup("/a/x."), "application/octet-stream");
    CHECK_EQ_STR(mime_lookup("/a/.html"), "application/octet-stream");
}

int main(void)
{
    setup();
    RUN_TEST(test_regular_file_200);
    RUN_TEST(test_directory);
    RUN_TEST(test_not_found);
    RUN_TEST(test_dotfile_404);
    RUN_TEST(test_dotdot_400);
    RUN_TEST(test_symlink_outside_root_403);
    RUN_TEST(test_cgi_bin_reserved_404);
    RUN_TEST(test_post_405);
    RUN_TEST(test_mime);
    teardown();
    return TEST_SUMMARY();
}
