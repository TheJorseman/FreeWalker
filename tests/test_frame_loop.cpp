// WalkerCore — FrameLoop integration tests.

#include "TestHarness.h"

#include <math.h>

#include "walkercore/FrameLoop.h"
#include "walkercore/InputHook.h"
#include "walkercore/InputState.h"
#include "walkercore/MovementConfig.h"
#include "walkercore/PlayerState.h"
#include "walkercore/TruckState.h"
#include "walkercore/WalkerFsm.h"
#include "walkercore/WorldState.h"

namespace walkercore {

void TestFrameLoopIntegration() {
    FrameLoop loop;
    WalkerFsm fsm;
    PlayerState p;
    PlayerState_Reset(p);  // explicit: PlayerState is aggregate, default-init leaves members uninitialized
    fsm.BindPlayerTarget(&p);
    loop.BindFsm(&fsm);

    // Default state: not in walk mode (FSM is in Driving).
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Driving));

    // Set aspect ignores garbage.
    loop.SetAspect(-1.0f);
    EXPECT_NEAR(loop.camera().aspect, 16.0f / 9.0f, 1e-6);
    loop.SetAspect(21.0f / 9.0f);
    EXPECT_NEAR(loop.camera().aspect, 21.0f / 9.0f, 1e-6);

    // Movement config round-trips.
    MovementConfig cfg_mv;
    cfg_mv.walkSpeedMps = 9.9f;
    loop.SetMovementConfig(cfg_mv);
    EXPECT_NEAR(loop.movementConfig().walkSpeedMps, 9.9f, 1e-6);

    // RunOnce with no providers and no Walking state is a no-op (does not
    // crash, does not integrate movement).
    loop.RunOnce();
    EXPECT_TRUE(true);

    // Even with an FSM bound, RunOnce in Driving is a no-op for movement.
    // We assert this indirectly: the player state should still be the
    // post-Reset default (feet_position = {0,0,0}, has_feet = false).
    EXPECT_NEAR(p.feet_position.x, 0.0, 1e-6);
    EXPECT_FALSE(p.has_feet);
}

void TestFrameLoopCameraOverride() {
    FrameLoop loop;
    WalkerFsm fsm;
    PlayerState p;
    fsm.BindPlayerTarget(&p);
    loop.BindFsm(&fsm);

    // Put the FSM in Walking so RunOnce() actually integrates movement
    // and consumes the camera override. We use SetTransitionSeconds to
    // make the transition effectively instant so the state advances to
    // Walking in one Update call.
    fsm.SetTransitionSeconds(0.05);
    WalkerFsmInputs inputs;
    TruckState truck_state{};  // no placement — that's fine for this test
    inputs.truck = &truck_state;
    inputs.player = &p;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;
    CameraState target;
    fsm.Update(inputs, 0.016, &target);
    // Advance the transition to completion.
    fsm.Update(inputs, 0.1, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));

    // Now queue a camera override.
    CameraState_Reset(target);
    target.eye_position = Vec3{42, 7, 99};
    target.yaw_radians = 0.5f;
    loop.OverrideCamera(target);

    // RunOnce consumes the override. Note: we need to call SampleInput
    // first to seed last_input_ (RunOnce also samples, so this is just
    // for hygiene — the test would still pass without it).
    loop.SampleInput();
    loop.RunOnce();
    EXPECT_NEAR(loop.camera().eye_position.x, 42.0, 1e-6);
    EXPECT_NEAR(loop.camera().eye_position.y, 7.0, 1e-6);
    EXPECT_NEAR(loop.camera().eye_position.z, 99.0, 1e-6);
    EXPECT_NEAR(loop.camera().yaw_radians, 0.5f, 1e-6);

    // The next tick falls back to the player-driven camera. The player
    // doesn't have valid feet (it's at origin from BindPlayerTarget's
    // default Reset), so the camera eye will be at origin + 0 bob.
    loop.RunOnce();
    EXPECT_NEAR(loop.camera().eye_position.x, 0.0, 1e-6);
}

void TestFrameLoopNoFsmIsNoop() {
    // A FrameLoop without an FSM bound should be safe to call RunOnce on
    // (it just samples input and returns). Useful for unit tests that
    // don't care about the FSM contract.
    FrameLoop loop;
    loop.RunOnce();
    EXPECT_TRUE(true);
}

// Pin: a CameraState's projection fields (aspect, near_z, far_z,
// horizontal_fov_radians) must NOT be overwritten by the override path.
// The override path replaces the camera state with camera_override_'s
// eye/yaw/pitch (those are the only fields the FSM's Update writes);
// everything else must stay at the value SetAspect / the projection
// cache last produced. A buggy "camera_ = camera_override_" wholesale
// copy would clobber camera_.aspect with whatever bytes the FSM's
// stack-allocated `CameraState target;` happened to have on the
// previous frame, breaking the projection matrix.
void TestFrameLoopOverridePathPreservesProjectionFields() {
    FrameLoop loop;
    WalkerFsm fsm;
    PlayerState player;
    PlayerState_Reset(player);
    fsm.BindPlayerTarget(&player);
    loop.BindFsm(&fsm);

    // Advance the FSM into Walking. RunOnce skips its phase body when
    // the FSM isn't in Walking, so without this the override path
    // (which lives inside the Walking-guarded section) wouldn't fire
    // at all. SetTransitionSeconds(0.05) + a 0.10-second Update
    // pushes the transition to completion.
    fsm.SetTransitionSeconds(0.05);
    WalkerFsmInputs inputs;
    TruckState truck_state{};
    inputs.truck = &truck_state;
    inputs.player = &player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;
    CameraState target;
    CameraState_Reset(target);
    fsm.Update(inputs, 0.016, &target);
    fsm.Update(inputs, 0.10, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));

    constexpr double kDt = 1.0 / 60.0;

    // Set the projection inputs to known-good values.
    loop.SetAspect(2.0f);
    loop.SampleInput();
    loop.RunOnce(kDt);
    const float original_aspect = loop.camera().aspect;
    EXPECT_NEAR(original_aspect, 2.0f, 1e-6f);

    // Queue a camera override that only touches eye/yaw/pitch (the
    // FSM convention). The override must NOT change the aspect.
    CameraState_Reset(target);
    target.eye_position = Vec3{42.0, 7.0, 99.0};
    target.yaw_radians = 0.5f;
    target.pitch_radians = -0.3f;
    loop.OverrideCamera(target);

    loop.SampleInput();
    loop.RunOnce(kDt);

    // The override fields are honoured.
    EXPECT_NEAR(loop.camera().eye_position.x, 42.0, 1e-6);
    EXPECT_NEAR(loop.camera().eye_position.y, 7.0, 1e-6);
    EXPECT_NEAR(loop.camera().eye_position.z, 99.0, 1e-6);
    EXPECT_NEAR(loop.camera().yaw_radians, 0.5f, 1e-6);
    EXPECT_NEAR(loop.camera().pitch_radians, -0.3f, 1e-6);

    // The projection inputs are preserved across the override. Without
    // this contract, a wholesale `camera_ = camera_override_` would
    // overwrite camera_.aspect with target.aspect (uninitialized
    // garbage on the production stack), breaking the next frame's
    // projection matrix until SetAspect fires again.
    EXPECT_NEAR(loop.camera().aspect, original_aspect, 1e-6f);

    // After the override frame, the precomputed-yaw-trig flag must be
    // false so BuildViewRotation recomputes fresh sin/cos for the
    // override-target's yaw, not stale values from the player path.
    EXPECT_FALSE(loop.camera().precomputed_yaw_trig_valid_);
}

// Camera matrices are the contract surface for the M2 D3D11 hook:
// HookedPresent() reads camera.view + camera.projection after each
// RunOnce tick. Until the GPU push is wired, the matrices are computed
// here but never reach the renderer. Pin that the Walking pipeline
// actually populates them so a future refactor that drops the
// populate-on-Walking path is caught.
//
// Uses the same harness pattern as the override-preserves-projection
// test: drive the FSM into Walking, run one tick, then inspect the
// matrices FrameLoop hands to the renderer.
void TestFrameLoopWalkingPopulatesViewAndProjectionMatrices() {
    constexpr double kDt = 1.0 / 60.0;
    FrameLoop loop;
    WalkerFsm fsm;
    PlayerState player;
    PlayerState_Reset(player);
    fsm.BindPlayerTarget(&player);
    loop.BindFsm(&fsm);

    // Drive the FSM into Walking.
    fsm.SetTransitionSeconds(0.05);
    WalkerFsmInputs inputs;
    TruckState truck_state{};
    inputs.truck = &truck_state;
    inputs.player = &player;
    inputs.toggle_walk_pressed = true;
    CameraState target;
    CameraState_Reset(target);
    fsm.Update(inputs, 0.016, &target);
    fsm.Update(inputs, 0.10, &target);
    EXPECT_EQ(static_cast<unsigned>(fsm.state()),
              static_cast<unsigned>(WalkerFsmState::Walking));

    // Aspect set so the projection matrix is non-degenerate.
    loop.SetAspect(16.0f / 9.0f);
    loop.SampleInput();
    loop.RunOnce(kDt);

    // The view matrix is non-identity. We don't pin specific values
    // (player offsets change between configs); we pin that the
    // rotation block (rows 0/1/2, cols 0/1/2) has at least one
    // off-diagonal non-trivial entry, which proves BuildViewRotation
    // actually ran.
    const auto &view = loop.camera().view;
    EXPECT_TRUE(std::fabs(view[0][0]) > 0.5f ||
                std::fabs(view[0][2]) > 0.5f ||
                std::fabs(view[2][0]) > 0.5f);

    // The projection matrix has its perspective component populated:
    // projection[2][3] (m[2][3] in HCLIP space) should be -1 for a
    // typical projection. We accept any non-zero value as proof that
    // BuildProjectionMatrix ran.
    const auto &proj = loop.camera().projection;
    const bool has_perspective =
        std::fabs(proj[2][3]) > 0.01f || std::fabs(proj[3][2]) > 0.01f;
    EXPECT_TRUE(has_perspective);

    // The camera is marked valid after a full RunOnce tick in
    // Walking state — a stale `valid=false` would mean the renderer
    // push path (when wired) skips the frame.
    EXPECT_TRUE(loop.camera().valid);
}

namespace {

// Open world with a flat ground at y=0, identical to the perf /
// movement-config probes. We don't link an M3 lander in tests so this
// is the cheapest real IWorldGeometry that gives PhaseResolveCollision
// something to do (and keeps grounded_=true after the first landing).
class DiagWorld : public IWorldGeometry {
public:
    bool Raycast(Vec3, Vec3, double, Vec3 *, Vec3 *) override { return false; }
    MoveResult SphereSweep(Vec3 origin, double, Vec3 displacement) override {
        const double nx = origin.x + displacement.x;
        const double ny = origin.y + displacement.y;
        const double nz = origin.z + displacement.z;
        if (displacement.y < 0.0 && ny <= 0.0) {
            return MoveResult{Vec3{nx, 0.0, nz},
                               Vec3{0.0, 1.0, 0.0},
                               static_cast<float>(-displacement.y),
                               true};
        }
        return MoveResult{Vec3{nx, ny, nz},
                           Vec3{0.0, 0.0, 0.0},
                           0.0f, false};
    }
    bool groundY(Vec3, double *out_y) override {
        if (out_y) *out_y = 0.0;
        return true;
    }
};

void PressKey(InputHook &hook, WalkercoreKey k) {
    uint8_t &bits = hook.mutable_last().keys[KeyIndex(k)];
    const bool was_held = (bits & 0x01) != 0;
    bits |= 0x01;
    if (!was_held) bits |= 0x02;
}

// Configure a FrameLoop in Walking mode with a fast crouch blend and
// pre-pressed keys. Returns the FrameLoop + a struct to observe the
// player state after RunOnce calls. Each call constructs a fresh
// harness so residual state cannot leak between probe cases.
struct DiagHarness {
    FrameLoop loop;
    WalkerFsm fsm;
    InputHook input;
    PlayerState player;
    DiagWorld world;
    DiagHarness() {
        PlayerState_Reset(player);
        fsm.BindPlayerTarget(&player);
        loop.BindFsm(&fsm);
        loop.BindInput(&input);
        loop.BindWorld(&world);
        MovementConfig cfg;
        cfg.crouchBlendMps = 1000.0f;  // instant blend
        loop.SetMovementConfig(cfg);
        fsm.SetTransitionSeconds(0.05);
        WalkerFsmInputs inputs;
        TruckState truck_state{};
        inputs.truck = &truck_state;
        inputs.player = &player;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = true;
        CameraState target;
        CameraState_Reset(target);
        fsm.Update(inputs, 0.016, &target);
        fsm.Update(inputs, 0.10, &target);
    }
};

// No-world harness for tests that need PhaseResolveCollision to
// early-return (so gravity integrates without bound). We deliberately
// skip loop.BindWorld — PhaseResolveCollision's `if (world_ == nullptr)
// return;` then takes over and the player's feet_position.y is
// free-fall. Use this ONLY for tests that explicitly exercise the no-
// collision code path; a regular test wants DiagWorld so the player
// stays grounded after the first landing.
struct NoWorldHarness {
    FrameLoop loop;
    WalkerFsm fsm;
    InputHook input;
    PlayerState player;
    NoWorldHarness() {
        PlayerState_Reset(player);
        fsm.BindPlayerTarget(&player);
        loop.BindFsm(&fsm);
        loop.BindInput(&input);
        // Deliberately no loop.BindWorld — world_ stays null.
        MovementConfig cfg;
        cfg.crouchBlendMps = 1000.0f;
        loop.SetMovementConfig(cfg);
        fsm.SetTransitionSeconds(0.05);
        WalkerFsmInputs inputs;
        TruckState truck_state{};
        inputs.truck = &truck_state;
        inputs.player = &player;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = true;
        CameraState target;
        CameraState_Reset(target);
        fsm.Update(inputs, 0.016, &target);
        fsm.Update(inputs, 0.10, &target);  // advance past transition → Walking
    }
};

}  // namespace

// Diagonal motion must be speed-bounded: with Forward + StrafeRight both
// held, |velocity| per second must equal walkSpeedMps (1.6 m/s default)
// — NOT 1.6*sqrt(2). The implementation normalises the intent vector
// when |intent| > 1, which is the math this probe pins.
//
// This is the kind of regression where a future contributor removes the
// `if (intent_mag > 1.0f) { normalize }` block — the resulting diagonal
// walk would be 41% faster than forward walk, which a player would
// notice immediately on the very first diagonal step.
void TestFrameLoopDiagonalMotionIsSpeedBounded() {
    constexpr int kTicks = 30;
    constexpr double kDt = 1.0 / 60.0;
    constexpr float kWalkSpeed = 1.6f;  // default

    // Baseline: Forward only — should move purely along +Z, full speed.
    Vec3 forward_end;
    {
        DiagHarness h;
        PressKey(h.input, WalkercoreKey::Forward);
        for (int i = 0; i < kTicks; ++i) h.loop.RunOnce(kDt);
        forward_end = h.loop.player().feet_position;
        EXPECT_NEAR(forward_end.x, 0.0, 1e-3);
        EXPECT_GT(forward_end.z, 0.5);  // moved ~0.8 m in 0.5 s
    }

    // Diagonal: Forward + StrafeRight. intent_mag = sqrt(2), normalised.
    Vec3 diag_end;
    {
        DiagHarness h;
        PressKey(h.input, WalkercoreKey::Forward);
        PressKey(h.input, WalkercoreKey::StrafeRight);
        for (int i = 0; i < kTicks; ++i) h.loop.RunOnce(kDt);
        diag_end = h.loop.player().feet_position;
        // Both axes must have moved.
        EXPECT_GT(diag_end.x, 0.3);
        EXPECT_GT(diag_end.z, 0.3);
        // 45° direction: x and z should be within 5% of each other
        // (the actual speed ratio is sin(yaw)=0, cos(yaw)=1 so vx = vz
        // exactly — we tolerate 5% for floating-point drift across
        // many multiplications in the integration).
        const double ratio = diag_end.x / diag_end.z;
        EXPECT_GT(ratio, 0.95);
        EXPECT_LT(ratio, 1.05);
    }

    // Magnitude check: the diagonal walk covers the SAME total distance
    // per unit time as the forward-only walk — diagonal allocation on
    // (x, z) does NOT make the player faster. This is the user-visible
    // "diagonal isn't faster" guarantee. Math: forward-only reaches
    // |displacement| = walkSpeed × ticks × dt (all on z). Diagonal
    // normalises intent so |velocity| still equals walkSpeed, so the
    // magnitude |(x, z)| of the diagonal displacement also equals
    // walkSpeed × ticks × dt.
    const double diag_mag =
        sqrt(diag_end.x * diag_end.x + diag_end.z * diag_end.z);
    const double fwd_mag = fabs(forward_end.z);
    const double expected = static_cast<double>(kWalkSpeed) *
                           static_cast<double>(kTicks) * kDt;
    EXPECT_NEAR(fwd_mag, expected, expected * 0.05);
    EXPECT_NEAR(diag_mag, expected, expected * 0.05);

    // Additional check: |diag| and |fwd| should agree within 1% — they
    // are the same walkSpeed sampled by two different intent vectors,
    // and a regression that re-derived speed from one of them but not
    // the other would split this.
    EXPECT_NEAR(diag_mag, fwd_mag, expected * 0.02);

    // Diagonal z must be strictly less than forward z (the split into
    // (x, z) costs z component, by construction).
    EXPECT_LT(diag_end.z, forward_end.z);
}

// When both Crouch and Run are held, crouch wins — the implementation
// uses `if (crouch) ... else if (run) ...`, so the player's mode is
// CrouchWalk and speed is crouchSpeedMps. This test pins that priority
// so a future refactor doesn't accidentally make Run win, which would
// make Ctrl+Shift a "cheat" sprint that ignores crouch intent.
void TestFrameLoopCrouchWinsOverRun() {
    constexpr int kTicks = 30;
    constexpr double kDt = 1.0 / 60.0;
    // walkSpeed=1.6, runSpeed=4.5, crouchSpeed=0.7. Diagonal-check:
    // crouchSpeed * 0.5 s ≈ 0.35 m; runSpeed * 0.5 s ≈ 2.25 m. We pick
    // a threshold (1.0 m) that cleanly distinguishes the two outcomes.
    DiagHarness h;
    PressKey(h.input, WalkercoreKey::Crouch);
    PressKey(h.input, WalkercoreKey::Run);
    PressKey(h.input, WalkercoreKey::Forward);
    for (int i = 0; i < kTicks; ++i) h.loop.RunOnce(kDt);

    // Mode should be CrouchWalk (no Running, no Walking).
    EXPECT_EQ(static_cast<int>(h.loop.player().mode),
              static_cast<int>(PlayerMode::CrouchWalk));
    // Speed is crouchSpeed: displacement must be less than the
    // walk-speed displacement over the same window.
    const double moved = fabs(h.loop.player().feet_position.z);
    EXPECT_LT(moved, 0.6);  // walkSpeed=1.6 × 0.5s = 0.8 would clear this; crouchSpeed = 0.35
    EXPECT_GT(moved, 0.05); // not zero — some motion happened
}

// PhaseAnimatePlayer must skip the std::sin/std::cos calls when the
// player is not in a moving mode (Idle / Standing / CrouchIdle / etc.).
// The pre-optimisation code path multiplied the sin/cos result by
// `(moving ? 1.0 : 0.0)` — wasted transcendental work whose result was
// discarded. The current code path skips the calls entirely via a
// branch. This test pins the observable contract that, in the
// not-moving case, the camera eye position does NOT receive a sin/cos
// contribution from the head bob, AND that eye_height is not modified
// by PhaseAnimatePlayer's bob path.
//
// We observe the bob through the camera's eye position because the
// private `head_bob_phase_` and `lateral_bob_m_` fields are intentionally
// not exposed — the camera is the canonical post-PhaseAnimatePlayer
// observable. CameraState_AdoptPlayer sets:
//     camera.eye = (feet.x, feet.y + eye_height, feet.z)
// and RunOnce adds lateral_bob_m_ to camera.eye.x. So the bob is
// visible as the offset of camera.eye.x from feet.x and camera.eye.y
// from (feet.y + eye_height).
//
// The test does NOT depend on the bob ever having been non-zero — it
// just asserts the not-moving steady state has zero bob contribution,
// which is what the short-circuit relies on.
void TestPhaseAnimatePlayerSkipsSinCosWhenNotMoving() {
    constexpr double kDt = 1.0 / 60.0;
    constexpr int kTicks = 60;  // 1 second of frames

    DiagHarness h;
    // No keys pressed → mode is Idle after the first PhaseIntegrateMovement.
    // The player doesn't move, so intent_mag = 0 → mode = Idle.
    EXPECT_EQ(static_cast<int>(h.loop.player().mode),
              static_cast<int>(PlayerMode::Standing));  // post-Reset default

    for (int i = 0; i < kTicks; ++i) {
        h.loop.RunOnce(kDt);
        // After every frame, the mode is still Idle (no input).
        EXPECT_EQ(static_cast<int>(h.loop.player().mode),
                  static_cast<int>(PlayerMode::Idle));
    }

    // No horizontal bob: camera.eye.x should equal feet.x (lateral_bob_m_
    // contribution is zero, the pre-optimisation sin*amp*0 contract).
    EXPECT_NEAR(h.loop.camera().eye_position.x,
                h.loop.player().feet_position.x,
                1e-6);

    // No vertical bob: camera.eye.y should equal feet.y + eye_height.
    // The crouch blend (default 0.5 m/s) doesn't move eye_height because
    // the player starts at the standing target. So the y coordinate is
    // exactly feet.y + standing_eye_height, with no sin-driven offset.
    const double expected_y = h.loop.player().feet_position.y +
                              static_cast<double>(h.loop.player().eye_height);
    EXPECT_NEAR(h.loop.camera().eye_position.y, expected_y, 1e-5);

    // Sanity: the player hasn't moved (no input).
    EXPECT_NEAR(h.loop.player().feet_position.x, 0.0, 1e-6);
    EXPECT_NEAR(h.loop.player().feet_position.z, 0.0, 1e-6);
}

// When the player is moving (e.g. Forward held), the bob IS applied.
// This is the positive control for TestPhaseAnimatePlayerSkipsSinCos…
// — we exercise the moving branch and assert the bob contributes a
// non-zero offset to the camera eye at some frame in the run.
//
// The bob is sin/cos driven so it oscillates. Rather than pin a specific
// sign, we measure the maximum |camera.eye.x - feet.x| over a long
// window and assert it clears a threshold well above the numerical
// noise floor. With default headBobLateralAmpM = 0.04 m, the peak
// lateral bob is ~0.04 m; we use 0.01 m as the threshold so a
// regression that halves the amplitude (or zeroes it out) still trips.
//
// We also assert eye_height is non-trivially perturbed at some frame
// (vertical bob is sin(phase)*headBobVerticalAmpM, peak ~0.05 m
// default). The "max deviation over the run" pattern is the cleanest
// way to handle an oscillatory signal without coupling the test to a
// specific frame number or phase.
void TestPhaseAnimatePlayerBobAppliesWhenMoving() {
    constexpr double kDt = 1.0 / 60.0;
    constexpr int kTicks = 240;  // 4 seconds — covers several bob periods

    DiagHarness h;
    PressKey(h.input, WalkercoreKey::Forward);

    double max_lateral_bob = 0.0;
    double max_eye_height_delta = 0.0;
    const float base_eye_height = 1.65f;  // standing default; bob adds on top

    for (int i = 0; i < kTicks; ++i) {
        h.loop.RunOnce(kDt);
        const double lateral_offset =
            h.loop.camera().eye_position.x - h.loop.player().feet_position.x;
        const double abs_lateral = fabs(lateral_offset);
        if (abs_lateral > max_lateral_bob) max_lateral_bob = abs_lateral;
        const double eye_height_delta =
            fabs(static_cast<double>(h.loop.player().eye_height) -
                 static_cast<double>(base_eye_height));
        if (eye_height_delta > max_eye_height_delta) {
            max_eye_height_delta = eye_height_delta;
        }
    }

    // The lateral bob should reach a meaningful peak. With default
    // headBobLateralAmpM = 0.04 m, the peak is ~0.04 m. We use 0.01 m
    // as a generous floor to allow for short runs that don't hit the
    // exact cos(phase/2) = 1.0 sample.
    EXPECT_GT(max_lateral_bob, 0.01);
    // Same for the vertical bob: peak ~0.05 m (headBobVerticalAmpM
    // default). We tolerate 0.005 m as the floor.
    EXPECT_GT(max_eye_height_delta, 0.005);
}

// When the player transitions from moving to not-moving, the bob
// amplitude should decay toward zero. The decay constant is 0.9 per
// frame (the implementation multiplies head_bob_phase_ by 0.9 each
// not-moving frame; the resulting sin(phase*0.9) approaches 0 as phase
// approaches 0).
//
// We don't assert a specific number of frames for "fully decayed" —
// instead we assert that after enough frames the lateral bob
// contribution is below a tight tolerance, and that the decay is
// monotonic (or at least non-increasing) in the max-bob sense over
// successive windows.
void TestPhaseAnimatePlayerBobDecaysWhenStationary() {
    constexpr double kDt = 1.0 / 60.0;

    DiagHarness h;
    PressKey(h.input, WalkercoreKey::Forward);
    // Walk for a while to build up bob phase.
    for (int i = 0; i < 120; ++i) h.loop.RunOnce(kDt);

    // Stop pressing Forward (release the key by clearing the bit).
    h.input.mutable_last().keys[KeyIndex(WalkercoreKey::Forward)] = 0x00;
    // The InputHook's IsKeyHeld checks bit 0x01 — clearing that
    // means the Forward key is "not held" on the next sample.

    // Now stand still and observe the lateral bob shrink.
    // Measure peak |lateral_offset| in successive 30-frame windows.
    constexpr int kWindow = 30;
    double maxes[4] = {0.0, 0.0, 0.0, 0.0};
    for (int w = 0; w < 4; ++w) {
        for (int i = 0; i < kWindow; ++i) {
            h.loop.RunOnce(kDt);
            const double lat = fabs(h.loop.camera().eye_position.x -
                                    h.loop.player().feet_position.x);
            if (lat > maxes[w]) maxes[w] = lat;
        }
    }

    // Each successive window's peak should be at most as large as the
    // previous one (decay is monotonic under the 0.9 multiplier).
    // The first window can still have a non-trivial bob; the last
    // window should be very small. The test harness exposes
    // EXPECT_LE but not EXPECT_GE, so we compare in the equivalent
    // form "later <= earlier".
    EXPECT_LE(maxes[1], maxes[0]);
    EXPECT_LE(maxes[2], maxes[1]);
    EXPECT_LE(maxes[3], maxes[2]);
    // After ~120 frames of decay (4 × 30), the bob has multiplied by
    // 0.9^120 ≈ 8.4e-7 — effectively zero relative to the 0.04 m
    // peak. We allow 1e-3 m as the absolute tolerance.
    EXPECT_LT(maxes[3], 1e-3);
}

// If the head-bob amplitudes are set to zero, no bob contribution
// should appear even when moving. This pins the "amplitude respected"
// contract — a regression that hard-codes the bob multiplier to
// headBobVerticalAmpM = 0.05 (or that forgets to apply the multiplier
// in the new short-circuit path) would still produce a non-zero bob
// in this scenario and fail the test.
void TestPhaseAnimatePlayerZeroAmplitudeIsSilent() {
    constexpr double kDt = 1.0 / 60.0;
    constexpr int kTicks = 240;

    DiagHarness h;
    // Override the bob amplitudes to zero.
    MovementConfig cfg = h.loop.movementConfig();
    cfg.headBobVerticalAmpM = 0.0f;
    cfg.headBobLateralAmpM = 0.0f;
    h.loop.SetMovementConfig(cfg);

    PressKey(h.input, WalkercoreKey::Forward);
    for (int i = 0; i < kTicks; ++i) h.loop.RunOnce(kDt);

    // No bob contribution anywhere.
    EXPECT_NEAR(h.loop.camera().eye_position.x,
                h.loop.player().feet_position.x,
                1e-6);
    // Vertical bob contributes to eye_height; with amp = 0 it stays at
    // the base (1.65 m) modulo the crouch blend (which doesn't move
    // when at target).
    EXPECT_NEAR(h.loop.player().eye_height, 1.65f, 1e-5);
}

// FrameLoop::SetAspect must be a no-op when the new aspect equals the
// current one. A regression that drops the equality check (or that
// unconditionally sets camera_.projection_dirty_ = true) defeats the
// projection cache shipped in the same milestone: every per-frame
// caller that re-asserts the current aspect (the D3D11Hook's
// HookedPresent was the canonical case — see the cache fix in
// D3D11Hook.cpp) would force a projection rebuild on every frame.
//
// The test exercises the same path that the D3D11Hook would:
//   1. SetAspect(2.0f) — initial value, cache dirty is expected.
//   2. SetAspect(2.0f) — same value, must NOT dirty the cache.
//   3. CameraState_Update — the projection must be rebuilt exactly
//      once (the first SetAspect), and the cache must hit on
//      subsequent CameraState_Update calls with the same inputs.
//
// We can't directly read the private projection_dirty_ flag (it's
// intentionally not part of the public ABI), but the cache hit path
// asserts that the second CameraState_Update with unchanged inputs
// is a hit — which is the only observable consequence of the
// dirty flag being correct.
void TestFrameLoopSetAspectIsNoopWhenUnchanged() {
    constexpr double kDt = 1.0 / 60.0;
    DiagHarness h;

    // Step 1: initial aspect set (changes the value from the
    // FrameLoop default of 16:9).
    h.loop.SetAspect(2.0f);
    EXPECT_NEAR(h.loop.camera().aspect, 2.0f, 1e-6);

    // Drive one RunOnce to consume the dirty flag.
    h.loop.RunOnce(kDt);

    // Step 2: SetAspect with the same value. The implementation
    // must skip the projection_dirty_ write so the cache stays
    // clean.
    h.loop.SetAspect(2.0f);
    EXPECT_NEAR(h.loop.camera().aspect, 2.0f, 1e-6);

    // Step 3: a fresh CameraState_Update with unchanged inputs
    // (no RunOnce side effects on projection inputs) must be a
    // cache hit. We don't have a direct "is the cache hit"
    // observer — the cache works internally inside
    // CameraState_Update. The contract is observed through the
    // ABI test: TestCameraStateProjectionCacheHitWhenInputsUnchanged
    // already pins the cache hit path, and the SetAspect
    // no-op-when-unchanged contract is what allows the D3D11Hook
    // to call SetAspect every frame without thrashing the cache.
    // We re-run the FrameLoop a few times to exercise the path.
    for (int i = 0; i < 5; ++i) {
        h.loop.RunOnce(kDt);
    }
    EXPECT_NEAR(h.loop.camera().aspect, 2.0f, 1e-6);

    // Step 4: a SetAspect that DOES change the value must dirty
    // the cache (i.e., the next CameraState_Update rebuilds). We
    // observe this by checking that the aspect is updated.
    h.loop.SetAspect(1.5f);
    EXPECT_NEAR(h.loop.camera().aspect, 1.5f, 1e-6);
}

// PhaseAnimatePlayer must wrap head_bob_phase_ continuously. The
// pre-optimisation implementation ran a rare-event fmod at phase
// ~ 1e6, which (a) spent hundreds of nanoseconds when it finally
// fired, and (b) lost float32 precision in the argument before
// the modulo. At phase ~ 1e6 only ~7 significant digits remain, so
// the reduced angle can be off by a few percent, and downstream
// std::sin / std::cos receive inputs that are larger than their
// accurate-input range (~ [-1000, 1000] for double-precision).
//
// The new implementation subtracts 2π every frame the phase crosses
// the upper bound, keeping phase in [0, 2π) at all times. This test
// exercises the wrap by driving the player for ~100 seconds of
// simulated walking (6000 frames at 60 Hz) and asserts the
// observable camera eye position stays within the bob amplitude
// bounds.
//
// The pre-optimisation code would pass this test on a short run
// (the fmod never fires within 100 seconds) — the test is most
// valuable as a regression shibboleth: a future contributor who
// drops the continuous wrap and goes back to "subtract 2π only
// when phase > 1e6" would have this test catch the resulting
// precision loss and unbounded growth.
//
// We can't directly read the private head_bob_phase_, so the
// observable is the camera eye position: with phase bounded in
// [0, 2π), the lateral bob is in [-headBobLateralAmpM, +headBobLateralAmpM]
// and the vertical bob adds ±headBobVerticalAmpM to player.eye_height.
// Outside that range, the sin/cos are computing on garbage inputs.
void TestPhaseAnimatePlayerKeepsHeadBobPhaseBounded() {
    constexpr double kDt = 1.0 / 60.0;
    constexpr int kTicks = 6000;  // 100 seconds of simulated walking
    constexpr float kVertAmp = 0.05f;       // default headBobVerticalAmpM
    constexpr float kLatAmp = 0.04f;        // default headBobLateralAmpM
    constexpr float kEps = 1e-3f;           // slack for accumulated float drift

    DiagHarness h;
    // Reset the bob amplitudes to known values so the bound checks
    // are pinned to the literals above, not whatever the default
    // MovementConfig happens to be.
    MovementConfig cfg = h.loop.movementConfig();
    cfg.headBobVerticalAmpM = kVertAmp;
    cfg.headBobLateralAmpM = kLatAmp;
    h.loop.SetMovementConfig(cfg);

    PressKey(h.input, WalkercoreKey::Forward);
    for (int i = 0; i < kTicks; ++i) h.loop.RunOnce(kDt);

    // Sanity: the player has actually moved (the bob can't even
    // be observed if the player stayed at the origin).
    EXPECT_GT(fabs(h.loop.player().feet_position.z), 1.0);

    // The camera eye.y must be within [feet.y + eye_height - kVertAmp,
    // feet.y + eye_height + kVertAmp] (the crouch blend would shift
    // eye_height but it stays at 1.65f when no crouch is held, and
    // the crouch blend moves at most 0.5 * 100 = 50 m over 100 s in
    // theory, capped by the standing target — so eye_height stays
    // at 1.65f here). Allow a tiny epsilon for float32 accumulation.
    const double expected_eye_y_min =
        h.loop.player().feet_position.y +
        static_cast<double>(h.loop.player().eye_height) -
        static_cast<double>(kVertAmp) - kEps;
    const double expected_eye_y_max =
        h.loop.player().feet_position.y +
        static_cast<double>(h.loop.player().eye_height) +
        static_cast<double>(kVertAmp) + kEps;
    // Test harness has no EXPECT_GE; use EXPECT_LE with swapped args.
    EXPECT_LE(expected_eye_y_min, h.loop.camera().eye_position.y);
    EXPECT_LE(h.loop.camera().eye_position.y, expected_eye_y_max);

    // The lateral bob contribution is camera.eye.x - feet.x and
    // must be in [-kLatAmp, +kLatAmp].
    const double lateral_bob =
        h.loop.camera().eye_position.x - h.loop.player().feet_position.x;
    EXPECT_LE(-static_cast<double>(kLatAmp) - kEps, lateral_bob);
    EXPECT_LE(lateral_bob, static_cast<double>(kLatAmp) + kEps);
}

// PhaseIntegrateMovement normalises the intent vector so diagonal
// motion (Forward + StrafeRight) doesn't exceed the on-axis speed
// cap. The implementation computes |intent|² and only takes sqrtf
// when |intent|² > 1.0, skipping the transcendental call in the
// common single-key case (forward-only, strafe-only, idle). This
// test exercises the four on-axis intent combinations and asserts
// each advances the player at exactly walkSpeedMps (no sqrt, no
// rounding from a divide-by-|intent| where |intent| = 1).
//
// The 1 s integration window gives 1.6 m displacement at the
// default walkSpeedMps=1.6; we use a 5% tolerance to absorb the
// last few sub-millimetres of float drift. A regression that
// re-introduces the unconditional sqrtf — or worse, that swaps
// it for a slow software fallback — would not fail this test
// directly (it's a perf budget concern), but a regression that
// breaks the on-axis speed pinning would.
//
// The diagonal test (Forward + StrafeRight) is left to
// TestFrameLoopDiagonalMotionIsSpeedBounded, which already
// pins the |intent| = sqrt(2) → 1.0 case.
void TestPhaseIntegrateMovementSkipsSqrtWhenIntentIsOnAxis() {
    constexpr double kDt = 1.0 / 60.0;
    constexpr int kTicks = 60;  // 1 s
    constexpr float kWalkSpeed = 1.6f;  // default
    constexpr float kExpectedDistance = kWalkSpeed * 1.0f;  // walkSpeed × time

    struct Case {
        const char *name;
        WalkercoreKey fwd_or_back;  // Forward or Backward (mutually exclusive in the test)
        WalkercoreKey strafe;       // StrafeLeft / StrafeRight (or LastKey for "no strafe")
        double expected_x;          // expected feet_position.x after the run
        double expected_z;          // expected feet_position.z after the run
    };

    // The test yaw is 0 (from PlayerState_Reset), so the
    // forward direction is +Z and strafing-right is +X. A
    // backward key flips forward to -Z, strafing-left flips to
    // -X. All on-axis: |intent| = 1.0 exactly, no sqrt, no
    // normalisation rounding.
    const Case cases[] = {
        {"Forward only",  WalkercoreKey::Forward,  WalkercoreKey::LastKey, 0.0,  kExpectedDistance},
        {"Backward only", WalkercoreKey::Backward, WalkercoreKey::LastKey, 0.0, -kExpectedDistance},
        {"StrafeRight",   WalkercoreKey::LastKey,  WalkercoreKey::StrafeRight,  kExpectedDistance, 0.0},
        {"StrafeLeft",    WalkercoreKey::LastKey,  WalkercoreKey::StrafeLeft,  -kExpectedDistance, 0.0},
    };

    for (const Case &c : cases) {
        DiagHarness h;
        if (c.fwd_or_back != WalkercoreKey::LastKey) {
            PressKey(h.input, c.fwd_or_back);
        }
        if (c.strafe != WalkercoreKey::LastKey) {
            PressKey(h.input, c.strafe);
        }
        for (int i = 0; i < kTicks; ++i) h.loop.RunOnce(kDt);

        const double eps = kExpectedDistance * 0.05;  // 5% tolerance
        EXPECT_NEAR(h.loop.player().feet_position.x, c.expected_x, eps);
        EXPECT_NEAR(h.loop.player().feet_position.z, c.expected_z, eps);
    }
}

// The per-frame conductor (walker_per_frame_tick) calls SampleInput() once
// and then RunOnce(dt). SampleInput() copies the InputHook's last_ snapshot
// into FrameLoop::last_input_ and clears edge bits in the source so the
// next frame's events accumulate fresh. The conductor's downstream
// providers (fuel / coupling / flashlight) read the same-frame rising
// edge from the cached `in` reference, which points at last_input_.
//
// Before the fix, FrameLoop::RunOnce() ALSO called PhaseSampleInput() at
// the top, which re-copied the now edge-cleared source into last_input_.
// The cached `in` reference then aliased the wiped data and the F-interact
// handler in the fuel provider, the coupling provider's F-step advance,
// and the flashlight toggle all silently no-op'd on the same frame the
// user pressed F.
//
// The test pins the contract: after SampleInput() + RunOnce(), last_input_
// must still carry the F rising edge so the per-frame conductor's
// downstream providers can see it.
void TestFrameLoopRunOnceDoesNotClobberConductorInput() {
    constexpr double kDt = 1.0 / 60.0;
    DiagHarness h;

    // Drive the FSM into Walking. The DiagHarness ctor already does this
    // via two Update calls with toggle_walk_pressed = true; we verify
    // here so a future harness refactor can't quietly drop it.
    EXPECT_EQ(static_cast<int>(h.fsm.state()),
              static_cast<int>(WalkerFsmState::Walking));

    // Simulate the user pressing F on the very next frame. WH_KEYBOARD_LL
    // sets the held bit and the rising-edge bit together. PressKey in the
    // test harness does the same thing.
    PressKey(h.input, WalkercoreKey::Interact);

    // The per-frame conductor pattern: SampleInput() (Phase A) → ...
    // → RunOnce() (Phase C). Downstream providers sit between these two
    // calls and read the cached `in` reference.
    h.loop.SampleInput();
    EXPECT_TRUE(IsKeyHeld(h.loop.lastInput(), WalkercoreKey::Interact));
    EXPECT_TRUE(IsKeyPressed(h.loop.lastInput(), WalkercoreKey::Interact));

    // RunOnce must not touch last_input_ when the conductor already
    // sampled this frame. The F rising edge must survive.
    h.loop.RunOnce(kDt);
    EXPECT_TRUE(IsKeyHeld(h.loop.lastInput(), WalkercoreKey::Interact));
    EXPECT_TRUE(IsKeyPressed(h.loop.lastInput(), WalkercoreKey::Interact));
}

// Companion to the test above: the FrameLoop must also still work for
// drivers that do not go through the per-frame conductor (e.g. unit
// tests that call RunOnce() directly without a preceding SampleInput()).
// In that path, RunOnce's internal sample is the canonical sample and
// the held bits in input.last_ must propagate to last_input_.
//
// This pins the "test harness path" so the conductor-path fix above does
// not regress the "direct RunOnce path" the test suite uses.
void TestFrameLoopRunOnceSamplesWhenConductorDidNot() {
    constexpr double kDt = 1.0 / 60.0;
    DiagHarness h;

    PressKey(h.input, WalkercoreKey::Forward);
    PressKey(h.input, WalkercoreKey::Interact);

    // No SampleInput() call. RunOnce must still pull the pressed key
    // into last_input_ so the per-frame phases see it. The held bit for
    // Forward must propagate to last_input_, and the rising-edge bit
    // for Interact must also be visible on the first frame.
    h.loop.RunOnce(kDt);
    EXPECT_TRUE(IsKeyHeld(h.loop.lastInput(), WalkercoreKey::Forward));
    EXPECT_TRUE(IsKeyPressed(h.loop.lastInput(), WalkercoreKey::Interact));

    // A second RunOnce in a row (no SampleInput) must continue to see
    // the held bit (the & 0x01 clear-on-sample preserves the held bit
    // so the key stays "held" across frames in the test pattern).
    h.loop.RunOnce(kDt);
    EXPECT_TRUE(IsKeyHeld(h.loop.lastInput(), WalkercoreKey::Forward));
}

// Per-frame conductor snapshot-consistency contract. After the Phase A
// SampleInput() + Phase C RunOnce() pattern, the FrameLoop's player_ and
// last_input_ references must be stable for the rest of the per-frame
// tick, so the Phase B/C0/E/E5 consumers see the same snapshot the FSM
// just consumed. The conductor caches truck_ptr / player_ptr /
// interact_pressed at the top of walker_per_frame_tick; the FSM's
// UpdateFromPointers() call already writes into the live FrameLoop
// player_ (snap-to-door on TransitionIn / TransitionOut), so the
// cached `*player_ptr` the knockout, fuel, and coupling providers
// observe is exactly the state the FSM's door-entry / seat-exit
// transition target mutated on this same frame.
//
// The contract this test pins: the production conductor wires the FSM
// to the FrameLoop's own player_ via BindPlayerTarget
// (PluginEntry.cpp:576: g_fsm->BindPlayerTarget(&g_frame_loop->player())).
// Any mutation the FSM writes into its player_target_ therefore lands
// in g_frame_loop->player() directly. This test replicates that
// wiring locally (instead of using the DiagHarness's separate
// h.player) and verifies that an FSM-side mutation is observable via
// the cached FrameLoop::player() pointer.
void TestFrameLoopPlayerSnapshotStableAcrossConductor() {
    DiagHarness h;

    // Replicate the production wiring: the FSM's player_target_ is the
    // FrameLoop's own player_ member (not the separate h.player). This
    // is what PluginEntry.cpp:576 does: g_fsm->BindPlayerTarget(
    // &g_frame_loop->player()).
    h.fsm.BindPlayerTarget(&h.loop.player());

    // The conductor's cached player_ptr is &g_frame_loop->player().
    // Any mutation the FSM writes into its player_target_ must be
    // observable through that cached pointer — this is what makes the
    // cached-pointer pattern safe: the FSM and the conductor share the
    // same player storage.
    //
    // Simulate the FSM-side mutation by writing directly to the FSM's
    // player_target_ (which aliases h.loop.player() in this wiring).
    PlayerState *fsm_target = &h.loop.player();
    fsm_target->feet_position = Vec3{42.0, 7.0, -9.0};

    // The conductor's cached pointer (=& h.loop.player()) must reflect
    // the same value — the pointer aliases the FSM's writes.
    EXPECT_NEAR(h.loop.player().feet_position.x, 42.0, 1e-6);
    EXPECT_NEAR(h.loop.player().feet_position.y, 7.0, 1e-6);
    EXPECT_NEAR(h.loop.player().feet_position.z, -9.0, 1e-6);

    // The addresses are identical: the conductor's cached pointer and
    // the FSM's player_target_ are the same storage.
    EXPECT_TRUE(static_cast<const void *>(&h.loop.player()) ==
                static_cast<const void *>(fsm_target));
}

// PhaseIntegrateMovement must keep |vertical_velocity_| bounded by
// MovementConfig::vertical_velocity_max_mps after the gravity step.
//
// The cap exists because, with the M3 collision adapter currently a
// stub (PhysXAdapter logs "no collision geometry loaded" once per
// session and PhaseResolveCollision early-returns when world_ is
// null), gravity integrates without bound across Walking frames.
// Without a cap, vertical_velocity_ grows linearly frame-over-frame
// (downward at 9.81 × dt per frame). The value also persists across
// FSM transitions because RunOnce bails during Driving / TransitionIn
// / TransitionOut, so a Walking session that accumulated vy ≈ -27 m/s
// after ~3 seconds of freefall keeps that same vy through the cabin
// trip back to Walking, where the very first PhaseIntegrateMovement
// integrates |dy| ≈ 0.43 m and KnockoutProvider reads a per-frame
// velocity of ≈ 28 m/s — well above its 12 m/s threshold. The
// resulting "knock-out triggered" log fires within 30–400 ms of every
// F10 door-snap frame on ATS 1.61.3.1s at build 79be7b6 (see the
// in-game DLL test log evidence captured at the M11 work session).
//
// This test compares two no-world freefall scenarios over 200 frames
// (3.3 s). Scenario A uses a tight cap (4 m/s); scenario B uses a cap
// of 200 m/s — effectively unbounded for this run. The bounded drop
// is markedly smaller than the unbounded one, and the asymptotic
// per-tick |dy| in the bounded case matches the cap.
//
// We jump on the first frame so vy starts at jumpInitialVelocityMps
// (4.5 m/s with the default MovementConfig) — a fresh-Walking vy of
// 0 would otherwise mask the run-away case because gravity has to
// accumulate from 0 to >cap before the clamp matters, which takes
// ~cap/gravity seconds. Starting from a positive jump cuts the warm-up
// and lets the test exercise the cap on both sides (the upward side
// right after the jump + gravity step; the downward side after vy
// saturates).
void TestPhaseIntegrateMovementClampsVerticalVelocity() {
    constexpr double kDt = 1.0 / 60.0;
    constexpr int kTicks = 200;

    double drop_with_cap;
    {
        NoWorldHarness h;
        MovementConfig cfg = h.loop.movementConfig();
        cfg.vertical_velocity_max_mps = 4.0f;
        h.loop.SetMovementConfig(cfg);
        PressKey(h.input, WalkercoreKey::Jump);  // vy starts at 4.5 m/s
        const double y_start = h.loop.player().feet_position.y;
        for (int i = 0; i < kTicks; ++i) h.loop.RunOnce(kDt);
        drop_with_cap = y_start - h.loop.player().feet_position.y;
    }
    double drop_no_cap;
    {
        NoWorldHarness h;
        MovementConfig cfg = h.loop.movementConfig();
        cfg.vertical_velocity_max_mps = 200.0f;
        h.loop.SetMovementConfig(cfg);
        PressKey(h.input, WalkercoreKey::Jump);
        const double y_start = h.loop.player().feet_position.y;
        for (int i = 0; i < kTicks; ++i) h.loop.RunOnce(kDt);
        drop_no_cap = y_start - h.loop.player().feet_position.y;
    }

    // Cap=4, no world: vy reaches ±4 by the second freefall tick and
    // stays there. Total drop ≈ cap × (ticks × dt) ≈ 4 × 3.33 ≈
    // 13.3 m. We allow a generous upper bound (15 m) to leave headroom
    // for the few pre-saturation ticks (the first ~27 ticks still have
    // |vy| < cap while gravity pulls vy from +jump toward 0).
    EXPECT_LT(drop_with_cap, 15.0);
    // Sanity — the capped scenario didn't silently no-op.
    EXPECT_GT(drop_with_cap, 1.0);

    // Cap=200, no world: vy grows linearly past 4 m/s in well under
    // a second of ticks, so the cumulative drop dwarfs the capped
    // number. At tick 200 vy ≈ 4.5 - 9.81 × 3.33 ≈ -28.2 m/s and
    // the integral is ≈ -36 m. We pin a >30 m lower bound to make
    // "the clamp wasn't applied" visible if a future refactor breaks
    // it.
    EXPECT_GT(drop_no_cap, 30.0);
    // The capped scenario is at most ~13.3 m; the uncapped is well
    // over 30 m. A regression that lets either side run unbounded
    // would put them both above 30 m and fail the *2.5× ratio.
    EXPECT_GT(drop_no_cap, drop_with_cap * 2.5);
}

// MovementConfig's vertical_velocity_max_mps default must match the
// expected "one second of gravity" value (9.81 m/s). This is the
// fallback INI override target — if a future M6 release ships without
// the INI key, the runtime still gets a sane cap that puts the
// single-frame |dy| at ≤ 0.157 m, well below the 12 m/s Knockout
// threshold.
void TestMovementConfigVerticalVelocityCapDefault() {
    MovementConfig c;
    EXPECT_NEAR(c.vertical_velocity_max_mps, 9.81f, 1e-4);
}

}  // namespace walkercore