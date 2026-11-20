#ifndef TEST_H
#define TEST_H

/*
 * 외부 의존성 없는 최소 단위 테스트 매크로.
 * 실패해도 다음 검사를 계속하고, 마지막에 TEST_SUMMARY() 가 종료 코드를 돌려준다.
 */

#include <stdio.h>
#include <string.h>

static int test_failures = 0;
static int test_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        test_checks++;                                                       \
        if (!(cond)) {                                                       \
            test_failures++;                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                       \
    do {                                                                     \
        long long a_ = (long long)(actual), e_ = (long long)(expected);      \
        test_checks++;                                                       \
        if (a_ != e_) {                                                      \
            test_failures++;                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s == %lld, expected %lld\n",     \
                    __FILE__, __LINE__, #actual, a_, e_);                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                       \
    do {                                                                     \
        const char *a_ = (actual), *e_ = (expected);                         \
        test_checks++;                                                       \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                             \
            test_failures++;                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", \
                    __FILE__, __LINE__, #actual, a_ ? a_ : "(null)", e_);    \
        }                                                                    \
    } while (0)

#define RUN_TEST(fn)                         \
    do {                                     \
        int before_ = test_failures;         \
        fn();                                \
        printf("%s %s\n",                    \
               test_failures == before_ ? "ok  " : "FAIL", #fn); \
    } while (0)

#define TEST_SUMMARY()                                                   \
    (printf("%d checks, %d failures\n", test_checks, test_failures),     \
     test_failures == 0 ? 0 : 1)

#endif
