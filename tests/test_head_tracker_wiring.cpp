// WalkerCore — Head tracker integration tests (M7 wiring).
//
// Verifies the contract between HeadTrackerProvider, its default
// configuration, and the diagnostic dump.
//
// Pure provider math (EMA smoothing, source disabling, negative-delta
// tracking) is already covered in test_head_tracker.cpp. This file
// covers the higher-level wiring:
//
//   - A freshly-installed HeadTrackerProvider reports active=false
//     (no source produced a sample yet).
//   - The default config leaves every source DISABLED — the
//     FrameLoop's mouse-look path is the canonical source of yaw/pitch
//     while we wait for the M7 lander to wire real TrackIR /
//     opentrack feeds.
//   - The dump section "(not initialized)" appears when no provider
//     is wired, and the live snapshot appears when one is wired via
//     the diagnostic accessor (the same path PluginEntry uses).
//   - The dump reports the dominant source as a human-readable name
//     via HeadTrackerSourceName (Mouse / TrackIR / Opentrack /
//     LastSource).
//   - After feeding samples from a source enabled at runtime, the
//     dump sees the smoothed output and the source label.

#include "TestHarness.h"

#include <math.h>
#include <string.h>

#include "walkercore/DiagnosticAccess.h"
#include "walkercore/Log.h"
#include "walkercore/PerFrame.h"
#include "walkercore/PlayerState.h"
#include "walkercore/PolishFeatures.h"
#include "walkercore/PolishProviders.h"

namespace {

char g_lines[64][256];
int g_line_count = 0;

extern "C" void HeadTrackerCaptureSink(int /*level*/, const char *msg) {
    if (g_line_count >= 64) return;
    snprintf(g_lines[g_line_count], sizeof(g_lines[g_line_count]),
             "%s", msg);
    g_line_count++;
}

void ResetCapture() {
    for (int i = 0; i < g_line_count; ++i) g_lines[i][0] = '\0';
    g_line_count = 0;
}

bool ContainsCapture(const char *needle) {
    for (int i = 0; i < g_line_count; ++i) {
        if (strstr(g_lines[i], needle) != nullptr) return true;
    }
    return false;
}

}  // namespace

namespace walkercore {

void TestHeadTrackerWiringDefaultConfigAllDisabled() {
    // The default config MUST leave every source disabled. This is the
    // contract that prevents double-counting with FrameLoop's
    // mouse-look path. If a future maintainer flips a default, this
    // test catches it.
    HeadTrackerProvider provider;
    const HeadTrackerConfig cfg = provider.config();
    EXPECT_FALSE(cfg.use_mouse);
    EXPECT_FALSE(cfg.use_trackir);
    EXPECT_FALSE(cfg.use_opentrack);
}

void TestHeadTrackerWiringFreshProviderReportsInactive() {
    // No samples have been fed yet, so active=false. dominant_source
    // defaults to Mouse (per the implementation), but active=false
    // signals "don't apply me to the camera" without needing to check
    // the source.
    HeadTrackerProvider provider;
    provider.Install();
    const HeadTrackerState s = provider.Sample();
    EXPECT_FALSE(s.active);
    EXPECT_NEAR(s.yaw_radians, 0.0f, 1e-6f);
    EXPECT_NEAR(s.pitch_radians, 0.0f, 1e-6f);
}

void TestHeadTrackerWiringInstallIdempotent() {
    // Install() returns false on the second call. The Dump writes
    // active=false but doesn't crash, regardless of how many times
    // Install() was called.
    HeadTrackerProvider provider;
    EXPECT_TRUE(provider.Install());
    EXPECT_FALSE(provider.Install());
}

void TestHeadTrackerWiringSourceNameAllValues() {
    // HeadTrackerSourceName must cover every value in the enum so the
    // dump never prints "(unknown)". Pinned against future enum growth.
    EXPECT_STREQ(HeadTrackerSourceName(HeadTrackerSource::Mouse), "Mouse");
    EXPECT_STREQ(HeadTrackerSourceName(HeadTrackerSource::TrackIR), "TrackIR");
    EXPECT_STREQ(HeadTrackerSourceName(HeadTrackerSource::Opentrack),
                 "Opentrack");
    EXPECT_STREQ(HeadTrackerSourceName(HeadTrackerSource::LastSource),
                 "LastSource");
}

void TestHeadTrackerWiringDumpReportsNotInitialized() {
    // Before the M7 lander wires a head tracker, the dump reports
    // "(not initialized)" without crashing.
    walkercore_log_shutdown();
    walkercore_log_init(&HeadTrackerCaptureSink);
    ResetCapture();
    diagnostic_internal::SetHeadTracker(nullptr);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("head tracker"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    walkercore_log_shutdown();
}

void TestHeadTrackerWiringDumpReportsInactiveByDefault() {
    // A wired but unconfigured head tracker (no source enabled)
    // shows active=false. This is the steady-state output in the
    // M7 stub phase — no TrackIR / opentrack feed is connected.
    walkercore_log_shutdown();
    walkercore_log_init(&HeadTrackerCaptureSink);
    ResetCapture();
    HeadTrackerProvider provider;
    provider.Install();
    diagnostic_internal::SetHeadTracker(&provider);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("head tracker"));
    EXPECT_TRUE(ContainsCapture("active=false"));
    diagnostic_internal::SetHeadTracker(nullptr);
    walkercore_log_shutdown();
}

void TestHeadTrackerWiringDumpReportsActiveAfterMouseFeed() {
    // Enable the mouse source, feed a single sample, sample the
    // smoothed state, and confirm the dump sees active=true with
    // the source label.
    walkercore_log_shutdown();
    walkercore_log_init(&HeadTrackerCaptureSink);
    ResetCapture();
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg = provider.config();
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 1.0f;
    cfg.smoothing_factor = 0.0f;  // no smoothing; reflect delta directly
    provider.SetConfig(cfg);
    provider.Install();

    HeadTrackerSample s;
    s.yaw_delta_rad = 0.5f;
    s.pitch_delta_rad = 0.0f;
    s.roll_delta_rad = 0.0f;
    provider.FeedSample(HeadTrackerSource::Mouse, s);
    (void)provider.Sample();

    diagnostic_internal::SetHeadTracker(&provider);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("head tracker"));
    EXPECT_TRUE(ContainsCapture("active=true"));
    EXPECT_TRUE(ContainsCapture("source=Mouse"));

    diagnostic_internal::SetHeadTracker(nullptr);
    walkercore_log_shutdown();
}

void TestHeadTrackerWiringDumpReportsYawAfterMouseFeed() {
    // End-to-end: enable mouse, feed 0.5 rad, sample. The dump line
    // must contain a yaw value close to 0.5 (with smoothing_factor=0,
    // no EMA lag). Pins the contract that the dump surfaces the
    // smoothed output that a real renderer would consume.
    walkercore_log_shutdown();
    walkercore_log_init(&HeadTrackerCaptureSink);
    ResetCapture();
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg = provider.config();
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 1.0f;
    cfg.smoothing_factor = 0.0f;
    provider.SetConfig(cfg);
    provider.Install();

    HeadTrackerSample s;
    s.yaw_delta_rad = 0.5f;
    s.pitch_delta_rad = 0.0f;
    s.roll_delta_rad = 0.0f;
    provider.FeedSample(HeadTrackerSource::Mouse, s);
    (void)provider.Sample();

    diagnostic_internal::SetHeadTracker(&provider);
    walker_dump_state();
    // Sample the head tracker one more time inside the dump. The
    // smoothed yaw should still be ~0.5 because (a) we set smoothing
    // factor to 0 (no lag) and (b) we haven't moved since.
    EXPECT_TRUE(ContainsCapture("yaw="));
    // The dump format is "yaw=%.3f rad"; with smoothing=0 the
    // smoothed value equals the cumulative target (0.5).
    EXPECT_TRUE(ContainsCapture("yaw=0.500 rad") ||
                ContainsCapture("yaw=0.500"));

    diagnostic_internal::SetHeadTracker(nullptr);
    walkercore_log_shutdown();
}

void TestHeadTrackerWiringDumpUpdatesOnFeed() {
    // Two consecutive dumps should reflect changes in the wired
    // provider — feed a 0.3 rad sample, dump; feed another 0.4 rad
    // sample, dump; second dump's yaw must be larger than the first.
    walkercore_log_shutdown();
    walkercore_log_init(&HeadTrackerCaptureSink);
    ResetCapture();
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg = provider.config();
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 1.0f;
    cfg.smoothing_factor = 0.0f;
    provider.SetConfig(cfg);
    provider.Install();

    HeadTrackerSample s;
    s.yaw_delta_rad = 0.3f;
    s.pitch_delta_rad = 0.0f;
    s.roll_delta_rad = 0.0f;
    provider.FeedSample(HeadTrackerSource::Mouse, s);
    (void)provider.Sample();

    diagnostic_internal::SetHeadTracker(&provider);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("yaw=0.300"));

    ResetCapture();

    s.yaw_delta_rad = 0.4f;
    provider.FeedSample(HeadTrackerSource::Mouse, s);
    (void)provider.Sample();
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("yaw=0.700"));

    diagnostic_internal::SetHeadTracker(nullptr);
    walkercore_log_shutdown();
}

void TestHeadTrackerWiringConfigOverride() {
    // SetConfig applies immediately; subsequent FeedSample uses the
    // new weights. This is the contract the M7 lander relies on for
    // live INI overrides.
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg = provider.config();
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 2.0f;
    cfg.smoothing_factor = 0.0f;
    provider.SetConfig(cfg);
    provider.Install();

    HeadTrackerSample s;
    s.yaw_delta_rad = 0.1f;
    s.pitch_delta_rad = 0.0f;
    s.roll_delta_rad = 0.0f;
    provider.FeedSample(HeadTrackerSource::Mouse, s);
    const HeadTrackerState after = provider.Sample();
    // 0.1 rad delta * 2.0 sensitivity = 0.2 rad smoothed (with
    // smoothing_factor=0 the EMA tracks the target directly).
    EXPECT_NEAR(after.yaw_radians, 0.2f, 1e-5);
}

void TestHeadTrackerWiringPeekDoesNotAdvance() {
    // Peek() is the read-only accessor used by diagnostic dumps. It
    // MUST NOT advance the EMA — repeated Peek() calls return the
    // same smoothed value. Sample() advances; Peek() does not.
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg = provider.config();
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 1.0f;
    cfg.smoothing_factor = 0.5f;
    provider.SetConfig(cfg);
    provider.Install();

    HeadTrackerSample s;
    s.yaw_delta_rad = 0.5f;
    s.pitch_delta_rad = 0.0f;
    s.roll_delta_rad = 0.0f;
    provider.FeedSample(HeadTrackerSource::Mouse, s);
    // One Sample() to advance the EMA — the rest of the test reads
    // through Peek() to verify it doesn't perturb the state.
    const HeadTrackerState seeded = provider.Sample();
    EXPECT_TRUE(seeded.active);

    // Multiple Peek() calls in a row MUST return the same values.
    // If Peek() advanced the EMA, repeated calls would converge to
    // the cumulative target (0.5 rad), changing the smoothed value.
    const HeadTrackerState p1 = provider.Peek();
    const HeadTrackerState p2 = provider.Peek();
    const HeadTrackerState p3 = provider.Peek();
    EXPECT_NEAR(p1.yaw_radians, p2.yaw_radians, 1e-6);
    EXPECT_NEAR(p2.yaw_radians, p3.yaw_radians, 1e-6);
    // And the values must match the seeded Sample() (within float
    // tolerance) — Peek doesn't move the EMA off the value Sample()
    // left it at.
    EXPECT_NEAR(p1.yaw_radians, seeded.yaw_radians, 1e-6);
}

void TestHeadTrackerWiringPeekIsConst() {
    // Peek() must be callable on a const provider so diagnostic code
    // that holds a const pointer doesn't need to downcast. This pins
    // the const-ness contract that lets DiagnosticDump.cpp hold
    // `const HeadTrackerProvider*` and still read state.
    const HeadTrackerProvider *cprovider = nullptr;
    HeadTrackerProvider provider;
    cprovider = &provider;
    const HeadTrackerState s = cprovider->Peek();
    EXPECT_FALSE(s.active);
}

}  // namespace walkercore