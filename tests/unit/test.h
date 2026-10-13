/* Minimal unit-test scaffolding for amigafs.
 *
 * No external dependency: each test is a standalone program returning non-zero
 * on failure so ctest reports it. CHECK_* macros report the file and line so a
 * failure names the exact assertion.
 */
#ifndef AMIGAFS_TEST_H
#define AMIGAFS_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_checks = 0;
static int t_failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        t_checks++;                                                           \
        if (cond) { printf("  ok   "); printf(__VA_ARGS__); printf("\n"); }  \
        else {                                                                \
            printf("  FAIL "); printf(__VA_ARGS__);                           \
            printf("   [%s:%d]\n", __FILE__, __LINE__);                      \
            t_failures++;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_EQ_U(actual, expect, ...)                                       \
    do {                                                                      \
        unsigned long long _a = (unsigned long long)(actual);                 \
        unsigned long long _e = (unsigned long long)(expect);                 \
        t_checks++;                                                           \
        if (_a == _e) { printf("  ok   "); printf(__VA_ARGS__);               \
                         printf(" = %llu\n", _a); }                           \
        else {                                                                \
            printf("  FAIL "); printf(__VA_ARGS__);                           \
            printf(" = %llu, expected %llu   [%s:%d]\n", _a, _e,            \
                   __FILE__, __LINE__);                                       \
            t_failures++;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_EQ_S(actual, expect, ...)                                       \
    do {                                                                      \
        const char *_a = (actual);                                            \
        const char *_e = (expect);                                            \
        t_checks++;                                                           \
        if (strcmp(_a, _e) == 0) { printf("  ok   "); printf(__VA_ARGS__);     \
                                   printf(" = \"%s\"\n", _a); }               \
        else {                                                                \
            printf("  FAIL "); printf(__VA_ARGS__);                           \
            printf(" = \"%s\", expected \"%s\"   [%s:%d]\n", _a, _e,          \
                   __FILE__, __LINE__);                                       \
            t_failures++;                                                     \
        }                                                                     \
    } while (0)

#define TEST_REPORT()                                                         \
    do {                                                                      \
        printf("%d checks, %d failure(s)\n", t_checks, t_failures);          \
        return t_failures ? 1 : 0;                                            \
    } while (0)

#endif /* AMIGAFS_TEST_H */