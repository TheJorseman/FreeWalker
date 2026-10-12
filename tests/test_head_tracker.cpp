// WalkerCore — HeadTrackerProvider tests.
//
// Verifies the head tracker's exponential-moving-average behaviour. The
// key invariants:
//   - Smoothed output converges to the cumulative target (yaw_accum_)
//     when input is steady-state.
//   - When input stops, the smoothed output STAYS at the held position
//     — the camera must not drift back to the neutral pose. (This is
//     the regression we just fixed: the old Sample() reset yaw_accum_
//     to 0 each call, causing the smoothed value to decay toward 0
//     whenever input stopped.)
//   - Negative deltas (head returns toward neutral) reduce the
//     cumulative target and the smoothed value tracks it.

#include "TestHarness.h"

#include <math.h>

#include "TestStubReset.h"
#include "walkercore/PolishFeatures.h"
#include "walkercore/PolishProviders.h"

namespace walkercore {

namespace {

HeadTrackerSample MakeSample(float yaw, float pitch) {
    HeadTrackerSample s;
    s.yaw_delta_rad = yaw;
    s.pitch_delta_rad = pitch;
    s.roll_delta_rad = 0.0f;
    return s;
}

}  // namespace

void TestHeadTrackerSteadyStateConverges() {
    // Steady 0.1 rad per frame input — the smoothed output chases the
    // CUMULATIVE target, so after 50 frames with weight 1.0 the
    // cumulative yaw is 5.0 rad and the EMA converges there. (The old
    // test expected 0.1 rad, which only held when the buggy code reset
    // the accumulator every Sample() — that behaviour was the regression
    // we are explicitly guarding against in TestHeadTrackerNoInputDoesNotDrift.)
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg;
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 1.0f;
    cfg.smoothing_factor = 0.5f;
    provider.SetConfig(cfg);
    provider.Install();

    // Feed a steady input for 50 frames.
    for (int i = 0; i < 50; ++i) {
        provider.FeedSample(HeadTrackerSource::Mouse,
                            MakeSample(0.1f, 0.0f));
        (void)provider.Sample();
    }
    // The cumulative target after 50 inputs of 0.1 rad is 5.0 rad. The
    // smoothed value lags the linearly-growing target by exactly one
    // per-frame step at alpha=0.5 (formula: lag = (alpha/(1-alpha)) * delta
    // = 1.0 * 0.1 = 0.1 rad). After 50 (FeedSample, Sample) iterations
    // plus one final Sample, smooth ≈ 5.0 - 0.05 = 4.95. We use a 0.1
    // tolerance to confirm convergence without overspecifying the lag.
    const HeadTrackerState s = provider.Sample();
    EXPECT_TRUE(s.active);
    EXPECT_NEAR(s.yaw_radians, 5.0f, 0.1f);
}

void TestHeadTrackerNoInputDoesNotDrift() {
    // CRITICAL: with the old bug, after input stops the smoothed value
    // would decay toward 0 (the camera drifted back when the user held
    // their head still). With the fix, the smoothed value stays at
    // whatever the cumulative target was when input stopped.
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg;
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 1.0f;
    cfg.smoothing_factor = 0.5f;
    provider.SetConfig(cfg);
    provider.Install();

    // Move the head 0.5 rad.
    for (int i = 0; i < 50; ++i) {
        provider.FeedSample(HeadTrackerSource::Mouse,
                            MakeSample(0.01f, 0.0f));
        (void)provider.Sample();
    }
    const HeadTrackerState before = provider.Sample();
    const float held = before.yaw_radians;
    EXPECT_TRUE(held > 0.3f);  // should have moved significantly

    // No more input for 100 frames — the smoothed value must not drift
    // back toward 0.
    for (int i = 0; i < 100; ++i) {
        const HeadTrackerState s = provider.Sample();
        // Smoothed must stay close to the held position (not decay).
        EXPECT_TRUE(s.yaw_radians > held * 0.95f);
    }
}

void TestHeadTrackerNegativeDeltaReturns() {
    // Negative deltas (user moves head back toward neutral) should
    // reduce the cumulative target; the smoothed value tracks it.
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg;
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 1.0f;
    cfg.smoothing_factor = 0.5f;
    provider.SetConfig(cfg);
    provider.Install();

    // Move head +0.5 rad.
    for (int i = 0; i < 50; ++i) {
        provider.FeedSample(HeadTrackerSource::Mouse,
                            MakeSample(0.01f, 0.0f));
        (void)provider.Sample();
    }
    const HeadTrackerState peak = provider.Sample();
    EXPECT_TRUE(peak.yaw_radians > 0.3f);

    // Move head back -0.01 rad per frame.
    for (int i = 0; i < 100; ++i) {
        provider.FeedSample(HeadTrackerSource::Mouse,
                            MakeSample(-0.01f, 0.0f));
        (void)provider.Sample();
    }
    const HeadTrackerState back = provider.Sample();
    EXPECT_TRUE(back.yaw_radians < peak.yaw_radians * 0.2f);
}

void TestHeadTrackerDisabledSourceIgnored() {
    // If a source is disabled in config, FeedSample for it is a no-op.
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg;
    cfg.use_mouse = false;        // disabled
    cfg.use_trackir = false;
    cfg.use_opentrack = false;
    cfg.smoothing_factor = 0.5f;
    provider.SetConfig(cfg);
    provider.Install();

    provider.FeedSample(HeadTrackerSource::Mouse,
                        MakeSample(1.0f, 1.0f));
    const HeadTrackerState s = provider.Sample();
    // No source active → the cumulative target is 0, smoothed stays 0.
    EXPECT_NEAR(s.yaw_radians, 0.0f, 1e-6f);
}

// -----------------------------------------------------------------------------
// HeadTrackerSourceName probe — same pattern as
// TestFeatureNameReturnsNameForEveryValue and
// TestWalkerFsmStateNameReturnsNameForEveryValue. Adding a new source
// (FreeTrack, another opentrack variant, ...) without updating
// HeadTrackerSourceName() makes the diagnostic dump emit "(unknown)"
// for the new source. The probe table below pins the contract:
//
//   - Each non-sentinel source has a non-null, non-empty,
//     "(unknown)"-free, exact-match display name.
//   - The sentinel LastSource itself has a name too (the switch case
//     must exist) so adding more sources doesn't accidentally delete
//     the LastSource case.
// -----------------------------------------------------------------------------

void TestHeadTrackerSourceNameReturnsNameForEveryValue() {
    struct Case { HeadTrackerSource s; const char *expected_name; };
    const Case kCases[] = {
        {HeadTrackerSource::Mouse,      "Mouse"},
        {HeadTrackerSource::TrackIR,    "TrackIR"},
        {HeadTrackerSource::Opentrack,  "Opentrack"},
        {HeadTrackerSource::LastSource, "LastSource"},
    };
    const size_t kCount = sizeof(kCases) / sizeof(kCases[0]);
    for (size_t i = 0; i < kCount; ++i) {
        const char *name = HeadTrackerSourceName(kCases[i].s);
        EXPECT_TRUE(name != nullptr);
        EXPECT_TRUE(name[0] != '\0');
        EXPECT_TRUE(strcmp(name, "(unknown)") != 0);
        EXPECT_STREQ(name, kCases[i].expected_name);
    }
}

// Sentinels and the LastSource contract: any source value below
// LastSource must be a real, nameable source (no gap allowed, since
// FeedSample accepts a HeadTrackerSource by value). Pin the range.

void TestHeadTrackerSourceRangeIsContiguous() {
    // The probe table covers everything from Mouse through LastSource;
    // adding a new source without extending the table above makes this
    // EXPECT_NE fail and points the maintainer at the missing row.
    EXPECT_EQ(static_cast<unsigned>(HeadTrackerSource::Mouse), 0u);
    EXPECT_EQ(static_cast<unsigned>(HeadTrackerSource::TrackIR),
              static_cast<unsigned>(HeadTrackerSource::Mouse) + 1u);
    EXPECT_EQ(static_cast<unsigned>(HeadTrackerSource::Opentrack),
              static_cast<unsigned>(HeadTrackerSource::TrackIR) + 1u);
    EXPECT_EQ(static_cast<unsigned>(HeadTrackerSource::LastSource),
              static_cast<unsigned>(HeadTrackerSource::Opentrack) + 1u);
}

// -----------------------------------------------------------------------------
// HeadTrackerProvider field-exhaustive probe.
//
// Config inputs (HeadTrackerConfig):
//
//   use_mouse                        (bool)   -- gates Mouse source
//   use_trackir                      (bool)   -- gates TrackIR source
//   use_opentrack                    (bool)   -- gates Opentrack source
//   mouse_sensitivity_rad_per_unit   (float)  -- per-source gain (Mouse)
//   trackir_sensitivity_rad_per_unit (float)  -- per-source gain (TrackIR)
//   opentrack_sensitivity_rad_per_unit (float) -- per-source gain (Opentrack)
//   smoothing_factor                 (float)  -- EMA coefficient (0=instant,
//                                                  1=full lag, never update)
//
// FeedSample inputs:
//
//   source     (HeadTrackerSource)
//   sample.yaw_delta_rad   (float)
//   sample.pitch_delta_rad (float)
//
// State outputs (HeadTrackerState):
//
//   active            (bool)
//   dominant_source   (HeadTrackerSource)  -- last source to feed
//   yaw_radians       (float)              -- smoothed yaw
//   pitch_radians     (float)              -- smoothed pitch
//
// Each contract below pins one observable behaviour. The probe resets
// the PolishStub warn flag at the top because Cases 2/3/5/6 feed
// from TrackIR and Opentrack (the only sources that fire a warn), and
// the per-name one-shot cache must start clean for both names.
// -----------------------------------------------------------------------------

void TestHeadTrackerProviderEveryFieldHasObservableEffect() {
    ResetPolishStubWarnForTesting();  // fresh warn for TrackIR + Opentrack

    // ---- Case 1: use_mouse = false gates Mouse input ----
    {
        HeadTrackerProvider provider;
        HeadTrackerConfig cfg;
        cfg.use_mouse = false;          // disabled
        cfg.use_trackir = false;
        cfg.use_opentrack = false;
        cfg.smoothing_factor = 0.0f;    // instant response (no lag)
        provider.SetConfig(cfg);
        provider.Install();

        // Even with a huge Mouse sample, no contribution.
        provider.FeedSample(HeadTrackerSource::Mouse, MakeSample(1.0f, 1.0f));
        const HeadTrackerState s = provider.Sample();
        EXPECT_FALSE(s.active);
        EXPECT_NEAR(s.yaw_radians, 0.0f, 1e-6f);
        EXPECT_NEAR(s.pitch_radians, 0.0f, 1e-6f);
    }

    // ---- Case 2: use_trackir = false gates TrackIR input ----
    {
        HeadTrackerProvider provider;
        HeadTrackerConfig cfg;
        cfg.use_mouse = false;
        cfg.use_trackir = false;        // disabled
        cfg.use_opentrack = false;
        cfg.smoothing_factor = 0.0f;
        provider.SetConfig(cfg);
        provider.Install();

        provider.FeedSample(HeadTrackerSource::TrackIR,
                            MakeSample(1.0f, 0.5f));
        const HeadTrackerState s = provider.Sample();
        EXPECT_FALSE(s.active);
        EXPECT_NEAR(s.yaw_radians, 0.0f, 1e-6f);
        EXPECT_NEAR(s.pitch_radians, 0.0f, 1e-6f);
    }

    // ---- Case 3: use_opentrack = false gates Opentrack input ----
    {
        HeadTrackerProvider provider;
        HeadTrackerConfig cfg;
        cfg.use_mouse = false;
        cfg.use_trackir = false;
        cfg.use_opentrack = false;      // disabled
        cfg.smoothing_factor = 0.0f;
        provider.SetConfig(cfg);
        provider.Install();

        provider.FeedSample(HeadTrackerSource::Opentrack,
                            MakeSample(1.0f, 0.5f));
        const HeadTrackerState s = provider.Sample();
        EXPECT_FALSE(s.active);
        EXPECT_NEAR(s.yaw_radians, 0.0f, 1e-6f);
    }

    // ---- Case 4: mouse_sensitivity_rad_per_unit scales Mouse gain ----
    {
        HeadTrackerProvider low;
        HeadTrackerConfig low_cfg;
        low_cfg.use_mouse = true;
        low_cfg.mouse_sensitivity_rad_per_unit = 1.0f;
        low_cfg.smoothing_factor = 0.0f;
        low.SetConfig(low_cfg);
        low.Install();
        low.FeedSample(HeadTrackerSource::Mouse, MakeSample(0.1f, 0.0f));
        const HeadTrackerState low_s = low.Sample();

        HeadTrackerProvider high;
        HeadTrackerConfig high_cfg;
        high_cfg.use_mouse = true;
        high_cfg.mouse_sensitivity_rad_per_unit = 4.0f;  // 4× gain
        high_cfg.smoothing_factor = 0.0f;
        high.SetConfig(high_cfg);
        high.Install();
        high.FeedSample(HeadTrackerSource::Mouse, MakeSample(0.1f, 0.0f));
        const HeadTrackerState high_s = high.Sample();

        // High-sensitivity yaw is 4× low-sensitivity yaw.
        EXPECT_NEAR(high_s.yaw_radians, low_s.yaw_radians * 4.0f, 1e-5f);
        // Negative control: pitch is unaffected (sample.pitch was 0).
        EXPECT_NEAR(high_s.pitch_radians, low_s.pitch_radians, 1e-6f);
    }

    // ---- Case 5: trackir_sensitivity_rad_per_unit scales TrackIR ----
    {
        HeadTrackerProvider low;
        HeadTrackerConfig low_cfg;
        low_cfg.use_trackir = true;
        low_cfg.trackir_sensitivity_rad_per_unit = 1.0f;
        low_cfg.smoothing_factor = 0.0f;
        low.SetConfig(low_cfg);
        low.Install();
        low.FeedSample(HeadTrackerSource::TrackIR, MakeSample(0.1f, 0.0f));
        const HeadTrackerState low_s = low.Sample();

        HeadTrackerProvider high;
        HeadTrackerConfig high_cfg;
        high_cfg.use_trackir = true;
        high_cfg.trackir_sensitivity_rad_per_unit = 3.0f;
        high_cfg.smoothing_factor = 0.0f;
        high.SetConfig(high_cfg);
        high.Install();
        high.FeedSample(HeadTrackerSource::TrackIR, MakeSample(0.1f, 0.0f));
        const HeadTrackerState high_s = high.Sample();

        EXPECT_NEAR(high_s.yaw_radians, low_s.yaw_radians * 3.0f, 1e-5f);
    }

    // ---- Case 6: opentrack_sensitivity_rad_per_unit scales Opentrack ----
    {
        HeadTrackerProvider low;
        HeadTrackerConfig low_cfg;
        low_cfg.use_opentrack = true;
        low_cfg.opentrack_sensitivity_rad_per_unit = 1.0f;
        low_cfg.smoothing_factor = 0.0f;
        low.SetConfig(low_cfg);
        low.Install();
        low.FeedSample(HeadTrackerSource::Opentrack, MakeSample(0.2f, 0.0f));
        const HeadTrackerState low_s = low.Sample();

        HeadTrackerProvider high;
        HeadTrackerConfig high_cfg;
        high_cfg.use_opentrack = true;
        high_cfg.opentrack_sensitivity_rad_per_unit = 5.0f;
        high_cfg.smoothing_factor = 0.0f;
        high.SetConfig(high_cfg);
        high.Install();
        high.FeedSample(HeadTrackerSource::Opentrack, MakeSample(0.2f, 0.0f));
        const HeadTrackerState high_s = high.Sample();

        EXPECT_NEAR(high_s.yaw_radians, low_s.yaw_radians * 5.0f, 1e-5f);
    }

    // ---- Case 7: smoothing_factor controls EMA lag ----
    {
        // Smoothing 0.0 → instant response: yaw jumps to cumulative
        // target on the first Sample after feed.
        HeadTrackerProvider instant;
        HeadTrackerConfig ic;
        ic.use_mouse = true;
        ic.mouse_sensitivity_rad_per_unit = 1.0f;
        ic.smoothing_factor = 0.0f;
        instant.SetConfig(ic);
        instant.Install();
        instant.FeedSample(HeadTrackerSource::Mouse, MakeSample(0.5f, 0.0f));
        const HeadTrackerState inst = instant.Sample();
        EXPECT_NEAR(inst.yaw_radians, 0.5f, 1e-5f);  // exact target

        // Smoothing 0.9 → ~10 % per step: yaw ≈ 0.5 * 0.1 = 0.05
        // after a single (Feed, Sample) pair.
        HeadTrackerProvider laggy;
        HeadTrackerConfig lc;
        lc.use_mouse = true;
        lc.mouse_sensitivity_rad_per_unit = 1.0f;
        lc.smoothing_factor = 0.9f;
        laggy.SetConfig(lc);
        laggy.Install();
        laggy.FeedSample(HeadTrackerSource::Mouse, MakeSample(0.5f, 0.0f));
        const HeadTrackerState lag = laggy.Sample();
        EXPECT_NEAR(lag.yaw_radians, 0.05f, 1e-5f);
        // The laggy state is much smaller than the instant state.
        EXPECT_TRUE(lag.yaw_radians < inst.yaw_radians * 0.2f);
    }

    // ---- Case 8: yaw_delta_rad vs pitch_delta_rad affect separate axes ----
    {
        HeadTrackerProvider provider;
        HeadTrackerConfig cfg;
        cfg.use_mouse = true;
        cfg.mouse_sensitivity_rad_per_unit = 1.0f;
        cfg.smoothing_factor = 0.0f;
        provider.SetConfig(cfg);
        provider.Install();

        // Feed only yaw; pitch must stay 0.
        provider.FeedSample(HeadTrackerSource::Mouse, MakeSample(0.3f, 0.0f));
        const HeadTrackerState a = provider.Sample();
        EXPECT_TRUE(a.yaw_radians > 0.2f);
        EXPECT_NEAR(a.pitch_radians, 0.0f, 1e-6f);

        // Feed only pitch; yaw must NOT change from feed (Sample
        // doesn't reset accumulators — this is the regression we guard
        // against in TestHeadTrackerNoInputDoesNotDrift).
        provider.FeedSample(HeadTrackerSource::Mouse, MakeSample(0.0f, 0.4f));
        const HeadTrackerState b = provider.Sample();
        EXPECT_NEAR(b.yaw_radians, a.yaw_radians, 1e-6f);
        EXPECT_TRUE(b.pitch_radians > 0.3f);
    }

    // ---- Case 9: dominant_source reflects last source fed ----
    {
        HeadTrackerProvider provider;
        HeadTrackerConfig cfg;
        cfg.use_mouse = true;
        cfg.use_trackir = true;
        cfg.use_opentrack = true;
        cfg.smoothing_factor = 0.0f;
        provider.SetConfig(cfg);
        provider.Install();

        provider.FeedSample(HeadTrackerSource::Mouse, MakeSample(0.1f, 0.0f));
        const HeadTrackerState ds_mouse = provider.Sample();
        EXPECT_EQ(static_cast<int>(ds_mouse.dominant_source),
                  static_cast<int>(HeadTrackerSource::Mouse));

        provider.FeedSample(HeadTrackerSource::TrackIR, MakeSample(0.1f, 0.0f));
        const HeadTrackerState ds_trackir = provider.Sample();
        EXPECT_EQ(static_cast<int>(ds_trackir.dominant_source),
                  static_cast<int>(HeadTrackerSource::TrackIR));

        provider.FeedSample(HeadTrackerSource::Opentrack, MakeSample(0.1f, 0.0f));
        const HeadTrackerState ds_opentrack = provider.Sample();
        EXPECT_EQ(static_cast<int>(ds_opentrack.dominant_source),
                  static_cast<int>(HeadTrackerSource::Opentrack));
    }

    // ---- Case 10: Peek() does not advance the EMA ----
    {
        HeadTrackerProvider provider;
        HeadTrackerConfig cfg;
        cfg.use_mouse = true;
        cfg.mouse_sensitivity_rad_per_unit = 1.0f;
        cfg.smoothing_factor = 0.0f;
        provider.SetConfig(cfg);
        provider.Install();

        // Feed a single sample, then Peek twice in a row.
        provider.FeedSample(HeadTrackerSource::Mouse, MakeSample(0.5f, 0.0f));
        const HeadTrackerState p1 = provider.Peek();
        const HeadTrackerState p2 = provider.Peek();
        EXPECT_NEAR(p1.yaw_radians, p2.yaw_radians, 1e-9f);
        // After both Peeks, drain via Sample to confirm Peek didn't
        // perturb the accumulator — the first Sample should still see
        // the full 0.5 rad target.
        const HeadTrackerState s = provider.Sample();
        EXPECT_NEAR(s.yaw_radians, 0.5f, 1e-5f);
    }
}

}  // namespace walkercore