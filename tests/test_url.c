#include "url.h"
#include "test.h"

static char out[256];

static void test_decode_plain(void)
{
    CHECK_EQ_INT(url_decode("/index.html", out, sizeof out), 0);
    CHECK_EQ_STR(out, "/index.html");
    CHECK_EQ_INT(url_decode("", out, sizeof out), 0);
    CHECK_EQ_STR(out, "");
}

static void test_decode_percent(void)
{
    CHECK_EQ_INT(url_decode("/a%20b", out, sizeof out), 0);
    CHECK_EQ_STR(out, "/a b");
    CHECK_EQ_INT(url_decode("/%69ndex%2Ehtml", out, sizeof out), 0);   /* 대소문자 무관 */
    CHECK_EQ_STR(out, "/index.html");
    CHECK_EQ_INT(url_decode("/%2e%2E/x", out, sizeof out), 0);
    CHECK_EQ_STR(out, "/../x");                                       /* 세그먼트 검사에서 걸림 */
    CHECK_EQ_INT(url_decode("/a%2Fb", out, sizeof out), 0);
    CHECK_EQ_STR(out, "/a/b");                                        /* %2F 도 '/' 로 */
    CHECK_EQ_INT(url_decode("/a+b", out, sizeof out), 0);
    CHECK_EQ_STR(out, "/a+b");                                        /* 경로의 '+' 는 그대로 */
    CHECK_EQ_INT(url_decode("/%25", out, sizeof out), 0);
    CHECK_EQ_STR(out, "/%");                                          /* 두 번 디코딩하지 않음 */
}

static void test_decode_invalid(void)
{
    CHECK_EQ_INT(url_decode("/bad%zz", out, sizeof out), -1);
    CHECK_EQ_INT(url_decode("/bad%4", out, sizeof out), -1);
    CHECK_EQ_INT(url_decode("/bad%", out, sizeof out), -1);
    CHECK_EQ_INT(url_decode("/bad%g0", out, sizeof out), -1);
    CHECK_EQ_INT(url_decode("/nul%00.html", out, sizeof out), -1);
}

static void test_decode_capacity(void)
{
    char small[4];
    CHECK_EQ_INT(url_decode("abc", small, sizeof small), 0);      /* 딱 맞음 (+NUL) */
    CHECK_EQ_INT(url_decode("abcd", small, sizeof small), -1);
    CHECK_EQ_INT(url_decode("%41%42%43", small, sizeof small), 0); /* 결과 길이 기준 */
    CHECK_EQ_STR(small, "ABC");
}

static void test_segments(void)
{
    CHECK_EQ_INT(url_check_segments("/"), URL_PATH_OK);
    CHECK_EQ_INT(url_check_segments("/a/b.html"), URL_PATH_OK);
    CHECK_EQ_INT(url_check_segments("/a//b"), URL_PATH_OK);
    CHECK_EQ_INT(url_check_segments("/a../b..c"), URL_PATH_OK);
    CHECK_EQ_INT(url_check_segments("/.."), URL_PATH_DOTDOT);
    CHECK_EQ_INT(url_check_segments("/a/../b"), URL_PATH_DOTDOT);
    CHECK_EQ_INT(url_check_segments("/a/.."), URL_PATH_DOTDOT);
    CHECK_EQ_INT(url_check_segments("/.env"), URL_PATH_DOTFILE);
    CHECK_EQ_INT(url_check_segments("/a/.git/config"), URL_PATH_DOTFILE);
    CHECK_EQ_INT(url_check_segments("/."), URL_PATH_DOTFILE);
    CHECK_EQ_INT(url_check_segments("/..."), URL_PATH_DOTFILE);
    CHECK_EQ_INT(url_check_segments("/.env/../x"), URL_PATH_DOTDOT);  /* ".." 이 우선 */
}

int main(void)
{
    RUN_TEST(test_decode_plain);
    RUN_TEST(test_decode_percent);
    RUN_TEST(test_decode_invalid);
    RUN_TEST(test_decode_capacity);
    RUN_TEST(test_segments);
    return TEST_SUMMARY();
}
