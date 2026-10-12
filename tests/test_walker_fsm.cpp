// WalkerCore — WalkerFsm tests.

#include "TestHarness.h"
#include "walkercore/WalkerFsm.h"

#include <math.h>

namespace walkercore {

namespace {

TruckState MakeDrivingTruck() {
    TruckState t;
    t.has_placement = true;
    t.placement.position = Vec3{0, 0, 0};
    t.placement.orientation.heading = 0.0f;  // facing +Z
    t.placement.orientation.pitch = 0.0f;
    t.placement.orientation.roll = 0.0f;
    return t;
}

PlayerState MakeWalkingPlayer() {
    PlayerState p;
    PlayerState_Reset(p);
    p.feet_position = Vec3{-1.4, 0, -1.2};  // outside driver's door anchor
    p.yaw_radians = 1.5708f;
    p.has_feet = true;
    p.mode = PlayerMode::Idle;
    return p;
}

}  // namespace

void TestWalkerFsmInitialState() {
    WalkerFsm fsm;
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Driving));
}

void TestWalkerFsmDrivingToWalking() {
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.2);

    TruckState truck = MakeDrivingTruck();
    PlayerState player = MakeWalkingPlayer();
    WalkerFsmInputs inputs;
    inputs.truck = &truck;
    inputs.player = &player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;  // F10

    CameraState target;
    fsm.Update(inputs, 0.016, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::TransitionOut));

    // Advance to completion.
    fsm.Update(inputs, 0.2, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));
}

void TestWalkerFsmWalkingToDriving() {
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.2);

    TruckState truck = MakeDrivingTruck();
    PlayerState player = MakeWalkingPlayer();
    WalkerFsmInputs inputs;
    inputs.truck = &truck;
    inputs.player = &player;
    inputs.interact_pressed = true;     // F to enter
    inputs.toggle_walk_pressed = false;

    // First, get into Walking.
    inputs.toggle_walk_pressed = true;
    CameraState target;
    fsm.Update(inputs, 0.016, &target);
    fsm.Update(inputs, 0.2, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));

    // Now try to enter via F while in range.
    inputs.interact_pressed = true;
    inputs.toggle_walk_pressed = false;
    fsm.Update(inputs, 0.016, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::TransitionIn));

    // Advance to completion.
    fsm.Update(inputs, 0.2, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Driving));
}

void TestWalkerFsmF10FromWalkingExits() {
    // NEW behaviour introduced with the single-authority refactor: F10
    // (toggle walk) from Walking state starts a TransitionIn tween to the
    // seat WITHOUT requiring the player to be near a door. This is the
    // "I want to drive now, no matter where I am" key.
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.2);

    // Get into Walking first.
    TruckState truck = MakeDrivingTruck();
    PlayerState player = MakeWalkingPlayer();
    WalkerFsmInputs inputs;
    inputs.truck = &truck;
    inputs.player = &player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;
    CameraState target;
    fsm.Update(inputs, 0.016, &target);
    fsm.Update(inputs, 0.2, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));

    // Now wander far away from the door (60 m off) and press F10.
    player.feet_position = Vec3{60.0, 0.0, 0.0};
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;
    fsm.Update(inputs, 0.016, &target);
    // Should start TransitionIn despite being far from the door.
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::TransitionIn));

    // And complete it.
    fsm.Update(inputs, 0.2, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Driving));
}

void TestWalkerFsmBindPlayerTarget() {
    // When player_target_ is bound, the FSM should snap the player to
    // the destination anchor at the moment a transition starts (so the
    // FrameLoop integrates from the correct position).
    WalkerFsm fsm;
    PlayerState bound;
    PlayerState_Reset(bound);
    bound.feet_position = Vec3{99.0, 99.0, 99.0};  // junk starting position
    fsm.BindPlayerTarget(&bound);

    fsm.SetTransitionSeconds(0.2);

    // Driving + F10 → TransitionOut → player should snap to the door
    // anchor (default config: local_enter_anchor = {-1.40, 0, -1.20}).
    WalkerFsmInputs inputs;
    TruckState truck = MakeDrivingTruck();  // truck at origin, heading 0
    inputs.truck = &truck;
    inputs.player = &bound;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;
    CameraState target;
    fsm.Update(inputs, 0.016, &target);
    // Truck at (0,0,0), heading=0, local_enter_anchor = (-1.40, 0, -1.20)
    // → world position (-1.40, 0, -1.20). The player's feet should now
    // be there.
    EXPECT_NEAR(bound.feet_position.x, -1.40, 1e-6);
    EXPECT_NEAR(bound.feet_position.z, -1.20, 1e-6);
}

void TestWalkerFsmNoPlayerTargetIsSafe() {
    // When player_target_ is NOT bound, the FSM should still advance
    // states correctly — just without writing into any PlayerState.
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.05);

    TruckState truck = MakeDrivingTruck();
    PlayerState player = MakeWalkingPlayer();
    WalkerFsmInputs inputs;
    inputs.truck = &truck;
    inputs.player = &player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;
    CameraState target;
    fsm.Update(inputs, 0.016, &target);
    fsm.Update(inputs, 0.1, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));
}

// -----------------------------------------------------------------------------
// Exhaustiveness sweep — pin every (state, ShouldRenderOverride) row.
// -----------------------------------------------------------------------------
//
// The D3D11 hook reads ShouldRenderOverride() each frame to decide whether
// to push the walker's view/projection matrices or call the game's
// original Present. The contract is:
//
//   state                 ShouldRenderOverride
//   Driving                    false  (game camera owns the view)
//   TransitionIn               false  (visual handover mid-tween — game
//                                      camera is on its way up to full
//                                      strength, walker is fading out)
//   TransitionOut              true   (walker is fading in)
//   Walking                    true
//
// If a future maintainer changes this contract (e.g. TransitionIn should
// also be walker-owned) the test below catches the regression because the
// four EXPECT_TRUE / EXPECT_FALSE rows pin the table.

void TestWalkerFsmShouldRenderOverridePerState() {
    // Walk through the full state diagram one frame at a time and pin
    // ShouldRenderOverride after every Update() call.
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.4);  // 400 ms transitions — long enough
                                     // to land in the middle and assert.

    TruckState truck = MakeDrivingTruck();
    PlayerState player = MakeWalkingPlayer();
    WalkerFsmInputs inputs;
    inputs.truck = &truck;
    inputs.player = &player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = false;
    CameraState target;

    // Row 1: fresh FSM is Driving → false.
    EXPECT_FALSE(fsm.ShouldRenderOverride());

    // Row 2: drive F10 → TransitionOut → true.
    inputs.toggle_walk_pressed = true;
    fsm.Update(inputs, 0.016, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::TransitionOut));
    EXPECT_TRUE(fsm.ShouldRenderOverride());

    // Row 3: advance ~50 % (0.2 s of 0.4 s) → still TransitionOut → true.
    fsm.Update(inputs, 0.2, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::TransitionOut));
    EXPECT_TRUE(fsm.ShouldRenderOverride());

    // Row 4: complete the transition → Walking → true.
    fsm.Update(inputs, 0.2, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));
    EXPECT_TRUE(fsm.ShouldRenderOverride());

    // Row 5: press F (interact) while in range → TransitionIn starts → false.
    inputs.interact_pressed = true;
    inputs.toggle_walk_pressed = false;
    fsm.Update(inputs, 0.016, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::TransitionIn));
    EXPECT_FALSE(fsm.ShouldRenderOverride());

    // Row 6: advance ~50 % → still TransitionIn → false.
    fsm.Update(inputs, 0.2, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::TransitionIn));
    EXPECT_FALSE(fsm.ShouldRenderOverride());

    // Row 7: complete → Driving → false.
    fsm.Update(inputs, 0.2, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Driving));
    EXPECT_FALSE(fsm.ShouldRenderOverride());
}

// transitionProgress is read by the render layer to scale fade-in /
// fade-out alpha (the same way the engine's own camera blends use it).
// It must:
//   - be 0 outside any transition (Driving, Walking).
//   - be 0 at the moment a transition starts (t=0).
//   - advance monotonically 0..1 across the transition window.
//   - clamp to 1 once the transition completes (not overshoot).

void TestWalkerFsmTransitionProgressMonotonic() {
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.2);

    TruckState truck = MakeDrivingTruck();
    PlayerState player = MakeWalkingPlayer();
    WalkerFsmInputs inputs;
    inputs.truck = &truck;
    inputs.player = &player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = false;
    CameraState target;

    // Driving: progress is 0.
    EXPECT_NEAR(fsm.transitionProgress(), 0.0f, 1e-6f);

    // F10 → TransitionOut: progress is 0 at first frame.
    inputs.toggle_walk_pressed = true;
    fsm.Update(inputs, 0.001, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::TransitionOut));
    EXPECT_NEAR(fsm.transitionProgress(), 0.0f, 1e-3f);

    // Advance ~25 % of the 0.2 s window.
    fsm.Update(inputs, 0.05, &target);
    const float p1 = fsm.transitionProgress();
    EXPECT_TRUE(p1 > 0.0f);
    EXPECT_TRUE(p1 < 1.0f);

    // Advance another ~50 % of the window.
    fsm.Update(inputs, 0.10, &target);
    const float p2 = fsm.transitionProgress();
    EXPECT_TRUE(p2 > p1);  // monotonic

    // Complete the transition with a small over-shoot dt. progress
    // must clamp to exactly 1.0 (the FSM snaps t to 1.0 when the
    // accumulated time exceeds the duration).
    fsm.Update(inputs, 0.5, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));
    EXPECT_NEAR(fsm.transitionProgress(), 1.0f, 1e-6f);
}

// The "Interact" key (F by default) while in Walking is the door-entry
// input. If the player is far from the door and not facing the truck,
// pressing F must NOT start a TransitionIn. This is the "I wandered off,
// I don't want to drive yet" guard.

void TestWalkerFsmInteractOutOfRangeIsSilent() {
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.05);

    TruckState truck = MakeDrivingTruck();
    PlayerState player = MakeWalkingPlayer();  // (-1.4, 0, -1.2), facing truck
    WalkerFsmInputs inputs;
    inputs.truck = &truck;
    inputs.player = &player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = false;
    CameraState target;

    // Get into Walking via F10.
    inputs.toggle_walk_pressed = true;
    fsm.Update(inputs, 0.016, &target);
    fsm.Update(inputs, 0.05, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));

    // Now wander 60 m off and press F. Must stay in Walking.
    player.feet_position = Vec3{60.0, 0.0, 0.0};
    inputs.toggle_walk_pressed = false;
    inputs.interact_pressed = true;
    fsm.Update(inputs, 0.016, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));
}

// -----------------------------------------------------------------------------
// WalkerFsmStateName probe — same pattern as the FeatureName probe in
// test_version_table.cpp. Adding a new WalkerFsmState value MUST add
// a case here (caught by -Werror -Wswitch-enum) and a row in the probe
// table below (caught by the test failure).
// -----------------------------------------------------------------------------

void TestWalkerFsmStateNameReturnsNameForEveryValue() {
    struct Case { WalkerFsmState s; const char *expected_name; };
    const Case kCases[] = {
        {WalkerFsmState::Driving,       "Driving"},
        {WalkerFsmState::Walking,       "Walking"},
        {WalkerFsmState::TransitionIn,  "TransitionIn"},
        {WalkerFsmState::TransitionOut, "TransitionOut"},
    };
    const size_t kCount = sizeof(kCases) / sizeof(kCases[0]);
    for (size_t i = 0; i < kCount; ++i) {
        const char *name = WalkerFsmStateName(kCases[i].s);
        EXPECT_TRUE(name != nullptr);
        EXPECT_TRUE(name[0] != '\0');
        // Unknown-fallback sentinel must never appear for a real state
        // value — that would mean the switch in WalkerFsm.cpp has a
        // missing case.
        EXPECT_TRUE(strcmp(name, "(unknown)") != 0);
        EXPECT_STREQ(name, kCases[i].expected_name);
    }
}

// Pin the transition-direction contract: TransitionOut goes FROM
// Driving TO Walking (start = Driving end = Walking). TransitionIn is
// the reverse. The two must not share a name — distinct dump rows
// would be useless without that. Names are stable so we can grep
// them in the game log.

void TestWalkerFsmStateNamesAreDistinct() {
    // All four states must have distinct strings; the diagnostic dump
    // produces one row per state and an analyst must be able to tell
    // which is which by name alone.
    const char *names[4];
    names[0] = WalkerFsmStateName(WalkerFsmState::Driving);
    names[1] = WalkerFsmStateName(WalkerFsmState::Walking);
    names[2] = WalkerFsmStateName(WalkerFsmState::TransitionIn);
    names[3] = WalkerFsmStateName(WalkerFsmState::TransitionOut);
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            EXPECT_TRUE(strcmp(names[i], names[j]) != 0);
        }
    }
}

// -----------------------------------------------------------------------------
// WalkerFsmInputs field-exhaustive probe.
//
// WalkerFsmInputs has 4 fields:
//   - TruckState truck
//   - PlayerState player
//   - bool interact_pressed
//   - bool toggle_walk_pressed
//
// Each MUST have an observable effect on FSM behaviour. If a future
// refactor drops a read of one of these fields, the probe for that
// field fails. The probe is intentionally orthogonal — each case uses
// a fresh FSM and isolates one field so the failure message names the
// field that lost its contract.
//
// Cases:
//   1. truck.has_placement == false → PlacePlayerAtDoor must early-out;
//      the FSM should NOT crash, NOT write into the bound player.
//   2. player.feet_position set far from the door while in Walking →
//      F press is silent (PlayerCanEnterDoor rejects on distance).
//   3. interact_pressed == true while Driving → must start TransitionOut.
//   4. toggle_walk_pressed == true while Driving → must start TransitionOut.
//
// The four cases share a single TestXxx so the failure surface points
// at the field name, not at a wall of independent test functions.
// -----------------------------------------------------------------------------

void TestWalkerFsmInputsEveryFieldHasObservableEffect() {
    // ---- Case 1: truck.has_placement == false gates PlacePlayerAtDoor ----
    {
        WalkerFsm fsm;
        PlayerState bound;
        PlayerState_Reset(bound);
        // Place bound.feet_position somewhere distinct so we can tell if
        // the FSM writes to it.
        bound.feet_position = Vec3{42.0, 7.0, 13.0};
        bound.has_feet = true;
        fsm.BindPlayerTarget(&bound);
        fsm.SetTransitionSeconds(0.05);

        WalkerFsmInputs inputs;
        TruckState t;
        // truck.has_placement stays false on purpose.
        inputs.truck = &t;
        inputs.player = &bound;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = true;  // F10 — should still trigger
                                            // TransitionOut, even without
                                            // a valid truck placement.
        CameraState target;
        fsm.Update(inputs, 0.016, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::TransitionOut));
        // PlacePlayerAtDoor returns early because !truck.has_placement,
        // so the bound player is unchanged — feet_position stays at the
        // sentinel (42, 7, 13) and has_feet stays true (its prior value).
        EXPECT_NEAR(bound.feet_position.x, 42.0, 1e-9);
        EXPECT_NEAR(bound.feet_position.y, 7.0, 1e-9);
        EXPECT_NEAR(bound.feet_position.z, 13.0, 1e-9);
    }

    // ---- Case 2: player.feet_position far from the door makes F silent ----
    {
        WalkerFsm fsm;
        fsm.SetTransitionSeconds(0.05);

        TruckState truck = MakeDrivingTruck();  // truck at origin, heading 0
        PlayerState player = MakeWalkingPlayer();
        WalkerFsmInputs inputs;
        inputs.truck = &truck;
        inputs.player = &player;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = false;
        CameraState target;

        // Drive F10 → TransitionOut → Walking.
        inputs.toggle_walk_pressed = true;
        fsm.Update(inputs, 0.016, &target);
        fsm.Update(inputs, 0.05, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::Walking));

        // Wander 60 m east of the truck (well outside the 1.5 m door
        // activation distance) and press F. The FSM must consult
        // player.feet_position — it's the only field that influences
        // PlayerCanEnterDoor — and reject the press.
        player.feet_position = Vec3{60.0, 0.0, 0.0};
        inputs.interact_pressed = true;
        inputs.toggle_walk_pressed = false;
        fsm.Update(inputs, 0.016, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::Walking));
    }

    // ---- Case 3: interact_pressed alone (no F10) starts TransitionOut ----
    {
        WalkerFsm fsm;
        fsm.SetTransitionSeconds(0.05);

        TruckState truck = MakeDrivingTruck();
        PlayerState player = MakeWalkingPlayer();
        WalkerFsmInputs inputs;
        inputs.truck = &truck;
        inputs.player = &player;
        inputs.interact_pressed = true;      // F only
        inputs.toggle_walk_pressed = false;
        CameraState target;
        fsm.Update(inputs, 0.016, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::TransitionOut));
    }

    // ---- Case 4: toggle_walk_pressed alone (no F) starts TransitionOut ----
    {
        WalkerFsm fsm;
        fsm.SetTransitionSeconds(0.05);

        TruckState truck = MakeDrivingTruck();
        PlayerState player = MakeWalkingPlayer();
        WalkerFsmInputs inputs;
        inputs.truck = &truck;
        inputs.player = &player;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = true;   // F10 only
        CameraState target;
        fsm.Update(inputs, 0.016, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::TransitionOut));
    }

    // ---- Negative control: with both key flags false, the FSM does not
    //      transition from Driving, regardless of the other inputs. This
    //      pins that neither interact_pressed NOR toggle_walk_pressed
    //      became "default-on" — both must be set by the input pipeline
    //      to have an effect. ----
    {
        WalkerFsm fsm;
        fsm.SetTransitionSeconds(0.05);

        TruckState truck = MakeDrivingTruck();
        PlayerState player = MakeWalkingPlayer();
        WalkerFsmInputs inputs;
        inputs.truck = &truck;
        inputs.player = &player;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = false;
        CameraState target;
        fsm.Update(inputs, 1.0, &target);  // 1 s of "nothing"
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::Driving));
    }
}

// -----------------------------------------------------------------------------
// WalkerFsm::ComputeTransitionTarget math probe.
//
// ComputeTransitionTarget lerps four scalars (feet, yaw, pitch, eye_height)
// between a "from" snapshot (captured at transition start) and a "to"
// value (computed from the current truck placement + door config).
// out_camera_target is written with the interpolated result.
//
// Math contracts pinned:
//
//   feet         = LerpVec3(from_feet, to_feet, t)        -- linear interp
//   yaw          = LerpAngle(from_yaw,  to_yaw,  t)       -- shortest path
//   pitch        = from_pitch * (1 - t)                    -- linear to 0
//   eye_height   = lerp(from_eye_height, to_eye_height, t) -- linear
//   eye_position = feet + (0, eye_height, 0)              -- Y offset
//
// When !truck.has_placement, to_feet/to_yaw/to_eye_height stay at
// from_* (the camera doesn't teleport without a valid placement).
//
// TransitionOut:   to_feet = driver_door.local_enter_anchor world
//                  to_yaw  = truck_heading + door.enter_yaw_offset_rad
//                  to_eye_height = 1.65 (standing)
// TransitionIn:    to_feet = driver_seat_local world
//                  to_yaw  = truck_heading
//                  to_eye_height = 0.0 (seated; camera origin = feet)
//
// Driving the FSM into a transition and observing the interpolated
// out_camera_target is the only externally-visible way to pin the
// lerp math (LerpVec3 / LerpAngle are anonymous-namespace helpers).
//
// To observe t = exactly 0 we call Update(dt=0) once the state is
// TransitionOut/In — case TransitionOut/In only advances t by
// dt/duration, so dt=0 keeps t at exactly 0.
// -----------------------------------------------------------------------------

void TestWalkerFsmTransitionTargetMath() {
    // ---------- TransitionOut: Driving → door anchor ----------
    {
        WalkerFsm fsm;
        fsm.SetTransitionSeconds(1.0);

        WalkerFsmInputs inputs;
        TruckState truck = MakeDrivingTruck();  // truck at origin, heading 0
        inputs.truck = &truck;
        // Player at origin (NOT at the door anchor) so the feet lerp is
        // visible. Yaw=0, pitch=0.4, eye_height=1.65.
        PlayerState p;
        PlayerState_Reset(p);
        p.feet_position = Vec3{0.0, 0.0, 0.0};
        p.yaw_radians = 0.0f;
        p.pitch_radians = 0.4f;
        p.eye_height = 1.65f;
        p.has_feet = true;
        inputs.player = &p;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = false;
        CameraState target;

        // Default TruckDoorConfig: driver_door.local_enter_anchor
        // = (-1.40, 0, -1.20), enter_yaw_offset_rad = π/2.
        // So to_feet = (-1.40, 0, -1.20), to_yaw = π/2, to_eye_height = 1.65.

        // Step 1: drive FSM into TransitionOut. dt=0 is fine for the
        // Driving case — it doesn't advance t.
        inputs.toggle_walk_pressed = true;
        fsm.Update(inputs, 0.0, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::TransitionOut));

        // Step 2: t = 0 exactly (dt = 0 → no advance).
        fsm.Update(inputs, 0.0, &target);
        EXPECT_TRUE(target.valid);
        // feet = from_feet = (0, 0, 0); eye_position.y = feet.y + eye_height.
        EXPECT_NEAR(target.eye_position.x, 0.0, 1e-6);
        EXPECT_NEAR(target.eye_position.y, 1.65, 1e-6);
        EXPECT_NEAR(target.eye_position.z, 0.0, 1e-6);
        EXPECT_NEAR(target.yaw_radians, 0.0f, 1e-6);
        EXPECT_NEAR(target.pitch_radians, 0.4f, 1e-6);

        // Step 3: t = 0.5 (dt = 0.5 / 1.0).
        fsm.Update(inputs, 0.5, &target);
        EXPECT_TRUE(target.valid);
        // feet = midpoint of (0,0,0) and (-1.40, 0, -1.20)
        EXPECT_NEAR(target.eye_position.x, -0.70, 1e-3);
        EXPECT_NEAR(target.eye_position.z, -0.60, 1e-3);
        // eye_position.y = feet.y + eye_height = 0 + 1.65 = 1.65 (both ends = 1.65)
        EXPECT_NEAR(target.eye_position.y, 1.65, 1e-3);
        // yaw = midpoint of 0 and π/2
        EXPECT_NEAR(target.yaw_radians, 0.7854f, 1e-3);
        // pitch = 0.4 * (1 - 0.5) = 0.2
        EXPECT_NEAR(target.pitch_radians, 0.2f, 1e-4);

        // Step 4: t = 1.0 (dt = 0.5 more → t clamped to 1.0, state→
        // Walking; this call STILL runs ComputeTransitionTarget at t=1).
        fsm.Update(inputs, 0.5, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::Walking));
        EXPECT_TRUE(target.valid);
        EXPECT_NEAR(target.eye_position.x, -1.40, 1e-6);
        EXPECT_NEAR(target.eye_position.z, -1.20, 1e-6);
        EXPECT_NEAR(target.yaw_radians, 1.5708f, 1e-4);
        // pitch decayed to 0
        EXPECT_NEAR(target.pitch_radians, 0.0f, 1e-6);
    }

    // ---------- TransitionIn: Walking → seat (eye_height → 0) ----------
    {
        WalkerFsm fsm;
        fsm.SetTransitionSeconds(1.0);

        WalkerFsmInputs inputs;
        TruckState truck = MakeDrivingTruck();
        inputs.truck = &truck;
        // Player standing tall at origin, yaw=π/2.
        PlayerState p;
        PlayerState_Reset(p);
        p.feet_position = Vec3{0.0, 0.0, 0.0};
        p.yaw_radians = 1.5708f;  // π/2 — already facing the truck
        p.pitch_radians = 0.2f;
        p.eye_height = 1.65f;
        p.has_feet = true;
        inputs.player = &p;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = false;
        CameraState target;

        // Drive into Walking first (F10 from Driving).
        inputs.toggle_walk_pressed = true;
        fsm.Update(inputs, 0.0, &target);  // t=0 first
        fsm.Update(inputs, 1.0, &target);  // overshoot → Walking
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::Walking));

        // Now TransitionIn via F10 (bypasses door check).
        inputs.toggle_walk_pressed = true;
        inputs.interact_pressed = false;
        fsm.Update(inputs, 0.0, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(WalkerFsmState::TransitionIn));

        // Default driver_seat_local = (-0.45, 0.55, -0.50), truck at
        // origin heading 0. So to_feet = (-0.45, 0.55, -0.50),
        // to_yaw = 0, to_eye_height = 0.0 (seated).

        // t = 0: feet at from_feet (0, 0, 0), eye_height 1.65.
        fsm.Update(inputs, 0.0, &target);
        EXPECT_TRUE(target.valid);
        EXPECT_NEAR(target.eye_position.x, 0.0, 1e-6);
        EXPECT_NEAR(target.eye_position.y, 1.65, 1e-6);
        EXPECT_NEAR(target.eye_position.z, 0.0, 1e-6);
        EXPECT_NEAR(target.yaw_radians, 1.5708f, 1e-6);
        EXPECT_NEAR(target.pitch_radians, 0.2f, 1e-6);

        // t = 0.5: feet = midpoint of (0,0,0) and (-0.45, 0.55, -0.50),
        // eye_height = 0.825, pitch = 0.1.
        fsm.Update(inputs, 0.5, &target);
        EXPECT_TRUE(target.valid);
        EXPECT_NEAR(target.eye_position.x, -0.225, 1e-4);
        EXPECT_NEAR(target.eye_position.y, 0.275 + 0.825, 1e-4);  // mid.y + eye_height
        EXPECT_NEAR(target.eye_position.z, -0.25, 1e-4);
        // yaw = midpoint of π/2 and 0 (LerpAngle shortest path).
        EXPECT_NEAR(target.yaw_radians, 0.7854f, 1e-4);
        EXPECT_NEAR(target.pitch_radians, 0.1f, 1e-5);
    }

    // ---------- No truck placement: to_* = from_* (camera stays put) ----
    {
        WalkerFsm fsm;
        fsm.SetTransitionSeconds(1.0);

        WalkerFsmInputs inputs;
        TruckState t;  // has_placement stays false
        inputs.truck = &t;
        PlayerState p;
        PlayerState_Reset(p);
        p.feet_position = Vec3{42.0, 7.0, 13.0};  // arbitrary
        p.yaw_radians = 1.234f;
        p.pitch_radians = 0.3f;
        p.eye_height = 1.5f;
        p.has_feet = true;
        inputs.player = &p;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = false;
        CameraState target;

        // Drive into TransitionOut. With !truck.has_placement,
        // PlacePlayerExited is a no-op, so from_feet = (42, 7, 13).
        inputs.toggle_walk_pressed = true;
        fsm.Update(inputs, 0.0, &target);
        fsm.Update(inputs, 0.5, &target);  // t = 0.5
        // to_feet = from_feet (no placement), so lerp stays at from.
        EXPECT_NEAR(target.eye_position.x, 42.0, 1e-6);
        EXPECT_NEAR(target.eye_position.y, 7.0 + 1.5, 1e-6);  // feet.y + eye_height
        EXPECT_NEAR(target.eye_position.z, 13.0, 1e-6);
        EXPECT_NEAR(target.yaw_radians, 1.234f, 1e-6);
        // pitch decays linearly (NOT dependent on placement).
        EXPECT_NEAR(target.pitch_radians, 0.15f, 1e-6);  // 0.3 * 0.5

        // At t=1.0: same — still at from (no placement means no "to").
        fsm.Update(inputs, 0.5, &target);
        EXPECT_NEAR(target.eye_position.x, 42.0, 1e-6);
        EXPECT_NEAR(target.eye_position.y, 7.0 + 1.5, 1e-6);
        EXPECT_NEAR(target.eye_position.z, 13.0, 1e-6);
        EXPECT_NEAR(target.yaw_radians, 1.234f, 1e-6);
        EXPECT_NEAR(target.pitch_radians, 0.0f, 1e-6);  // pitch decayed to 0
    }
}

// -----------------------------------------------------------------------------
// Driving → TransitionOut camera-tween "from" capture contract.
//
// When the per-frame conductor drives the FSM, it sets
// g_fsm->BindPlayerTarget(&g_frame_loop->player()) at init, and the
// conductor passes &g_frame_loop->player() as the inputs.player pointer
// every frame. The two pointers alias the same memory.
//
// When a Driving → TransitionOut transition fires, the FSM captures
// transition_from_player_ = *player (cabin-seat snapshot), then runs
// PlacePlayerExited on *player_target_ (which is the SAME memory as
// *player in production), then on the next line OVERWRITES
// transition_from_player_ = *player. The overwrite reads the
// post-placement memory, so the "from" values used by
// ComputeTransitionTarget are the DOOR anchor — not the cabin seat.
//
// ComputeTransitionTarget's TransitionOut branch then sets to_feet =
// door anchor and to_yaw = door yaw, which means lerp(from=door, to=door)
// is a constant no-op. The cabin-seat-to-door tween never happens; the
// camera jumps directly to the door anchor on the transition-start frame.
//
// The fix is a one-line removal of the second overwrite. Pin the
// contract with a test that mirrors the conductor's pointer-aliasing
// pattern: BindPlayerTarget(&player) AND pass inputs.player = &player
// for the SAME object, drive a TransitionOut, and check that the
// TransitionOut camera target at t=0 reports the cabin-seat eye
// position (not the door anchor).
// -----------------------------------------------------------------------------

void TestWalkerFsmTransitionOutCameraFromMatchesCabinSeatWhenPointerAliases() {
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.2);

    // Truck at origin, heading 0 (default).
    TruckState truck = MakeDrivingTruck();

    // Player in the cabin. Use a custom cabin-seat-feet position so
    // we can tell the door anchor (PlacePlayerExited's destination)
    // apart from the cabin seat (the intended "from").
    PlayerState player;
    PlayerState_Reset(player);
    player.feet_position = Vec3{2.5, 0.55, -0.50};  // cabin seat
    player.yaw_radians = 0.0f;                       // facing forward
    player.pitch_radians = 0.1f;
    player.eye_height = 1.65f;
    player.has_feet = true;
    player.mode = PlayerMode::Idle;

    // Mirror the conductor's pattern: BindPlayerTarget to the SAME
    // memory we pass as inputs.player.
    fsm.BindPlayerTarget(&player);

    // Trigger the Driving → TransitionOut transition.
    WalkerFsmInputs inputs;
    inputs.truck = &truck;
    inputs.player = &player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;  // F10
    CameraState target;
    fsm.Update(inputs, 0.016, &target);

    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::TransitionOut));

    // The bound player was snapped to the door anchor by
    // PlacePlayerExited (default config: local_enter_anchor = (-1.40, 0,
    // -1.20) → world (-1.40, 0, -1.20)).
    EXPECT_NEAR(player.feet_position.x, -1.40, 1e-6);
    EXPECT_NEAR(player.feet_position.z, -1.20, 1e-6);

    // Now advance t to 0 exactly (dt = 0). ComputeTransitionTarget
    // uses transition_from_player_ as the "from" — which the bug
    // would have overwritten with the door anchor. The correct
    // behaviour is "from = cabin seat" so the camera tween animates
    // from the cabin view to the door anchor.
    fsm.Update(inputs, 0.0, &target);
    EXPECT_TRUE(target.valid);

    // The expected "from" eye_position = feet_cabin_seat + (0, eye_height, 0)
    // = (2.5, 0.55 + 1.65, -0.50) = (2.5, 2.20, -0.50).
    //
    // If the bug is active, transition_from_player_ was overwritten
    // with the door-anchor player state, so eye_position = feet_door +
    // eye_height = (-1.40, 0 + 1.65, -1.20) = (-1.40, 1.65, -1.20).
    EXPECT_NEAR(target.eye_position.x, 2.5, 1e-6);
    EXPECT_NEAR(target.eye_position.y, 2.20, 1e-6);
    EXPECT_NEAR(target.eye_position.z, -0.50, 1e-6);

    // Yaw at t=0 = from_yaw = 0.0 (cabin seat facing forward).
    // If the bug is active, from_yaw = door_yaw (heading + offset).
    EXPECT_NEAR(target.yaw_radians, 0.0f, 1e-6);
}

// WalkerFsmInputs pointer-refactor contract. The per-frame conductor uses
// UpdateFromPointers to avoid the per-frame copy of the truck (104 bytes)
// and player (48 bytes) into a struct-by-value WalkerFsmInputs. This
// test pins three things:
//
//   1. The size of WalkerFsmInputs is now small enough that the struct
//      is dominated by its 4 words (2 pointers + 2 byte bools +
//      padding), NOT by the embedded TruckState/PlayerState copies
//      (~160 bytes). Specifically sizeof(WalkerFsmInputs) should be
//      well under 64 bytes.
//
//   2. UpdateFromPointers produces the same state transitions as
//      Update(inputs, ...) when called with equivalent pointer data —
//      the only difference is the call boundary.
//
//   3. Mutations to the source player state (via the pointer) are
//      observable on the next Update call, whereas the old struct-
//      by-value path made mutations to inputs.player.feet_position
//      invisible to the FSM (they only modified a discarded copy).
//      This is the real correctness gain from the refactor.
void TestWalkerFsmInputsPointerRefactorContract() {
    // Contract 1: struct size. The previous by-value struct was
    // sizeof(TruckState) + sizeof(PlayerState) + 2 bools + padding =
    // 104 + 48 + 2 + 6 = 160 bytes. The pointer-based struct is
    // 2 pointers + 2 bools + 6 padding = 24 bytes (8-byte aligned on
    // x64). Assert the upper bound; if someone re-introduces copies
    // (or accidentally re-embeds the structs) the size jumps and
    // this probe fails.
    EXPECT_LE(sizeof(walkercore::WalkerFsmInputs), 32u);

    // Contract 2: UpdateFromPointers produces the same Driving →
    // TransitionOut state change as the struct-based Update when
    // called with the same truck/player/interact/toggle_walk data.
    {
        walkercore::WalkerFsm fsm;
        fsm.SetTransitionSeconds(0.2);
        walkercore::TruckState truck = MakeDrivingTruck();
        walkercore::PlayerState player = MakeWalkingPlayer();
        walkercore::CameraState target1;
        walkercore::CameraState target2;

        // Path A: UpdateFromPointers.
        fsm.UpdateFromPointers(&truck, &player, /*interact=*/false,
                               /*toggle_walk=*/true, 0.016, &target1);
        const walkercore::WalkerFsmState s1 = fsm.state();
        EXPECT_EQ(static_cast<unsigned>(s1),
                  static_cast<unsigned>(walkercore::WalkerFsmState::TransitionOut));

        // Path B: same data via the struct API. Both paths should
        // reach the same state with the same inputs.
        walkercore::WalkerFsm fsm2;
        fsm2.SetTransitionSeconds(0.2);
        walkercore::WalkerFsmInputs inputs;
        inputs.truck = &truck;
        inputs.player = &player;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = true;
        fsm2.Update(inputs, 0.016, &target2);
        const walkercore::WalkerFsmState s2 = fsm2.state();
        EXPECT_EQ(static_cast<unsigned>(s1),
                  static_cast<unsigned>(s2));
    }

    // Contract 3: mutations to the source player state are observable
    // on the next Update call. The struct-by-value path couldn't
    // express this (mutating inputs.player.feet_position only
    // touched the discarded copy). The pointer path does — the FSM
    // sees the new feet_position.
    {
        walkercore::WalkerFsm fsm;
        fsm.SetTransitionSeconds(0.05);
        walkercore::TruckState truck = MakeDrivingTruck();
        walkercore::PlayerState player = MakeWalkingPlayer();
        walkercore::CameraState target;

        // Drive F10 → TransitionOut (call 1) → Walking (call 2).
        // The first call starts the transition with t=0; the second
        // call advances t to 1.0 (dt=0.05 / seconds=0.05 = 1.0) and
        // flips state to Walking.
        fsm.UpdateFromPointers(&truck, &player, false, true, 0.016, &target);
        fsm.UpdateFromPointers(&truck, &player, false, false, 0.05, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(walkercore::WalkerFsmState::Walking));

        // Move the player 60 m east — well outside the door
        // activation radius. The next Update with interact_pressed
        // must NOT start a TransitionIn because PlayerCanEnterDoor
        // reads the new feet_position through the pointer.
        player.feet_position = walkercore::Vec3{60.0, 0.0, 0.0};
        fsm.UpdateFromPointers(&truck, &player, /*interact=*/true,
                               /*toggle_walk=*/false, 0.016, &target);
        EXPECT_EQ(static_cast<unsigned>(fsm.state()),
                  static_cast<unsigned>(walkercore::WalkerFsmState::Walking));
    }
}

}  // namespace walkercore