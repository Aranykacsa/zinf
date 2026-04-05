#pragma once
#include <stdio.h>
#include <stdlib.h>

/* Minimal test framework — no external dependencies */

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;

#define ASSERT_EQ(a, b) do { \
    g_tests_run++; \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL %s:%d  expected %lld, got %lld\n", \
                __FILE__, __LINE__, (long long)(b), (long long)(a)); \
        g_tests_failed++; \
    } else { \
        g_tests_passed++; \
    } \
} while (0)

#define ASSERT_OK(rc)   ASSERT_EQ((rc), 0)
#define ASSERT_NE(a, b) do { \
    g_tests_run++; \
    if ((a) == (b)) { \
        fprintf(stderr, "  FAIL %s:%d  expected values to differ, both are %lld\n", \
                __FILE__, __LINE__, (long long)(a)); \
        g_tests_failed++; \
    } else { \
        g_tests_passed++; \
    } \
} while (0)

#define TEST_BEGIN(name) do { \
    printf("[ RUN ] %s\n", (name)); \
} while (0)

#define TEST_END(name) do { \
    if (g_tests_failed == 0) \
        printf("[ OK  ] %s\n", (name)); \
    else \
        printf("[FAIL ] %s (%d failures)\n", (name), g_tests_failed); \
} while (0)

static inline void test_summary(void) {
    printf("\n=== Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed) printf(" (%d FAILED)", g_tests_failed);
    printf(" ===\n");
    if (g_tests_failed) exit(1);
}
