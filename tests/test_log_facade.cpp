// WalkerCore — log facade SDK-free API tests.
//
// Verifies that the public log facade works without including scssdk.h:
//   - walkercore_log_init accepts a NULL sink and falls back to stderr.
//   - walkercore_log accepts our own level constants.
//   - When a custom sink is registered, it receives the formatted message.
//   - Shutdown is idempotent.
//
// The SCS sink path is not exercised here (it would require linking
// against scssdk.h, which would defeat the point of the test — that the
// public header has no SDK dependency). Instead we install a custom
// sink of type walkercore_log_fn_t, capture what it receives, and check.

#include "TestHarness.h"
#include "walkercore/Log.h"

#include <stdio.h>
#include <string.h>

namespace {

// Test state. Tests in this file run serially, so a single capture
// buffer is enough.
char g_captured_lines[8][256];
int g_captured_count = 0;
int g_captured_last_level = -1;

// The custom sink the test installs via walkercore_log_init. It captures
// each line into g_captured_lines and remembers the level so the test
// can assert severity routing.
extern "C" void CaptureSink(int level, const char *message) {
    g_captured_last_level = level;
    if (g_captured_count >= 8) return;
    snprintf(g_captured_lines[g_captured_count],
             sizeof(g_captured_lines[g_captured_count]),
             "%d:%s", level, message);
    g_captured_count++;
}

void ResetCapture() {
    for (int i = 0; i < g_captured_count; ++i) {
        g_captured_lines[i][0] = '\0';
    }
    g_captured_count = 0;
    g_captured_last_level = -1;
}

bool ContainsCapture(const char *needle) {
    for (int i = 0; i < g_captured_count; ++i) {
        if (strstr(g_captured_lines[i], needle) != nullptr) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace walkercore {

void TestLogCustomSinkReceivesAllLevels() {
    walkercore_log_shutdown();   // idempotent — clear any prior binding
    walkercore_log_init(&CaptureSink);
    ResetCapture();

    walkercore_log(WALKERCORE_LOG_LEVEL_INFO,  "info-payload");
    walkercore_log(WALKERCORE_LOG_LEVEL_WARN,  "warn-payload");
    walkercore_log(WALKERCORE_LOG_LEVEL_ERROR, "err-payload");

    walkercore_log_shutdown();
    EXPECT_EQ(g_captured_count, 3);
    EXPECT_TRUE(ContainsCapture("info-payload"));
    EXPECT_TRUE(ContainsCapture("warn-payload"));
    EXPECT_TRUE(ContainsCapture("err-payload"));
}

void TestLogLevelReachesSink() {
    // The level int the sink receives must match the level the caller
    // passed. This pins the contract that the adapter does NOT translate
    // levels (the values match SCS_LOG_TYPE_* by construction).
    walkercore_log_shutdown();
    walkercore_log_init(&CaptureSink);
    ResetCapture();

    walkercore_log(WALKERCORE_LOG_LEVEL_WARN, "level-warn-check");
    walkercore_log_shutdown();
    EXPECT_EQ(g_captured_last_level, WALKERCORE_LOG_LEVEL_WARN);
    EXPECT_EQ(WALKERCORE_LOG_LEVEL_INFO,  0);
    EXPECT_EQ(WALKERCORE_LOG_LEVEL_WARN,  1);
    EXPECT_EQ(WALKERCORE_LOG_LEVEL_ERROR, 2);
}

void TestLogShutdownIdempotent() {
    // Init / shutdown / shutdown / log should not crash. After shutdown
    // the sink is cleared, so a log call falls back to stderr (which is
    // valid in tests — they just don't get checked). Most importantly
    // the second shutdown must not double-free g_scratch.
    walkercore_log_shutdown();
    walkercore_log_init(&CaptureSink);
    walkercore_log_shutdown();
    walkercore_log_shutdown();   // second shutdown is a no-op
    walkercore_log(WALKERCORE_LOG_LEVEL_INFO, "after-shutdown");
    walkercore_log_shutdown();
    EXPECT_TRUE(true);
}

void TestLogNullSinkIsSafe() {
    // Calling walkercore_log_init with NULL is documented to fall back
    // to OutputDebugStringW. We don't try to capture that sink (it goes
    // to the debugger), but we do confirm that a subsequent log call
    // does not crash.
    walkercore_log_shutdown();
    walkercore_log_init(nullptr);
    walkercore_log(WALKERCORE_LOG_LEVEL_INFO, "null-sink-call");
    walkercore_log_shutdown();
    EXPECT_TRUE(true);
}

}  // namespace walkercore