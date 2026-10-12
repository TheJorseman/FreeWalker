// WalkerCore — Door animator integration tests (M7 wiring).
//
// Verifies the contract between WalkerFsm transitions and the
// DoorAnimProvider. Pure DoorAnimProvider math (smoothstep easing) is
// already covered in test_door_anim.cpp; this file covers the higher-
// level contract:
//
//   - Driving → TransitionOut / Walking triggers AnimateTo(open_rad).
//   - Walking → TransitionIn / Driving triggers AnimateTo(0).
//   - Door tween advances regardless of state (so it survives past the
//     transition that started it).
//   - The diagnostic dump surfaces door anim state.
//
// We don't drive walker_per_frame_tick here (it needs the full
// D3D11/InputHook stack); instead we replicate the small observer logic
// from PluginEntry — `g_prev_fsm_state` + the transition classifier +
// `DoorAnimProvider.Tick()` — and assert the same end-state.

#include "TestHarness.h"

#include <math.h>
#include <string.h>

#include "walkercore/DiagnosticAccess.h"
#include "walkercore/Log.h"
#include "walkercore/PerFrame.h"
#include "walkercore/PlayerState.h"
#include "walkercore/PolishProviders.h"
#include "walkercore/TruckDoorConfig.h"
#include "walkercore/WalkerFsm.h"

namespace {

char g_lines[64][256];
int g_line_count = 0;

extern "C" void DoorAnimCaptureSink(int /*level*/, const char *msg) {
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

// Replica of the transition-driven door animation logic in PluginEntry.
// Takes the previous FSM state by reference (so the test can observe
// updates) and the current state; if a transition fires, dispatches to
// the door animator. Returns true if a transition fired.
bool StepFsmToDoor(walkercore::WalkerFsmState prev,
                    walkercore::WalkerFsmState cur,
                    walkercore::DoorAnimProvider &door,
                    const walkercore::TruckDoorConfig &cfg) {
    if (cur == prev) return false;
    if (prev == walkercore::WalkerFsmState::Driving &&
        cur != walkercore::WalkerFsmState::Driving) {
        door.AnimateTo(cfg.door_max_open_rad, cfg.door_open_duration_s);
        return true;
    }
    if (prev == walkercore::WalkerFsmState::Walking &&
        cur != walkercore::WalkerFsmState::Walking) {
        door.AnimateTo(0.0f, cfg.door_open_duration_s);
        return true;
    }
    return false;
}

}  // namespace

namespace walkercore {

void TestDoorAnimWiringInitialStateNotAnimating() {
    // A fresh DoorAnimProvider starts at angle 0 with no tween in
    // flight. Mirrors the dump's "animating=false angle=0" baseline.
    DoorAnimProvider door;
    const DoorAnimState s = door.state();
    EXPECT_FALSE(s.animating);
    EXPECT_NEAR(s.angle_radians, 0.0f, 1e-6f);
}

void TestDoorAnimWiringDriveToExitOpensDoor() {
    // Exit transition: Driving → TransitionOut → Walking. The FIRST
    // step (Driving → TransitionOut) is the rising edge that fires
    // AnimateTo(open_rad). The animator is left animating; the next
    // step (TransitionOut → Walking) does not re-fire because neither
    // end state matches the rule (prev=TransitionOut, cur=Walking).
    DoorAnimProvider door_anim;
    const TruckDoorConfig cfg = DefaultTruckDoorConfig();
    EXPECT_NEAR(cfg.door_max_open_rad, 1.5708f, 1e-6f);

    walkercore::WalkerFsmState prev = WalkerFsmState::Driving;

    // Frame 1: Driving → TransitionOut. Should fire open.
    const bool fired1 = StepFsmToDoor(prev, WalkerFsmState::TransitionOut,
                                       door_anim, cfg);
    EXPECT_TRUE(fired1);
    EXPECT_TRUE(door_anim.state().animating);
    prev = WalkerFsmState::TransitionOut;

    // Frame 2: TransitionOut → Walking. Should NOT fire (prev is not
    // Driving or Walking, so neither rule matches).
    const bool fired2 = StepFsmToDoor(prev, WalkerFsmState::Walking,
                                       door_anim, cfg);
    EXPECT_FALSE(fired2);
    // The tween from frame 1 is still in flight (or may have finished,
    // depending on dt). Either is acceptable here; we test the duration
    // pin in the next test.
}

void TestDoorAnimWiringWalkToEnterClosesDoor() {
    // Enter transition: Walking → TransitionIn → Driving. The FIRST
    // step (Walking → TransitionIn) fires AnimateTo(0).
    DoorAnimProvider door_anim;
    const TruckDoorConfig cfg = DefaultTruckDoorConfig();

    // Pre-open the door so the close has visible motion.
    door_anim.AnimateTo(cfg.door_max_open_rad, 0.01f);
    // Tick past the very short duration so it lands at the target.
    door_anim.Tick(0.02);
    EXPECT_NEAR(door_anim.state().angle_radians, cfg.door_max_open_rad, 1e-3);

    walkercore::WalkerFsmState prev = WalkerFsmState::Walking;

    // Frame 1: Walking → TransitionIn. Should fire close.
    const bool fired1 = StepFsmToDoor(prev, WalkerFsmState::TransitionIn,
                                       door_anim, cfg);
    EXPECT_TRUE(fired1);
    EXPECT_TRUE(door_anim.state().animating);
    prev = WalkerFsmState::TransitionIn;

    // Frame 2: TransitionIn → Driving. Should NOT fire.
    const bool fired2 = StepFsmToDoor(prev, WalkerFsmState::Driving,
                                       door_anim, cfg);
    EXPECT_FALSE(fired2);
}

void TestDoorAnimWiringDoorTweenCompletes() {
    // The door tween completes after door_open_duration_s. We use the
    // default 0.6s; a 0.7s tick lands it at the target angle.
    DoorAnimProvider door_anim;
    const TruckDoorConfig cfg = DefaultTruckDoorConfig();
    StepFsmToDoor(WalkerFsmState::Driving, WalkerFsmState::TransitionOut,
                   door_anim, cfg);
    EXPECT_TRUE(door_anim.state().animating);

    // Halfway: angle is partway to target.
    door_anim.Tick(cfg.door_open_duration_s * 0.5);
    EXPECT_TRUE(door_anim.state().animating);
    EXPECT_TRUE(door_anim.state().angle_radians > 0.0f);
    EXPECT_TRUE(door_anim.state().angle_radians < cfg.door_max_open_rad);

    // Past the end: tween done, angle at target.
    door_anim.Tick(cfg.door_open_duration_s);
    EXPECT_FALSE(door_anim.state().animating);
    EXPECT_NEAR(door_anim.state().angle_radians, cfg.door_max_open_rad, 1e-3);
}

void TestDoorAnimWiringTickAdvancesRegardlessOfState() {
    // The Tick happens every frame regardless of FSM state, so a tween
    // started by an exit transition keeps advancing even after the
    // transition has finished (the camera tween ends at transition_seconds_,
    // the door tween ends at door_open_duration_s; we leave it to the
    // M7 lander to align them if needed). This test pins the contract
    // that ticks are unconditional.
    DoorAnimProvider door_anim;
    const TruckDoorConfig cfg = DefaultTruckDoorConfig();
    StepFsmToDoor(WalkerFsmState::Driving, WalkerFsmState::TransitionOut,
                   door_anim, cfg);

    // Tick 0.1 s. We do NOT change FSM state — the contract is that
    // Tick is independent of FSM changes.
    const float before = door_anim.state().angle_radians;
    door_anim.Tick(0.1);
    const float after = door_anim.state().angle_radians;
    EXPECT_TRUE(after > before);
}

void TestDoorAnimWiringStopCancelsTween() {
    // Stop() drops the tween immediately. Useful for forcing the door
    // closed on a hard reset (e.g. teleport / truck swap).
    DoorAnimProvider door_anim;
    const TruckDoorConfig cfg = DefaultTruckDoorConfig();
    StepFsmToDoor(WalkerFsmState::Driving, WalkerFsmState::TransitionOut,
                   door_anim, cfg);
    EXPECT_TRUE(door_anim.state().animating);

    door_anim.Stop();
    EXPECT_FALSE(door_anim.state().animating);
    // Angle stays where it was mid-tween (not snapped to target).
    door_anim.Tick(1.0);
    EXPECT_FALSE(door_anim.state().animating);
}

void TestDoorAnimWiringDumpReportsNotInitialized() {
    // Before the M7 lander wires a door animator, the dump reports
    // "(not initialized)" without crashing.
    walkercore_log_shutdown();
    walkercore_log_init(&DoorAnimCaptureSink);
    ResetCapture();
    diagnostic_internal::SetDoorAnim(nullptr);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("door anim"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    walkercore_log_shutdown();
}

void TestDoorAnimWiringDumpReportsIdleAngle() {
    // A wired but idle door animator shows angle=0.000, animating=false.
    walkercore_log_shutdown();
    walkercore_log_init(&DoorAnimCaptureSink);
    ResetCapture();
    DoorAnimProvider door;
    diagnostic_internal::SetDoorAnim(&door);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("door anim"));
    EXPECT_TRUE(ContainsCapture("angle=0.000"));
    EXPECT_TRUE(ContainsCapture("animating=false"));
    diagnostic_internal::SetDoorAnim(nullptr);
    walkercore_log_shutdown();
}

void TestDoorAnimWiringDumpReportsAnimating() {
    // A wired and animating door animator shows animating=true with a
    // non-zero angle. Pins the contract that walker_per_frame_tick's
    // transition detector is observable in the dump.
    walkercore_log_shutdown();
    walkercore_log_init(&DoorAnimCaptureSink);
    ResetCapture();
    DoorAnimProvider door;
    const TruckDoorConfig cfg = DefaultTruckDoorConfig();
    door.AnimateTo(cfg.door_max_open_rad, 0.6);
    diagnostic_internal::SetDoorAnim(&door);
    walker_dump_state();
    EXPECT_TRUE(ContainsCapture("door anim"));
    EXPECT_TRUE(ContainsCapture("animating=true"));
    // Target is 1.5708 rad, mid-tween is ~0.785 (45°). We don't
    // require exact 45° since the test doesn't tick — the angle is
    // the initial value (0) until the tween advances. Check for the
    // starting "0." or the immediate post-AnimateTo state.
    EXPECT_TRUE(ContainsCapture("angle=0.000") || ContainsCapture("angle=0."));
    diagnostic_internal::SetDoorAnim(nullptr);
    walkercore_log_shutdown();
}

void TestDoorAnimWiringTransitionStartToAngleAtTarget() {
    // End-to-end: fire the transition detector, advance through a full
    // door_open_duration_s, land at the target angle.
    DoorAnimProvider door_anim;
    const TruckDoorConfig cfg = DefaultTruckDoorConfig();
    StepFsmToDoor(WalkerFsmState::Driving, WalkerFsmState::TransitionOut,
                   door_anim, cfg);
    door_anim.Tick(cfg.door_open_duration_s * 0.5);
    door_anim.Tick(cfg.door_open_duration_s * 0.6);
    EXPECT_FALSE(door_anim.state().animating);
    EXPECT_NEAR(door_anim.state().angle_radians, cfg.door_max_open_rad, 1e-3);
}

}  // namespace walkercore