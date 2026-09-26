#ifndef GLYPHFX_TESTUTIL_H
#define GLYPHFX_TESTUTIL_H

#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                        \
    do {                                                                          \
        g_checks++;                                                               \
        long long va = (long long)(a);                                            \
        long long vb = (long long)(b);                                            \
        if (va != vb) {                                                           \
            g_failures++;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va, vb); \
        }                                                                         \
    } while (0)

static inline int test_summary(const char *name) {
    printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

#endif
