// WalkerCore — FlashlightProvider tests.
//
// Verifies the on/off state transitions and the Compute() snapshot that
// the dump surfaces. The key invariants:
//   - A fresh FlashlightProvider starts OFF.
//   - Toggle() flips state, every press flips once.
//   - SetOn() is authoritative — Toggle() after SetOn(false) goes on,
//     Toggle() after SetOn(true) goes off.
//   - Compute() origin == camera eye_position.
//   - Compute() direction matches the camera's forward vector for the
//     same yaw/pitch used in CameraState_BuildViewMatrix.
//   - When OFF, intensity is 0; when ON, intensity is 1.
//   - The dump section "(not initialized)" appears when no Flashlight is
//     wired, and the live snapshot appears when one is wired via the
//     diagnostic accessor (the same path PluginEntry uses).
//
// These tests are deterministic and cheap — no real D3D11 / no real
// game state. They pin the M7 contract so the MM lander can implement
// the real point-light injection without re-litigating the state model.

#include "TestHarness.h"

#include <math.h>
#include <string.h>

#include "walkercore/CameraState.h"
#include "walkercore/DiagnosticAccess.h"
#include "walkercore/Log.h"
#include "walkercore/PerFrame.h"
#include "walkercore/PlayerState.h"
#include "walkercore/PolishProviders.h"

namespace {

// Capture sink for the diagnostic-dump section. We only inspect whether
// the dump emitted the "flashlight :" line, not its level.
char g_lines[64][256];
int g_line_count = 0;

extern "C" void FlashlightCaptureSink(int /*level*/, const char *msg) {
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

// Build a CameraState with the given yaw/pitch, eye, FOV.
walkercore::CameraState MakeCamera(double eye_x, double eye_y, double eye_z,
                                    float yaw, float pitch) {
    walkercore::CameraState c;
    walkercore::CameraState_Reset(c);
    c.eye_position = walkercore::Vec3{eye_x, eye_y, eye_z};
    c.yaw_radians = yaw;
    c.pitch_radians = pitch;
    walkercore::CameraState_Update(c);
    return c;
}

}  // namespace

namespace walkercore {

void TestFlashlightInitialStateOff() {
    // A fresh provider is OFF until something explicitly turns it on.
    FlashlightProvider provider;
    EXPECT_FALSE(provider.isOn());
}

void TestFlashlightToggleFlips() {
    // Toggle() must flip on/off on every call. Two presses from a clean
    // start return to OFF.
    FlashlightProvider provider;
    EXPECT_FALSE(provider.isOn());
    provider.Toggle();
    EXPECT_TRUE(provider.isOn());
    provider.Toggle();
    EXPECT_FALSE(provider.isOn());
    provider.Toggle();
    EXPECT_TRUE(provider.isOn());
}

void TestFlashlightSetOnExplicit() {
    // SetOn is authoritative — it overrides whatever the current state
    // was. After SetOn(true), Toggle() goes to false.
    FlashlightProvider provider;
    provider.SetOn(true);
    EXPECT_TRUE(provider.isOn());
    provider.Toggle();
    EXPECT_FALSE(provider.isOn());
    provider.SetOn(true);
    EXPECT_TRUE(provider.isOn());
    provider.SetOn(false);
    EXPECT_FALSE(provider.isOn());
}

void TestFlashlightOffHasZeroIntensity() {
    // While OFF, the snapshot's intensity MUST be 0. Consumers can
    // trust intensity as the "do I render the cone?" flag without an
    // extra on/off branch.
    FlashlightProvider provider;
    const CameraState c = MakeCamera(0.0, 1.7, 5.0, 0.5f, 0.1f);
    const FlashlightState s = provider.Compute(c);
    EXPECT_FALSE(s.on);
    EXPECT_NEAR(s.intensity, 0.0f, 1e-6f);
}

void TestFlashlightOnHasUnitIntensity() {
    // While ON, the snapshot's intensity MUST be 1. The renderer uses
    // this to drive a uniform-scale cone mesh in M7 lander.
    FlashlightProvider provider;
    provider.SetOn(true);
    const CameraState c = MakeCamera(0.0, 1.7, 5.0, 0.5f, 0.1f);
    const FlashlightState s = provider.Compute(c);
    EXPECT_TRUE(s.on);
    EXPECT_NEAR(s.intensity, 1.0f, 1e-6f);
}

void TestFlashlightComputeOriginMatchesCamera() {
    // The flashlight lives at the camera origin — that's the whole point
    // of being a headlamp. Verbatim copy of eye_position.
    FlashlightProvider provider;
    provider.SetOn(true);
    const CameraState c = MakeCamera(2.0, 1.8, -3.0, 0.0f, 0.0f);
    const FlashlightState s = provider.Compute(c);
    EXPECT_NEAR(s.origin.x, 2.0, 1e-6);
    EXPECT_NEAR(s.origin.y, 1.8, 1e-6);
    EXPECT_NEAR(s.origin.z, -3.0, 1e-6);
}

void TestFlashlightComputeDirectionMatchesCameraForward() {
    // The flashlight direction MUST match the camera's forward vector
    // — same convention as CameraState_BuildViewMatrix. At yaw=0,
    // pitch=0 the camera looks down -Z, so direction = (0, 0, -1).
    FlashlightProvider provider;
    provider.SetOn(true);
    {
        const CameraState c = MakeCamera(0.0, 0.0, 0.0, 0.0f, 0.0f);
        const FlashlightState s = provider.Compute(c);
        EXPECT_NEAR(s.direction.x, 0.0, 1e-6);
        EXPECT_NEAR(s.direction.y, 0.0, 1e-6);
        EXPECT_NEAR(s.direction.z, -1.0, 1e-6);
    }
    // At yaw=PI/2, pitch=0 the forward vector is (1, 0, 0).
    {
        const CameraState c = MakeCamera(0.0, 0.0, 0.0,
                                          static_cast<float>(M_PI_2),
                                          0.0f);
        const FlashlightState s = provider.Compute(c);
        EXPECT_NEAR(s.direction.x, 1.0, 1e-5);
        EXPECT_NEAR(s.direction.y, 0.0, 1e-6);
        EXPECT_NEAR(s.direction.z, 0.0, 1e-5);
    }
    // Pitched up at +0.3 rad: forward.y is positive.
    {
        const CameraState c = MakeCamera(0.0, 0.0, 0.0, 0.0f, 0.3f);
        const FlashlightState s = provider.Compute(c);
        EXPECT_NEAR(s.direction.x, 0.0, 1e-6);
        EXPECT_NEAR(s.direction.y, sinf(0.3f), 1e-5);
        EXPECT_NEAR(s.direction.z, -cosf(0.3f), 1e-5);
    }
}

void TestFlashlightDirectionIsNormalised() {
    // The flashlight direction is a UNIT vector regardless of the
    // yaw/pitch used. |d|² must equal 1.
    FlashlightProvider provider;
    provider.SetOn(true);
    const CameraState c = MakeCamera(0.0, 0.0, 0.0, 1.234f, -0.456f);
    const FlashlightState s = provider.Compute(c);
    const double len2 = s.direction.x * s.direction.x +
                        s.direction.y * s.direction.y +
                        s.direction.z * s.direction.z;
    EXPECT_NEAR(len2, 1.0, 1e-5);
}

void TestFlashlightConeAnglesArePositive() {
    // Inner and outer cone half-angles must be positive and inner <=
    // outer (a wider penumbra is what gives the flashlight a "soft
    // edge"). Pinned so M7 lander can render the cone mesh from these
    // values directly.
    FlashlightProvider provider;
    const CameraState c = MakeCamera(0.0, 0.0, 0.0, 0.0f, 0.0f);
    const FlashlightState s = provider.Compute(c);
    EXPECT_TRUE(s.inner_cone_rad > 0.0f);
    EXPECT_TRUE(s.outer_cone_rad > 0.0f);
    EXPECT_TRUE(s.outer_cone_rad >= s.inner_cone_rad);
}

void TestFlashlightDumpReportsNotInitializedWhenUnwired() {
    // Before the M7 lander wires a flashlight, the dump must report
    // "(not initialized)" rather than crashing or printing garbage.
    walkercore_log_shutdown();
    walkercore_log_init(&FlashlightCaptureSink);
    ResetCapture();
    diagnostic_internal::SetFlashlight(nullptr);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("flashlight"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    walkercore_log_shutdown();
}

void TestFlashlightDumpReportsOffWhenProviderOff() {
    // With a wired-but-OFF flashlight the dump shows "on=false intensity=0".
    walkercore_log_shutdown();
    walkercore_log_init(&FlashlightCaptureSink);
    ResetCapture();

    FlashlightProvider provider;  // off by default
    diagnostic_internal::SetFlashlight(&provider);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("flashlight"));
    EXPECT_TRUE(ContainsCapture("on=false"));
    EXPECT_TRUE(ContainsCapture("intensity=0.00"));

    diagnostic_internal::SetFlashlight(nullptr);
    walkercore_log_shutdown();
}

void TestFlashlightDumpReportsOnWhenProviderOn() {
    // With a wired AND ON flashlight the dump shows "on=true intensity=1".
    walkercore_log_shutdown();
    walkercore_log_init(&FlashlightCaptureSink);
    ResetCapture();

    FlashlightProvider provider;
    provider.SetOn(true);
    diagnostic_internal::SetFlashlight(&provider);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("flashlight"));
    EXPECT_TRUE(ContainsCapture("on=true"));
    EXPECT_TRUE(ContainsCapture("intensity=1.00"));

    diagnostic_internal::SetFlashlight(nullptr);
    walkercore_log_shutdown();
}

void TestFlashlightDumpMirrorsToggle() {
    // Toggling the wired provider flips the dump's on/off field. Pins
    // the contract that walker_per_frame_tick's rising-edge handler can
    // be observed in the dump without restarts.
    walkercore_log_shutdown();
    walkercore_log_init(&FlashlightCaptureSink);
    ResetCapture();

    FlashlightProvider provider;
    diagnostic_internal::SetFlashlight(&provider);

    provider.Toggle();
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("on=true"));

    ResetCapture();
    provider.Toggle();
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("on=false"));

    diagnostic_internal::SetFlashlight(nullptr);
    walkercore_log_shutdown();
}

}  // namespace walkercore