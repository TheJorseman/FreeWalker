// WalkerCore — honest-stub one-shot warning probes.
//
// Verifies the "warn-once-per-session" contract for every M-lander
// honest stub:
//   - M3 PhysXAdapter       (ResetPhysXStubWarnForTesting)
//   - M9 CouplingProvider   (ResetCouplingStubWarnForTesting)
//   - M10 KnockoutProvider  (ResetKnockoutStubWarnForTesting)
//   - M7 PolishProviders    (ResetPolishStubWarnForTesting)
//   - M5/M8 FuelProvider    (ResetFuelPumpStubWarnForTesting)
//
// For each, after a fresh reset:
//   1. The first call to the stub entry-point emits exactly ONE
//      warning.
//   2. Subsequent calls (4 more) emit ZERO additional warnings.
//
// Together this pins the "log once, then stay silent" behaviour —
// the regression mode would be a stub that forgets to set its
// warned flag and spams the game log every frame.

#include "TestHarness.h"
#include "TestStubReset.h"

#include <cstdio>
#include <cstdarg>
#include <string.h>

#include "walkercore/FrameLoop.h"
#include "walkercore/FuelPump.h"
#include "walkercore/Knockout.h"
#include "walkercore/Log.h"
#include "walkercore/PhysXAdapter.h"
#include "walkercore/PlayerState.h"
#include "walkercore/PolishProviders.h"
#include "walkercore/TrailerCoupling.h"

namespace walkercore {

namespace {

// Mirror the capture helpers in test_diagnostic_dump.cpp's anonymous
// namespace. We can't share them across TUs without lifting them
// into a header, but the probe tests are short enough that the
// duplication is the cheaper path.

// 256 lines × 1024 chars — plenty for the one-or-two lines these
// tests produce per run.
constexpr int kMaxCaptureLines = 256;
constexpr int kMaxCaptureChars = 1024;
char g_capture[kMaxCaptureLines][kMaxCaptureChars];
int g_capture_count = 0;
int g_last_level = 0;

extern "C" void StubWarnCaptureSink(int level, const char *msg) {
    g_last_level = level;
    if (g_capture_count < kMaxCaptureLines) {
        std::snprintf(g_capture[g_capture_count], kMaxCaptureChars, "%s",
                      msg != nullptr ? msg : "");
        ++g_capture_count;
    }
}

void ResetStubCapture() {
    g_capture_count = 0;
    g_last_level = 0;
    for (int i = 0; i < kMaxCaptureLines; ++i) g_capture[i][0] = '\0';
}

int CountOccurrences(const char *needle) {
    int count = 0;
    if (needle == nullptr) return 0;
    const size_t n = strlen(needle);
    for (int i = 0; i < g_capture_count; ++i) {
        for (const char *p = g_capture[i]; *p != '\0'; ++p) {
            if (strncmp(p, needle, n) == 0) {
                ++count;
                break;  // one match per line
            }
        }
    }
    return count;
}

}  // namespace

// -----------------------------------------------------------------------------
// M3 PhysXAdapter.
// -----------------------------------------------------------------------------

void TestPhysXStubWarnsExactlyOnce() {
    walkercore_log_shutdown();
    walkercore_log_init(&StubWarnCaptureSink);
    ResetPhysXStubWarnForTesting();
    ResetStubCapture();

    PhysXAdapter adapter;
    Vec3 hit_pt{};
    Vec3 hit_n{};
    // First call → must emit. Four more calls → must stay silent.
    const int before_first = CountOccurrences("PhysXAdapter: stub");
    adapter.Raycast({0, 0, 0}, {0, 0, 1}, 1.0, &hit_pt, &hit_n);
    const int after_first = CountOccurrences("PhysXAdapter: stub");
    for (int i = 0; i < 4; ++i) {
        adapter.SphereSweep({0, 0, 0}, 0.5, {0.1, 0, 0});
        adapter.groundY({0, 0, 0}, nullptr);
    }
    const int after_five = CountOccurrences("PhysXAdapter: stub");

    walkercore_log_shutdown();
    EXPECT_EQ(after_first - before_first, 1);
    EXPECT_EQ(after_five - after_first, 0);
}

// -----------------------------------------------------------------------------
// M10 KnockoutProvider.
// -----------------------------------------------------------------------------

void TestKnockoutStubWarnsExactlyOnce() {
    walkercore_log_shutdown();
    walkercore_log_init(&StubWarnCaptureSink);
    ResetKnockoutStubWarnForTesting();
    ResetStubCapture();

    KnockoutProvider knockout;
    PlayerState p;
    PlayerState_Reset(p);
    p.feet_position = Vec3{0, 0, 0};
    p.has_feet = true;
    p.mode = PlayerMode::Idle;

    const int before_first = CountOccurrences("KnockoutProvider: stub");
    knockout.Tick(p, 0.016);
    const int after_first = CountOccurrences("KnockoutProvider: stub");
    for (int i = 0; i < 4; ++i) {
        knockout.Tick(p, 0.016);
    }
    const int after_five = CountOccurrences("KnockoutProvider: stub");

    walkercore_log_shutdown();
    EXPECT_EQ(after_first - before_first, 1);
    EXPECT_EQ(after_five - after_first, 0);
}

// -----------------------------------------------------------------------------
// M9 CouplingProvider (TrailerCoupling).
// -----------------------------------------------------------------------------

void TestCouplingStubWarnsExactlyOnce() {
    walkercore_log_shutdown();
    walkercore_log_init(&StubWarnCaptureSink);
    ResetCouplingStubWarnForTesting();
    ResetStubCapture();

    CouplingProvider coupling;
    TruckState t;
    // Defaults: placement not set, no trailer. Just need a non-empty
    // TruckState handle for Tick() to operate on.
    t.has_placement = false;
    PlayerState p;
    PlayerState_Reset(p);

    const int before_first = CountOccurrences("CouplingProvider: stub");
    coupling.Tick(t, p, /*interact_pressed=*/false, 0.016);
    const int after_first = CountOccurrences("CouplingProvider: stub");
    for (int i = 0; i < 4; ++i) {
        coupling.Tick(t, p, false, 0.016);
    }
    const int after_five = CountOccurrences("CouplingProvider: stub");

    walkercore_log_shutdown();
    EXPECT_EQ(after_first - before_first, 1);
    EXPECT_EQ(after_five - after_first, 0);
}

// -----------------------------------------------------------------------------
// M7 Polish providers (HeadTracker).
//
// The PolishProviders WarnStub() is per-name: each unique name warns
// exactly once. We exercise it via HeadTrackerProvider::FeedSample
// which warns for TrackIR / Opentrack sources when those sources are
// enabled in the cfg.
// -----------------------------------------------------------------------------

void TestPolishStubWarnsExactlyOncePerName() {
    walkercore_log_shutdown();
    walkercore_log_init(&StubWarnCaptureSink);
    ResetPolishStubWarnForTesting();
    ResetStubCapture();

    HeadTrackerProvider ht;
    HeadTrackerConfig cfg;
    cfg.use_trackir = true;
    cfg.use_opentrack = true;
    ht.SetConfig(cfg);
    ht.Install();

    const HeadTrackerSample s = {0.01f, 0.02f, 0.0f};
    const int before_first = CountOccurrences("PolishProvider HeadTracker/TrackIR: stub");

    ht.FeedSample(HeadTrackerSource::TrackIR, s);   // 1st: should warn
    ht.FeedSample(HeadTrackerSource::TrackIR, s);   // 2nd: silent (per-name)
    ht.FeedSample(HeadTrackerSource::TrackIR, s);   // 3rd: silent
    ht.FeedSample(HeadTrackerSource::TrackIR, s);   // 4th: silent
    ht.FeedSample(HeadTrackerSource::TrackIR, s);   // 5th: silent
    const int after_five = CountOccurrences("PolishProvider HeadTracker/TrackIR: stub");

    // Opentrack should still get its own first warning (per-name
    // one-shot, not per-process one-shot).
    ht.FeedSample(HeadTrackerSource::Opentrack, s); // 1st Opentrack: should warn
    const int after_opentrack_first = CountOccurrences("PolishProvider HeadTracker/Opentrack: stub");

    walkercore_log_shutdown();
    EXPECT_EQ(after_five - before_first, 1);
    EXPECT_EQ(after_opentrack_first - 0, 1);  // never reset since capture start
}

// -----------------------------------------------------------------------------
// M5/M8 FuelProvider stub-honest warn contract. Pinned identically to
// the four stubs above (Knockout/TrailerCoupling/Polish/PhysX): first
// call emits exactly one warning, the next four emit zero.
//
// FuelProvider::Tick() crosses through WarnStub() before any real work
// (M5 station finding, M8 fuel/adblue sync, fill-rate math). The
// reset hook matches the existing four stubs' pattern.
// -----------------------------------------------------------------------------

void TestFuelPumpStubWarnsExactlyOnce() {
    walkercore_log_shutdown();
    walkercore_log_init(&StubWarnCaptureSink);
    ResetFuelPumpStubWarnForTesting();
    ResetStubCapture();

    FuelProvider fuel;
    TruckState truck;
    PlayerState player;
    PlayerState_Reset(player);
    player.feet_position = Vec3{0, 0, 0};
    player.has_feet = true;

    const int before_first = CountOccurrences("FuelProvider: stub");
    fuel.Tick(truck, player, 0.016);
    const int after_first = CountOccurrences("FuelProvider: stub");
    for (int i = 0; i < 4; ++i) {
        fuel.Tick(truck, player, 0.016);
    }
    const int after_five = CountOccurrences("FuelProvider: stub");

    walkercore_log_shutdown();
    EXPECT_EQ(after_first - before_first, 1);
    EXPECT_EQ(after_five - after_first, 0);
}

}  // namespace walkercore
