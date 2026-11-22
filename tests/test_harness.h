#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

extern int g_tests_run;
extern int g_tests_failed;

#define TEST_ASSERT(cond) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define TEST_ASSERT_EQ_INT(a, b) do { \
    g_tests_run++; \
    long _a = (long)(a); \
    long _b = (long)(b); \
    if (_a != _b) { \
        g_tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s == %s (%ld != %ld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
    } \
} while (0)

#define TEST_ASSERT_EQ_STR(a, b) do { \
    g_tests_run++; \
    const char *_a = (a); \
    const char *_b = (b); \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
        g_tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", \
                __FILE__, __LINE__, #a, #b, \
                _a ? _a : "(null)", _b ? _b : "(null)"); \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    printf("RUN %s\n", #fn); \
    fn(); \
} while (0)

#endif
