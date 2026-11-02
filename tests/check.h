// Minimal test harness for the standalone tests (no gtest; host g++ only).
#ifndef MOD_BOSS_MECHANICS_TEST_CHECK_H
#define MOD_BOSS_MECHANICS_TEST_CHECK_H

#include <cmath>
#include <cstdio>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do \
    { \
        ++g_checks; \
        if (!(cond)) \
        { \
            ++g_failures; \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_NEAR(a, b, eps) \
    do \
    { \
        ++g_checks; \
        double const _a = static_cast<double>(a); \
        double const _b = static_cast<double>(b); \
        if (std::fabs(_a - _b) > (eps)) \
        { \
            ++g_failures; \
            std::printf("FAIL %s:%d: %s = %g, expected %g (eps %g)\n", __FILE__, __LINE__, #a, _a, _b, \
                static_cast<double>(eps)); \
        } \
    } while (0)

inline int Report(char const* name)
{
    std::printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

#endif
