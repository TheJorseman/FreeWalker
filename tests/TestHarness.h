// WalkerCore — minimal test harness.
//
// We don't pull in gtest to keep the dependency surface tiny. The harness
// here is enough for the math + parser tests we ship:
//
//   - EXPECT_EQ / EXPECT_NE / EXPECT_LT / EXPECT_LE / EXPECT_GT / EXPECT_GE
//   - EXPECT_TRUE / EXPECT_FALSE
//   - EXPECT_NEAR(actual, expected, eps) — for floating-point
//   - EXPECT_STREQ — for C strings
//
// Failures increment a global counter. Each test returns void; the runner
// calls them in order and prints a summary.

#ifndef WALKERCORE_TEST_HARNESS_H
#define WALKERCORE_TEST_HARNESS_H

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace walkercore {
namespace testing {

inline int &FailCount() {
    static int fails = 0;
    return fails;
}

inline int &TestCount() {
    static int tests = 0;
    return tests;
}

inline const char *&CurrentTestName() {
    static const char *name = "";
    return name;
}

inline void RecordFail(const char *file, int line, const char *expr) {
    FailCount()++;
    fprintf(stderr, "    [FAIL %s:%d] %s\n", file, line, expr);
}

}  // namespace testing
}  // namespace walkercore

#define EXPECT_TRUE(cond)                                                  \
    do {                                                                   \
        walkercore::testing::TestCount()++;                                 \
        if (!(cond)) {                                                     \
            walkercore::testing::RecordFail(__FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b)                                                    \
    do {                                                                   \
        walkercore::testing::TestCount()++;                                 \
        const auto _a = (a);                                               \
        const auto _b = (b);                                               \
        if (!(_a == _b)) {                                                 \
            walkercore::testing::RecordFail(__FILE__, __LINE__, #a " == " #b); \
        }                                                                  \
    } while (0)

#define EXPECT_NE(a, b)                                                    \
    do {                                                                   \
        walkercore::testing::TestCount()++;                                 \
        const auto _a = (a);                                               \
        const auto _b = (b);                                               \
        if (!(_a != _b)) {                                                 \
            walkercore::testing::RecordFail(__FILE__, __LINE__, #a " != " #b); \
        }                                                                  \
    } while (0)

#define EXPECT_LT(a, b)                                                    \
    do {                                                                   \
        walkercore::testing::TestCount()++;                                 \
        const auto _a = (a);                                               \
        const auto _b = (b);                                               \
        if (!(_a < _b)) {                                                  \
            walkercore::testing::RecordFail(__FILE__, __LINE__, #a " < " #b); \
        }                                                                  \
    } while (0)

#define EXPECT_LE(a, b)                                                    \
    do {                                                                   \
        walkercore::testing::TestCount()++;                                 \
        const auto _a = (a);                                               \
        const auto _b = (b);                                               \
        if (!(_a <= _b)) {                                                 \
            walkercore::testing::RecordFail(__FILE__, __LINE__, #a " <= " #b); \
        }                                                                  \
    } while (0)

#define EXPECT_GT(a, b)                                                    \
    do {                                                                   \
        walkercore::testing::TestCount()++;                                 \
        const auto _a = (a);                                               \
        const auto _b = (b);                                               \
        if (!(_a > _b)) {                                                  \
            walkercore::testing::RecordFail(__FILE__, __LINE__, #a " > " #b); \
        }                                                                  \
    } while (0)

#define EXPECT_NEAR(actual, expected, eps)                                 \
    do {                                                                   \
        walkercore::testing::TestCount()++;                                 \
        const double _a = static_cast<double>(actual);                      \
        const double _e = static_cast<double>(expected);                    \
        const double _d = (_a) - (_e);                                       \
        const double _abs = (_d < 0) ? -_d : _d;                           \
        if (_abs > static_cast<double>(eps)) {                             \
            char buf[256];                                                 \
            snprintf(buf, sizeof(buf), "%s ~= %s within %g (got %g vs %g)", \
                     #actual, #expected, static_cast<double>(eps), _a, _e); \
            walkercore::testing::RecordFail(__FILE__, __LINE__, buf);      \
        }                                                                  \
    } while (0)

#define EXPECT_STREQ(a, b)                                                 \
    do {                                                                   \
        walkercore::testing::TestCount()++;                                 \
        const char *_a = (a);                                              \
        const char *_b = (b);                                              \
        if (strcmp(_a, _b) != 0) {                                         \
            walkercore::testing::RecordFail(__FILE__, __LINE__, #a " == " #b); \
        }                                                                  \
    } while (0)

#define RUN_TEST(name)                                                     \
    do {                                                                   \
        walkercore::testing::CurrentTestName() = #name;                    \
        const int fails_before = walkercore::testing::FailCount();          \
        fprintf(stderr, "  - %s\n", #name);                                  \
        name();                                                            \
        const int fails_after = walkercore::testing::FailCount();           \
        if (fails_after == fails_before) {                                 \
            fprintf(stderr, "    ok\n");                                   \
        }                                                              \
    } while (0)

#define TEST_SUMMARY()                                                     \
    do {                                                                   \
        const int total = walkercore::testing::TestCount();                 \
        const int fails = walkercore::testing::FailCount();                \
        fprintf(stderr, "\n%d checks, %d failed\n", total, fails);         \
        return (fails == 0) ? 0 : 1;                                       \
    } while (0)

#endif  // WALKERCORE_TEST_HARNESS_H