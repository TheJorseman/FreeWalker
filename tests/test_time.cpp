// WalkerCore — Time.h contract probe.
//
// NowMicros() is the domain's single source of truth for "microseconds
// since session start" — the FrameLoop and SettingsStore both consume
// it for dt calculation and polling-interval throttling. Despite
// being widely called, it shipped without a dedicated test_*.cpp
// before this commit (the indirect coverage in test_settings_check_
// file_changed.cpp and test_frame_loop_perf.cpp exercises perf
// budgets, not contract invariants).
//
// This file pins:
//   - NowMicros returns a non-zero value (any platform that boots
//     QPC has a non-zero counter).
//   - NowMicros is monotonically non-decreasing across a tight loop
//     (regression shibboleth: a future contributor who swaps the
//     implementation for time(nullptr) would break this).
//   - NowMicros advances meaningfully over a Sleep(1) window
//     (~1000-3000 µs on a real Windows host; the test is generous
//     to CI VMs that share the QPC clock).
//   - DeltaSeconds() returns positive for now > then, zero for
//     now == then, AND zero for now < then (the "negative delta
//     is a clock anomaly, clamp to 0" contract documented in
//     Time.h).
//   - DeltaSeconds() with a ~1 s span returns ~1.0 (double
//     precision check; this is what the per-frame conductor
//     relies on when it passes dt_seconds to FrameLoop::RunOnce).
//
// The "monotonic" probe is the strongest regression shibboleth:
// a future change that, say, switched to timeGetTime() (which is
// only millisecond-resolution and rolls over), or to a
// wall-clock that the user can change mid-session, would trip
// the loop's invariants and pin the test to the right behaviour.

#include "TestHarness.h"

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "walkercore/Time.h"

namespace walkercore {

void TestTimeNowMicrosReturnsNonZero() {
    // Sanity: a running process on a modern Windows host has a
    // non-zero QPC counter. If this fails, either the host is
    // bizarre (no QPC) or NowMicros is returning 0 unconditionally.
    const uint64_t t = NowMicros();
    EXPECT_NE(t, 0ull);
}

void TestTimeNowMicrosIsMonotonic() {
    // A tight loop of 1000 calls. The QPC clock is monotonic on
    // every supported Windows version, so each call must return a
    // value >= the previous one. We tolerate "==" because QPC has
    // a finite tick rate and consecutive calls within a single
    // tick quantum can return the same value.
    constexpr int kIterations = 1000;
    uint64_t prev = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        const uint64_t cur = NowMicros();
        // Test harness has no EXPECT_GE; use EXPECT_LE with swapped
        // args to express "cur >= prev".
        EXPECT_LE(prev, cur);
        prev = cur;
    }
}

void TestTimeNowMicrosAdvancesOverSleep() {
#if defined(_WIN32)
    // Sleep(1) is documented to sleep for AT LEAST 1 ms — typically
    // 1-3 ms on a real Windows host. We expect the time delta to
    // be in the 1000..50000 µs range. The lower bound catches a
    // regression where NowMicros always returns the same value;
    // the upper bound catches a regression that, e.g., adds a
    // huge constant offset.
    const uint64_t t0 = NowMicros();
    Sleep(1);
    const uint64_t t1 = NowMicros();
    const uint64_t delta = (t1 > t0) ? (t1 - t0) : 0ull;
    // Test harness has EXPECT_GT/EXPECT_LE but not EXPECT_GE; we
    // write "delta >= 1000" as "1000 <= delta" (i.e. EXPECT_LE).
    EXPECT_LE(1000ull, delta);     // at least 1 ms
    EXPECT_LT(delta, 50000ull);    // and not absurdly long
#else
    // Skip the Sleep probe on non-Windows. The build only runs on
    // Windows in CI, but the symbol is defined cross-platform for
    // sanity.
    EXPECT_TRUE(true);
#endif
}

void TestTimeDeltaSecondsPositive() {
    // now > then: returns a positive value equal to (now-then)/1e6.
    const uint64_t then = 1'000'000ull;     // 1 s in µs
    const uint64_t now  = 1'500'000ull;     // 1.5 s in µs
    const double dt = DeltaSeconds(now, then);
    EXPECT_NEAR(dt, 0.5, 1e-9);
}

void TestTimeDeltaSecondsZeroWhenEqual() {
    // now == then: returns 0. The contract is "0 delta means no
    // time has passed"; callers use this to skip integration.
    const uint64_t t = NowMicros();
    EXPECT_EQ(DeltaSeconds(t, t), 0.0);
}

void TestTimeDeltaSecondsClampedWhenNegative() {
    // now < then: clock anomaly. The contract clamps to 0 rather
    // than returning a negative number, because every consumer
    // (FrameLoop, SettingsStore::CheckFileChanged) treats a 0 dt
    // as "skip this frame's integration" — a negative dt would
    // make the player fly backwards in time.
    const uint64_t now  = 1'000'000ull;
    const uint64_t then = 1'500'000ull;
    const double dt = DeltaSeconds(now, then);
    EXPECT_EQ(dt, 0.0);
}

void TestTimeDeltaSecondsHandlesRealisticDelta() {
    // A ~1 s span should return ~1.0. This is the realistic case
    // the per-frame conductor hits: dt_seconds passed to
    // FrameLoop::RunOnce is computed as DeltaSeconds(now, last)
    // where the two timestamps are typically a frame apart (16.6
    // ms at 60 Hz). We use a 1 s span for the test because
    // a 16.6 ms span has too much measurement noise on slow CI VMs.
#if defined(_WIN32)
    const uint64_t t0 = NowMicros();
    Sleep(1000);  // 1 s
    const uint64_t t1 = NowMicros();
    const double dt = DeltaSeconds(t1, t0);
    // Sleep(1000) is documented to sleep for AT LEAST 1000 ms.
    // Generous upper bound of 1.2 s for CI VMs that oversleep.
    // Test harness has no EXPECT_GE; use EXPECT_LE with swapped args.
    EXPECT_LE(0.95, dt);
    EXPECT_LT(dt, 1.20);
#else
    EXPECT_TRUE(true);
#endif
}

void TestTimeDeltaSecondsMicrosecondPrecision() {
    // A 1 µs delta should return exactly 1e-6. This pins the
    // µs → s conversion (divide by 1e6) and rules out a
    // regression that drops a factor of 1e3 (→ ms instead of µs).
    const double dt = DeltaSeconds(1'000'001ull, 1'000'000ull);
    EXPECT_NEAR(dt, 1e-6, 1e-12);
}

}  // namespace walkercore
