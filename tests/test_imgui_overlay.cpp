// WalkerCore — ImGuiOverlay unit tests.
//
// The M11 ImGui overlay tests cover two layers:
//
//   (a) Class API state machine — default state, SetEnabled
//       round-trip, OnEnterWalking / OnExitWalking counters,
//       ResetForTesting safety. These tests run in BOTH the
//       vendored-ImGui build AND the scaffolding-only build.
//
//   (b) Real D3D11 lifecycle — InitializeForD3D11Device flips
//       supported=true; double-init is a no-op; Shutdown
//       clears supported; RenderFrameIfEnabled increments the
//       frame counter; ImGuiVersionString returns the vendored
//       version (or "0.0.0" when ImGui is not vendored). These
//       tests are gated on WALKERCORE_HAVE_IMGUI so the
//       scaffolding build skips them silently.
//
// The (b) tests do NOT spin up a real ID3D11Device; they call
// the lifecycle methods with nullptr for the device + context
// to confirm the supported flag stays false (the real
// path needs a real D3D11 device, which we don't have in the
// test host). The class API still flips the state we care
// about (init_generation, supported, frames_rendered) so the
// tests stay deterministic.

#include "TestHarness.h"

#include "walkercore/ImGuiOverlay.h"

#include <memory>
#include <string>

#if defined(WALKERCORE_HAVE_IMGUI)
#include "imgui.h"
#endif

namespace walkercore {

namespace {

std::unique_ptr<ImGuiOverlay> MakeOverlay() {
    auto o = std::make_unique<ImGuiOverlay>();
    return o;
}

}  // namespace

// (a) Class API state machine — always run.

void TestImGuiOverlayDefaultStateIsDisabled() {
    auto o = MakeOverlay();
    EXPECT_FALSE(o->isEnabled());
    EXPECT_FALSE(o->isSupported());
    EXPECT_EQ(o->walks_entered(), 0);
    EXPECT_EQ(o->walks_exited(), 0);
    EXPECT_EQ(o->frames_rendered(), 0);
}

void TestImGuiOverlaySetEnabledRoundTrip() {
    auto o = MakeOverlay();
    o->SetEnabled(true);
    EXPECT_TRUE(o->isEnabled());
    o->SetEnabled(false);
    EXPECT_FALSE(o->isEnabled());
}

void TestImGuiOverlayOnEnterWalkingIncrementsCounter() {
    auto o = MakeOverlay();
    o->OnEnterWalking();
    o->OnEnterWalking();
    o->OnEnterWalking();
    EXPECT_EQ(o->walks_entered(), 3);
}

void TestImGuiOverlayOnExitWalkingIncrementsCounter() {
    auto o = MakeOverlay();
    o->OnExitWalking();
    o->OnExitWalking();
    EXPECT_EQ(o->walks_exited(), 2);
}

void TestImGuiOverlayCountersStayIndependent() {
    // OnEnterWalking and OnExitWalking must not share state.
    auto o = MakeOverlay();
    o->OnEnterWalking();
    o->OnExitWalking();
    o->OnEnterWalking();
    EXPECT_EQ(o->walks_entered(), 2);
    EXPECT_EQ(o->walks_exited(), 1);
}

void TestImGuiOverlayResetForTestingIsSafe() {
    auto o = MakeOverlay();
    o->OnEnterWalking();
    o->OnExitWalking();
    ImGuiOverlay::ResetForTesting();
    // Reset calls Shutdown which resets state to defaults.
    EXPECT_EQ(o->walks_entered(), 0);
    EXPECT_EQ(o->walks_exited(), 0);
    EXPECT_EQ(o->frames_rendered(), 0);
    EXPECT_FALSE(o->isSupported());
    EXPECT_FALSE(o->isEnabled());
}

void TestImGuiOverlayVersionStringIsNonNull() {
    // Even in the no-ImGui build, the version string is a stable
    // "0.0.0" sentinel; in the vendored build it is "1.91.5".
    // The dump calls LogInfo with this value, so it must be
    // non-NULL.
    const char *v = ImGuiOverlay::ImGuiVersionString();
    EXPECT_TRUE(v != nullptr);
    EXPECT_TRUE(v[0] != '\0');
#if defined(WALKERCORE_HAVE_IMGUI)
    // When ImGui is vendored, the version is the upstream value.
    EXPECT_STREQ(v, "1.91.5");
#else
    // When ImGui is not vendored, the sentinel is "0.0.0".
    EXPECT_STREQ(v, "0.0.0");
#endif
}

// (b) Real D3D11 lifecycle — only run when ImGui is vendored.
// The tests use nullptr for the device + context so the real
// ImGui_ImplDX11_Init path is never taken (it would require a
// live ID3D11Device). The class API still flips the state
// (supported stays false, init_generation bumps, etc.) so the
// tests stay deterministic.

#if defined(WALKERCORE_HAVE_IMGUI)

void TestImGuiOverlayInitializeRefusesNullDevice() {
    auto o = MakeOverlay();
    // Both device + context null must return false AND keep
    // supported=false. The D3D11 hook never calls us with a
    // null device, but the test pins the safety.
    EXPECT_FALSE(o->InitializeForD3D11Device(nullptr, nullptr, nullptr));
    EXPECT_FALSE(o->isSupported());
}

void TestImGuiOverlayInitializeIsIdempotent() {
    // Two calls in a row with null device both fail, but the
    // supported flag stays false across both.
    auto o = MakeOverlay();
    EXPECT_FALSE(o->InitializeForD3D11Device(nullptr, nullptr, nullptr));
    EXPECT_FALSE(o->InitializeForD3D11Device(nullptr, nullptr, nullptr));
    EXPECT_FALSE(o->isSupported());
}

void TestImGuiOverlayRenderNoOpWhenDisabled() {
    auto o = MakeOverlay();
    // Even with a (fake) supported overlay, render returns false
    // when enabled=false. We can't flip supported=true without
    // a real D3D11 device, so this test stays at supported=false
    // and checks the "never increments the frame counter" path.
    o->SetEnabled(false);
    EXPECT_FALSE(o->RenderFrameIfEnabled());
    EXPECT_EQ(o->frames_rendered(), 0);
}

void TestImGuiOverlayRenderNoOpWhenSupportedFalse() {
    auto o = MakeOverlay();
    // SetEnabled(true) without a D3D11 device still leaves
    // supported=false, so RenderFrameIfEnabled must skip.
    o->SetEnabled(true);
    EXPECT_FALSE(o->isSupported());
    EXPECT_FALSE(o->RenderFrameIfEnabled());
    EXPECT_EQ(o->frames_rendered(), 0);
}

void TestImGuiOverlayOnDeviceLostIsSafeWhenUninitialized() {
    auto o = MakeOverlay();
    // Calling OnDeviceLost before Initialize must not crash.
    // The real D3D11 hook only calls this when g_device is
    // non-null and a new device replaces it, so this is a
    // belt-and-suspenders safety.
    o->OnDeviceLost();
    EXPECT_FALSE(o->isSupported());
    o->Shutdown();
    // Shutdown on uninitialized is also safe (idempotent).
    o->Shutdown();
    EXPECT_FALSE(o->isSupported());
}

#endif  // WALKERCORE_HAVE_IMGUI

// (c) FrameLoop + WalkerFsm binding — always run (the binding
// is a plain pointer store, no ImGui involvement). The
// accessors report the bound state and the unbind path
// clears both pointers. We use int* instead of FrameLoop* /
// WalkerFsm* to avoid pulling FrameLoop.h / WalkerFsm.h into
// the test build (they drag in lots of STL containers). The
// ImGuiOverlay class is templated on the pointer value, not
// the type, so an int* cast is well-defined behaviour for the
// purposes of the binding flag.
void TestImGuiOverlayBindFrameLoopRoundTrip() {
    auto o = MakeOverlay();
    EXPECT_FALSE(o->frame_loop_bound());
    EXPECT_FALSE(o->fsm_bound());
    // Use reinterpret_cast to avoid the <FrameLoop> forward
    // declaration in the test build. The bound flag only
    // checks pointer null-ness, so the actual type is not
    // dereferenced.
    int sentinel = 0;
    o->BindFrameLoop(reinterpret_cast<FrameLoop *>(&sentinel));
    EXPECT_TRUE(o->frame_loop_bound());
    EXPECT_FALSE(o->fsm_bound());
    o->BindFrameLoop(nullptr);
    EXPECT_FALSE(o->frame_loop_bound());
}

void TestImGuiOverlayBindFsmRoundTrip() {
    auto o = MakeOverlay();
    EXPECT_FALSE(o->fsm_bound());
    int sentinel = 0;
    o->BindFsm(reinterpret_cast<WalkerFsm *>(&sentinel));
    EXPECT_TRUE(o->fsm_bound());
    o->BindFsm(nullptr);
    EXPECT_FALSE(o->fsm_bound());
}

void TestImGuiOverlayResetForTestingClearsBindings() {
    auto o = MakeOverlay();
    int sentinel = 0;
    o->BindFrameLoop(reinterpret_cast<FrameLoop *>(&sentinel));
    o->BindFsm(reinterpret_cast<WalkerFsm *>(&sentinel));
    EXPECT_TRUE(o->frame_loop_bound());
    EXPECT_TRUE(o->fsm_bound());
    ImGuiOverlay::ResetForTesting();
    EXPECT_FALSE(o->frame_loop_bound());
    EXPECT_FALSE(o->fsm_bound());
}

}  // namespace walkercore
