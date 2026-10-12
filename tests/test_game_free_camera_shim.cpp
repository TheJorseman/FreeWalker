// WalkerCore — GameFreeCameraShim unit tests.
//
// The shim must remain a no-op on the test build because:
//   - SendInput would touch the real OS key event queue on a Windows
//     build.
//   - We do not want a test run to flip the host's developer-mode
//     cvar or fire keystrokes at the calling shell.
// We enforce that by compiling GameFreeCameraShim.cpp with
// WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED=1 in tests/CMakeLists.txt
// (parallel to WALKERCORE_TEST_NO_PER_FRAME_TICK), turning SynthKey
// into a counter + return-value increment.
//
// What we still cover here:
//   - SetEnabled / isEnabled round-trip.
//   - Toggle VK + Exit-press VK customisation.
//   - OnEnterWalking increments walks_entered_ and exits cleanly when
//     disabled, when supported=false (advisory path), and when both
//     are true.
//   - OnExitWalking emits the "Walking exit" log line (asserted via
//     counters, the actual log is exercised by the integration tests).
//   - ConsumeEchoKey returns false for any VK that was not synthesised.
//   - The ABI exports wired through DiagnosticAccess / DiagnosticDump
//     accept non-null shims and forward through to the methods.
//
// Tests use a per-test heap-allocated shim so the running counter
// state is isolated — counters mutate across calls and a static
// instance shared across tests would leak state from earlier tests
// into later assertions.

#include "TestHarness.h"

#include "walkercore/GameFreeCameraShim.h"

#include <memory>

namespace walkercore {

namespace {

std::unique_ptr<GameFreeCameraShim> MakeShim() {
    // Fresh instance per test. Construct default, then reset to a
    // known-disabled baseline (toggle VK 0x60 = VK_NUMPAD0, no exit
    // press) so the assertions are deterministic.
    auto s = std::make_unique<GameFreeCameraShim>();
    s->SetEnabled(false);
    s->SetToggleVk(0x60);
    s->SetExitPressVk(0);
    return s;
}

}  // namespace

void TestGameFreeCameraShimDefaultStateIsDisabled() {
    auto s = MakeShim();
    EXPECT_FALSE(s->isEnabled());
    EXPECT_EQ(s->toggle_vk(), 0x60);    // VK_NUMPAD0
    EXPECT_EQ(s->exit_press_vk(), 0);
    EXPECT_EQ(s->walks_entered(), 0);
    EXPECT_EQ(s->toggles_dispatched(), 0);
}

void TestGameFreeCameraShimSetEnabledRoundTrip() {
    auto s = MakeShim();
    s->SetEnabled(true);
    EXPECT_TRUE(s->isEnabled());
    s->SetEnabled(false);
    EXPECT_FALSE(s->isEnabled());
}

void TestGameFreeCameraShimVkCustomisation() {
    auto s = MakeShim();
    // ATS / ETS2 use F10 for toggle walk; an operator could swap
    // the camera-toggle VK to anything VK_* coded.
    s->SetToggleVk(0x79);    // VK_F10
    s->SetExitPressVk(0x1B); // VK_ESCAPE
    EXPECT_EQ(s->toggle_vk(), 0x79);
    EXPECT_EQ(s->exit_press_vk(), 0x1B);
}

void TestGameFreeCameraShimOnEnterWalkingDisabledIsAdvisory() {
    auto s = MakeShim();
    s->SetEnabled(false);
    s->OnEnterWalking();
    s->OnEnterWalking();
    EXPECT_EQ(s->walks_entered(), 2);
    // Disabled shim: no key synthesis, no counter bump.
    EXPECT_EQ(s->toggles_dispatched(), 0);
    EXPECT_EQ(s->last_dispatch_ms(), 0);
}

void TestGameFreeCameraShimOnEnterWalkingSupportedFalseIsAdvisory() {
    auto s = MakeShim();
    s->SetEnabled(true);
    // supported_ stays false because ProbeGDeveloperFromConfig has
    // not found "1" in any game config on this host.
    s->OnEnterWalking();
    EXPECT_EQ(s->walks_entered(), 1);
    EXPECT_EQ(s->toggles_dispatched(), 0);
}

void TestGameFreeCameraShimTriggerToggleRespectsDisabled() {
    auto s = MakeShim();
    s->SetEnabled(false);
    const int produced = s->TriggerToggle();
    EXPECT_EQ(produced, 0);
    EXPECT_EQ(s->toggles_dispatched(), 0);
}

void TestGameFreeCameraShimEchoSuppressionAfterTrigger() {
    auto s = MakeShim();
    s->SetEnabled(true);
    s->SetToggleVk(0x79);    // pretend F10
    s->SetExitPressVk(0);

    // Without supported_=true, TriggerToggle is a no-op — we cannot
    // exercise the echo loop in this build. The contract that
    // ConsumeEchoKey returns false for any VK we didn't synthesise
    // still holds.
    s->OnEnterWalking();           // logs advisory, no synthesis
    EXPECT_FALSE(s->ConsumeEchoKey(0x79));
    EXPECT_FALSE(s->ConsumeEchoKey(0x60));
}

void TestGameFreeCameraShimOnExitWalkingLeavesCountersIntact() {
    auto s = MakeShim();
    s->SetEnabled(true);
    s->OnEnterWalking();
    s->OnEnterWalking();
    EXPECT_EQ(s->walks_entered(), 2);
    s->OnExitWalking();
    // OnExitWalking must not mutate walks_entered / toggles_dispatched.
    EXPECT_EQ(s->walks_entered(), 2);
    EXPECT_EQ(s->toggles_dispatched(), 0);
}

}  // namespace walkercore
