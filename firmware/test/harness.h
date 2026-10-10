/* Tiny assert-based test harness: no framework, exit code = number of failures (capped). */
#ifndef PINCER_TEST_HARNESS_H
#define PINCER_TEST_HARNESS_H
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_pass = 0, t_fail = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (cond) t_pass++;                                                                \
        else { t_fail++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        long long _a = (long long)(a), _b = (long long)(b);                                            \
        if (_a == _b) t_pass++;                                                                        \
        else { t_fail++; fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } \
    } while (0)

#define CHECK_U64(a, b)                                                                                  \
    do {                                                                                                 \
        uint64_t _a = (a), _b = (b);                                                                     \
        if (_a == _b) t_pass++;                                                                          \
        else { t_fail++; fprintf(stderr, "  FAIL %s:%d: %s == %s (%" PRIu64 " vs %" PRIu64 ")\n", __FILE__, __LINE__, #a, #b, _a, _b); } \
    } while (0)

#define CHECK_STR(a, b)                                                                                 \
    do {                                                                                                \
        const char *_a = (a), *_b = (b);                                                                \
        if (_a && _b && strcmp(_a, _b) == 0) t_pass++;                                                  \
        else { t_fail++; fprintf(stderr, "  FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); } \
    } while (0)

#define CHECK_MEM(a, b, n)                                                                              \
    do {                                                                                                \
        if (memcmp((a), (b), (n)) == 0) t_pass++;                                                       \
        else { t_fail++; fprintf(stderr, "  FAIL %s:%d: memcmp(%s, %s, %s)\n", __FILE__, __LINE__, #a, #b, #n); } \
    } while (0)

#define RUN(fn)                      \
    do {                             \
        printf("- %s\n", #fn);       \
        fn();                        \
    } while (0)

#define TEST_END()                                                                   \
    do {                                                                             \
        printf("%s: %d passed, %d failed\n", __FILE__, t_pass, t_fail);              \
        return t_fail ? 1 : 0;                                                       \
    } while (0)

static void hex2bin(const char *hex, uint8_t *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}
#endif
