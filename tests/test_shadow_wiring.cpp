// WalkerCore — ShadowProvider wiring tests (M7).
//
// ShadowProvider is a pure function over PlayerState — no Install(),
// no Tick(), just Compute(PlayerState). The wiring responsibilities
// for this provider are minimal:
//
//   - Default config has a sane sun direction (overhead-45°).
//   - Compute() with a player that has feet returns active=true with a
//     projected shadow centre.
//   - Compute() with a player that lacks feet returns active=false.
//   - SetConfig() applies immediately; subsequent Compute uses the new
//     settings.
//   - The dump section "(not initialized)" appears when no provider
//     is wired, and "(no player state)" when the provider is wired
//     but the FrameLoop isn't.

#include "TestHarness.h"

#include <math.h>
#include <string.h>

#include "walkercore/CameraState.h"
#include "walkercore/DiagnosticAccess.h"
#include "walkercore/FrameLoop.h"
#include "walkercore/Log.h"
#include "walkercore/PerFrame.h"
#include "walkercore/PlayerState.h"
#include "walkercore/PolishProviders.h"

namespace {

char g_lines[64][256];
int g_line_count = 0;

extern "C" void ShadowCaptureSink(int /*level*/, const char *msg) {
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

walkercore::PlayerState MakePlayerAt(double x, double y, double z) {
    walkercore::PlayerState p;
    walkercore::PlayerState_Reset(p);
    p.feet_position = walkercore::Vec3{x, y, z};
    p.has_feet = true;
    return p;
}

}  // namespace

namespace walkercore {

void TestShadowWiringDefaultSunDirection() {
    // Default sun is overhead at 45° from +Y, near-unit length. We
    // don't pin the exact vector (M7 lander may adjust), but we DO
    // pin that it has approximately unit length and Y is positive.
    // Tolerance is loose because the literal 0.7071 isn't exactly
    // 1/sqrt(2) — that's documented in src/provider/PolishProviders.cpp.
    ShadowProvider provider;
    const ShadowConfig cfg = provider.config();
    const double len2 = cfg.sun_direction.x * cfg.sun_direction.x +
                        cfg.sun_direction.y * cfg.sun_direction.y +
                        cfg.sun_direction.z * cfg.sun_direction.z;
    EXPECT_NEAR(len2, 1.0, 1e-3);
    // Y must be positive (sun above ground).
    EXPECT_TRUE(cfg.sun_direction.y > 0.0);
}

void TestShadowWiringComputeActiveWithFeet() {
    // Player has feet → shadow is active.
    ShadowProvider provider;
    const PlayerState p = MakePlayerAt(0.0, 0.0, 0.0);
    const PlayerShadowState ss = provider.Compute(p);
    EXPECT_TRUE(ss.active);
}

void TestShadowWiringComputeInactiveWithoutFeet() {
    // PlayerState_Reset clears has_feet to false → shadow inactive.
    ShadowProvider provider;
    PlayerState p;
    PlayerState_Reset(p);
    EXPECT_FALSE(p.has_feet);
    const PlayerShadowState ss = provider.Compute(p);
    EXPECT_FALSE(ss.active);
}

void TestShadowWiringComputeRadiusIsPositive() {
    // The shadow radius is positive and finite. Renderers rely on
    // this to draw a non-degenerate blob.
    ShadowProvider provider;
    const PlayerState p = MakePlayerAt(5.0, 0.0, -3.0);
    const PlayerShadowState ss = provider.Compute(p);
    EXPECT_TRUE(ss.radius > 0.0f);
}

void TestShadowWiringSetConfigAppliesImmediately() {
    // SetConfig must take effect on the next Compute() call. We
    // override scale and softness and verify the output reflects
    // them.
    ShadowProvider provider;
    ShadowConfig cfg = provider.config();
    cfg.scale = 2.0f;
    cfg.softness = 0.5f;
    cfg.rgba = 0x12345678u;
    provider.SetConfig(cfg);

    const PlayerState p = MakePlayerAt(0.0, 0.0, 0.0);
    const PlayerShadowState ss = provider.Compute(p);
    // Radius is capsule_radius (0.35) * scale (2.0) = 0.7.
    EXPECT_NEAR(ss.radius, 0.7f, 1e-5);
    EXPECT_NEAR(ss.softness, 0.5f, 1e-5);
    EXPECT_EQ(static_cast<unsigned>(ss.rgba), 0x12345678u);
}

void TestShadowWiringSunOnHorizonIsSafe() {
    // Edge case: a sun direction with y≈0 would project the shadow
    // to infinity. The provider must clamp this to a safe fallback
    // (overhead sun) rather than producing a NaN/Inf centre.
    ShadowProvider provider;
    ShadowConfig cfg = provider.config();
    cfg.sun_direction = Vec3{1.0, 1e-6, 0.0};  // y ≈ 0
    // Normalise (the default is normalised but we're overriding).
    const double len = sqrt(1.0 + 1e-12);
    cfg.sun_direction.x /= len;
    cfg.sun_direction.y /= len;
    cfg.sun_direction.z /= len;
    provider.SetConfig(cfg);

    const PlayerState p = MakePlayerAt(0.0, 0.0, 0.0);
    const PlayerShadowState ss = provider.Compute(p);
    // The fallback places the shadow at the player's feet; we just
    // require finite values.
    EXPECT_TRUE(ss.worldCentre.x > -1e9 && ss.worldCentre.x < 1e9);
    EXPECT_TRUE(ss.worldCentre.y > -1e9 && ss.worldCentre.y < 1e9);
    EXPECT_TRUE(ss.worldCentre.z > -1e9 && ss.worldCentre.z < 1e9);
}

void TestShadowWiringDumpReportsNotInitialized() {
    // Before M7 wires a shadow provider, the dump reports
    // "(not initialized)" without crashing.
    walkercore_log_shutdown();
    walkercore_log_init(&ShadowCaptureSink);
    ResetCapture();
    diagnostic_internal::SetShadow(nullptr);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("shadow"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    walkercore_log_shutdown();
}

void TestShadowWiringDumpReportsNoPlayerState() {
    // ShadowProvider wired, but no FrameLoop → "(no player state)".
    walkercore_log_shutdown();
    walkercore_log_init(&ShadowCaptureSink);
    ResetCapture();
    ShadowProvider provider;
    diagnostic_internal::SetShadow(&provider);
    diagnostic_internal::SetFrameLoop(nullptr);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("shadow"));
    EXPECT_TRUE(ContainsCapture("(no player state)"));
    diagnostic_internal::SetShadow(nullptr);
    walkercore_log_shutdown();
}

void TestShadowWiringDumpReportsActiveWithFrameLoopAndPlayer() {
    // Full wiring: ShadowProvider + FrameLoop with a positioned
    // player → the dump shows the projected shadow centre.
    walkercore_log_shutdown();
    walkercore_log_init(&ShadowCaptureSink);
    ResetCapture();
    ShadowProvider provider;
    FrameLoop fl;
    PlayerState_Reset(fl.player());
    fl.player().feet_position = Vec3{2.0, 2.0, 0.0};
    fl.player().has_feet = true;
    diagnostic_internal::SetShadow(&provider);
    diagnostic_internal::SetFrameLoop(&fl);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("shadow"));
    EXPECT_TRUE(ContainsCapture("active=true"));
    EXPECT_TRUE(ContainsCapture("radius="));
    diagnostic_internal::SetShadow(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
    walkercore_log_shutdown();
}

// -----------------------------------------------------------------------------
// ShadowProvider::Compute field-exhaustive probe.
//
// ShadowProvider::Compute(PlayerState) reads from 6 sources:
//
//   cfg.sun_direction (Vec3)    → worldCentre (x, z; y when fallback)
//   cfg.scale (float)           → radius
//   cfg.softness (float)        → softness
//   cfg.rgba (uint32_t)         → rgba
//   p.has_feet (bool)           → active
//   p.feet_position (Vec3)      → worldCentre
//
// Each MUST have an observable effect. If a future refactor drops a
// read of one of these, the case for that field fails with a clear
// name in the failure message. Each case uses a fresh provider
// instance so the others aren't perturbed by lingering config state.
// -----------------------------------------------------------------------------

void TestShadowProviderComputeEveryFieldHasObservableEffect() {
    // Build a player that's NOT on the ground (feet.y = 2.0) so the
    // sun-direction projection produces a non-trivial offset. With
    // feet.y = 0 the projection factor t = 0 and worldCentre x/z
    // equal feet x/z for any sun, masking the sun direction's effect.
    PlayerState elevated;
    PlayerState_Reset(elevated);
    elevated.feet_position = Vec3{0.0, 2.0, 0.0};
    elevated.has_feet = true;

    // ---- Case 1: cfg.sun_direction → worldCentre changes ----
    {
        ShadowProvider provider_a;  // default sun.z = -0.7071
        const PlayerShadowState a = provider_a.Compute(elevated);

        ShadowProvider provider_b;
        ShadowConfig cfg_b = provider_b.config();
        // Flip sun.z to +0.999 — projection of an elevated player
        // now goes the OPPOSITE direction on Z. Default sun.z is
        // negative so default projects to +Z when feet.y is positive.
        cfg_b.sun_direction = Vec3{0.0, 0.7071, 0.999};
        provider_b.SetConfig(cfg_b);
        const PlayerShadowState b = provider_b.Compute(elevated);

        // worldCentre.z must differ — flip sun.z from -0.7071 to
        // +0.999 reverses the projection direction on Z.
        EXPECT_TRUE(b.worldCentre.z != a.worldCentre.z);
        // Sanity on the direction (positive-sun.z projection is on the
        // opposite side of feet.z for elevated feet).
        EXPECT_TRUE(b.worldCentre.z < a.worldCentre.z);
        // Negative controls: radius/softness/rgba unchanged.
        EXPECT_NEAR(b.radius, a.radius, 1e-6f);
        EXPECT_NEAR(b.softness, a.softness, 1e-6f);
        EXPECT_EQ(b.rgba, a.rgba);
    }

    // ---- Case 2: cfg.scale → radius changes ----
    {
        ShadowProvider provider_a;
        const PlayerShadowState a = provider_a.Compute(elevated);

        ShadowProvider provider_b;
        ShadowConfig cfg_b = provider_b.config();
        cfg_b.scale = 5.0f;
        provider_b.SetConfig(cfg_b);
        const PlayerShadowState b = provider_b.Compute(elevated);

        EXPECT_TRUE(b.radius > a.radius);  // 0.35 * 5.0 > 0.35 * 1.2
        // Negative controls.
        EXPECT_NEAR(b.softness, a.softness, 1e-6f);
        EXPECT_EQ(b.rgba, a.rgba);
        EXPECT_NEAR(b.worldCentre.x, a.worldCentre.x, 1e-6);
        EXPECT_NEAR(b.worldCentre.z, a.worldCentre.z, 1e-6);
    }

    // ---- Case 3: cfg.softness → softness changes ----
    {
        ShadowProvider provider_a;
        const PlayerShadowState a = provider_a.Compute(elevated);

        ShadowProvider provider_b;
        ShadowConfig cfg_b = provider_b.config();
        cfg_b.softness = 0.9f;
        provider_b.SetConfig(cfg_b);
        const PlayerShadowState b = provider_b.Compute(elevated);

        EXPECT_NEAR(b.softness, 0.9f, 1e-6f);
        EXPECT_TRUE(b.softness != a.softness);
        // Negative controls.
        EXPECT_NEAR(b.radius, a.radius, 1e-6f);
        EXPECT_EQ(b.rgba, a.rgba);
        EXPECT_NEAR(b.worldCentre.x, a.worldCentre.x, 1e-6);
        EXPECT_NEAR(b.worldCentre.z, a.worldCentre.z, 1e-6);
    }

    // ---- Case 4: cfg.rgba → rgba changes ----
    {
        ShadowProvider provider_a;
        const PlayerShadowState a = provider_a.Compute(elevated);

        ShadowProvider provider_b;
        ShadowConfig cfg_b = provider_b.config();
        cfg_b.rgba = 0xDEADBEEFu;
        provider_b.SetConfig(cfg_b);
        const PlayerShadowState b = provider_b.Compute(elevated);

        EXPECT_EQ(b.rgba, 0xDEADBEEFu);
        EXPECT_TRUE(b.rgba != a.rgba);
        // Negative controls.
        EXPECT_NEAR(b.radius, a.radius, 1e-6f);
        EXPECT_NEAR(b.softness, a.softness, 1e-6f);
        EXPECT_NEAR(b.worldCentre.x, a.worldCentre.x, 1e-6);
        EXPECT_NEAR(b.worldCentre.z, a.worldCentre.z, 1e-6);
    }

    // ---- Case 5: p.has_feet → active flips ----
    {
        ShadowProvider provider;
        PlayerState no_feet = elevated;
        no_feet.has_feet = false;
        const PlayerShadowState off = provider.Compute(no_feet);
        EXPECT_FALSE(off.active);
        // Sanity: with has_feet=true the same provider returns
        // active=true. Pin that baseline.
        EXPECT_TRUE(provider.Compute(elevated).active);
    }

    // ---- Case 6: p.feet_position → worldCentre changes ----
    {
        ShadowProvider provider;
        PlayerState moved;
        PlayerState_Reset(moved);
        moved.feet_position = Vec3{10.0, 0.0, -7.0};  // feet on ground
        moved.has_feet = true;
        const PlayerShadowState s = provider.Compute(moved);
        // With feet.y = 0, the projection factor t = 0, so
        // worldCentre.x = feet.x and worldCentre.z = feet.z.
        EXPECT_NEAR(s.worldCentre.x, 10.0, 1e-3);
        EXPECT_NEAR(s.worldCentre.z, -7.0, 1e-3);
        EXPECT_TRUE(s.active);
        // Negative controls: same provider config → radius/softness/rgba
        // match the default-config baseline.
        const PlayerShadowState baseline = provider.Compute(elevated);
        EXPECT_NEAR(s.radius, baseline.radius, 1e-6f);
        EXPECT_NEAR(s.softness, baseline.softness, 1e-6f);
        EXPECT_EQ(s.rgba, baseline.rgba);
    }
}

}  // namespace walkercore