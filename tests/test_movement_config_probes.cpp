// WalkerCore — MovementConfig field-exhaustive probe.
//
// Every documented field of MovementConfig must have an observable effect
// on the on-foot simulation OR must be explicitly marked "no consumer
// yet" with a finding. This test pins both states with isolated scenarios:
//
//   1. Fields consumed today (12 of 14) → vary the field, run a fixed
//      scenario, observe the relevant output, assert it differs from the
//      default by a non-trivial amount.
//
//   2. Fields declared but currently unconsumed (2 of 14, the M3 step-up
//      / slope knobs) → vary the field, run a fixed scenario, assert
//      that the relevant output is unaffected. This pins the
//      honest-no-consumer state until the M3 lander ships; replacing
//      either field with a real consumer in PhaseResolveCollision will
//      change these two assertions and that's the explicit signal to
//      move the field out of this stub list.
//
// The probe uses fresh FrameLoop / WalkerFsm / InputHook per case so
// residual state from one case cannot perturb another.

#include "TestHarness.h"

#include <math.h>

#include "walkercore/CameraState.h"
#include "walkercore/FrameLoop.h"
#include "walkercore/InputHook.h"
#include "walkercore/InputState.h"
#include "walkercore/MovementConfig.h"
#include "walkercore/PlayerState.h"
#include "walkercore/TruckState.h"
#include "walkercore/WalkerFsm.h"
#include "walkercore/WorldState.h"

namespace walkercore {

namespace {

// Open world with a flat ground at y = 0. SphereSweep accepts the full
// displacement on the horizontal axes (so the player can walk freely)
// and reports a hit at y = 0 when the vertical sweep lands on the
// ground (so the player stays grounded and `grounded_` flips to true
// after the first landing, which keeps the head bob phase running).
// This is the bare-minimum world the FrameLoop needs to exercise
// every MovementConfig field without the M3 PhysX lander.
class OpenWorld : public IWorldGeometry {
public:
    bool Raycast(Vec3, Vec3, double, Vec3 *, Vec3 *) override { return false; }
    MoveResult SphereSweep(Vec3 origin, double, Vec3 displacement) override {
        const double nx = origin.x + displacement.x;
        const double ny = origin.y + displacement.y;
        const double nz = origin.z + displacement.z;
        // Clamp the vertical motion at y = 0: any descent past the
        // ground lands on the ground.
        if (displacement.y < 0.0 && ny <= 0.0) {
            return MoveResult{Vec3{nx, 0.0, nz},
                               Vec3{0.0, 1.0, 0.0},
                               static_cast<float>(-displacement.y),
                               true};
        }
        return MoveResult{Vec3{nx, ny, nz},
                           Vec3{0.0, 0.0, 0.0},
                           0.0f,
                           false};
    }
    bool groundY(Vec3, double *out_y) override {
        if (out_y) *out_y = 0.0;
        return true;
    }
};

// Simulate a key-down on the bound InputHook's last_ snapshot. Mirrors the
// production WH_KEYBOARD_LL handler: sets the held bit and (on the rising
// edge) the pressed bit. The next Sample() call from FrameLoop copies the
// state into last_input_ (which PhaseIntegrateMovement reads).
void PressKey(InputHook &hook, WalkercoreKey k) {
    uint8_t &bits = hook.mutable_last().keys[KeyIndex(k)];
    const bool was_held = (bits & 0x01) != 0;
    bits |= 0x01;
    if (!was_held) bits |= 0x02;
}

// Release a key: clear held, set falling edge.
void ReleaseKey(InputHook &hook, WalkercoreKey k) {
    uint8_t &bits = hook.mutable_last().keys[KeyIndex(k)];
    const bool was_held = (bits & 0x01) != 0;
    if (was_held) bits |= 0x04;
    bits &= ~0x01;
}

// Set raw mouse delta on the bound InputHook.
void SetMouseDelta(InputHook &hook, int dx, int dy) {
    hook.mutable_last().mouse_dx = dx;
    hook.mutable_last().mouse_dy = dy;
}

// Set up a FrameLoop + WalkerFsm + InputHook in Walking mode, ready for
// scenario runs. Returns a reference to the FrameLoop and the
// PlayerState (so callers can observe post-scenario values via
// loop.player() / loop.camera()).
struct Harness {
    FrameLoop loop;
    WalkerFsm fsm;
    InputHook input;
    PlayerState player;
    OpenWorld world;

    Harness() {
        PlayerState_Reset(player);
        fsm.BindPlayerTarget(&player);
        loop.BindFsm(&fsm);
        loop.BindInput(&input);
        loop.BindWorld(&world);
        // Short transition so we reach Walking in one Update() call.
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
        fsm.Update(inputs, 0.10, &target);  // advance past transition
    }

    void RunTicks(int n, double dt = 1.0 / 60.0) {
        for (int i = 0; i < n; ++i) {
            loop.RunOnce(dt);
        }
    }
};

// Build a harness, apply a movement config and a key state, run N ticks,
// and return the resulting player position + camera eye position. Each
// invocation constructs a fresh harness so residual state from prior
// cases cannot leak.
struct Result {
    Vec3 feet;
    Vec3 eye;
    float yaw;
    float pitch;
    float eye_height;
    PlayerMode mode;
};

Result ScenarioWith(MovementConfig cfg, WalkercoreKey held_key,
                    int ticks, int mouse_dx = 0, int mouse_dy = 0) {
    Harness h;
    if (held_key != WalkercoreKey::LastKey) {
        PressKey(h.input, held_key);
    }
    if (mouse_dx != 0 || mouse_dy != 0) {
        SetMouseDelta(h.input, mouse_dx, mouse_dy);
    }
    h.loop.SetMovementConfig(cfg);
    h.RunTicks(ticks);
    Result r;
    r.feet = h.loop.player().feet_position;
    r.eye = h.loop.camera().eye_position;
    r.yaw = h.loop.player().yaw_radians;
    r.pitch = h.loop.player().pitch_radians;
    r.eye_height = h.loop.player().eye_height;
    r.mode = h.loop.player().mode;
    return r;
}

// Variant that presses a one-shot key (Jump) on the first tick only and
// holds another key for the duration. Mouse delta is applied on the
// first sample so PhaseApplyMouseLook sees it before zeroing it.
Result ScenarioWithJump(MovementConfig cfg, int ticks) {
    Harness h;
    PressKey(h.input, WalkercoreKey::Jump);  // rising edge consumed in tick 0
    h.loop.SetMovementConfig(cfg);
    h.RunTicks(ticks);
    Result r;
    r.feet = h.loop.player().feet_position;
    r.eye = h.loop.camera().eye_position;
    r.yaw = h.loop.player().yaw_radians;
    r.pitch = h.loop.player().pitch_radians;
    r.eye_height = h.loop.player().eye_height;
    r.mode = h.loop.player().mode;
    return r;
}

// Variant for mouse-look fields. Single-tick to read a clean PhaseApply-
// MouseLook effect before user-position integration perturbs things.
Result ScenarioWithMouseDelta(MovementConfig cfg, int mouse_dx, int mouse_dy) {
    Harness h;
    SetMouseDelta(h.input, mouse_dx, mouse_dy);
    h.loop.SetMovementConfig(cfg);
    h.RunTicks(1);  // one tick so PhaseApplyMouseLook fires
    Result r;
    r.feet = h.loop.player().feet_position;
    r.eye = h.loop.camera().eye_position;
    r.yaw = h.loop.player().yaw_radians;
    r.pitch = h.loop.player().pitch_radians;
    r.eye_height = h.loop.player().eye_height;
    r.mode = h.loop.player().mode;
    return r;
}

// Variant that measures the bob envelope (peak - trough) over N ticks.
double BobEnvelope(MovementConfig cfg, WalkercoreKey held_key, int ticks) {
    Harness h;
    PressKey(h.input, held_key);
    h.loop.SetMovementConfig(cfg);
    float y_min = 1e9f, y_max = -1e9f;
    for (int i = 0; i < ticks; ++i) {
        h.loop.RunOnce(1.0 / 60.0);
        const float y = h.loop.player().eye_height;
        if (y < y_min) y_min = y;
        if (y > y_max) y_max = y;
    }
    return static_cast<double>(y_max - y_min);
}

// Count zero-crossings of the lateral bob over N ticks — a count of 2
// means one full cycle (peak → trough → peak). Differences in
// headBobFrequencyHz change the cycle period, so the zero-crossing
// count over a fixed sample window differs.
int LateralBobZeroCrossings(MovementConfig cfg, int ticks) {
    Harness h;
    PressKey(h.input, WalkercoreKey::Forward);
    h.loop.SetMovementConfig(cfg);
    int crossings = 0;
    int prev_sign = 0;
    for (int i = 0; i < ticks; ++i) {
        h.loop.RunOnce(1.0 / 60.0);
        // PhaseAnimatePlayer writes lateral_bob into the camera
        // eye.position.x via AdoptingPlayer + lateral offset. Use a
        // proxy via the player's eyes and the camera. We poll the
        // camera eye X minus an initial baseline.
        const float x = h.loop.camera().eye_position.x;
        const int sign = (x > 0.001f) ? 1 : (x < -0.001f) ? -1 : 0;
        if (sign != 0 && prev_sign != 0 && sign != prev_sign) {
            crossings++;
        }
        if (sign != 0) prev_sign = sign;
    }
    return crossings;
}

constexpr double kEpsSmall = 1e-4;

// A MovementConfig that overrides crouchBlendMps to a very fast value so
// eye_height reaches the target on the first tick; tests that probe
// eye-height fields are isolated from blend dynamics by overriding the
// field, not by waiting on a multi-tick blend.
inline MovementConfig MakeTestConfig() {
    MovementConfig c;
    c.crouchBlendMps = 1000.0f;  // effectively instant
    return c;
}

}  // namespace

void TestMovementConfigEveryFieldHasObservableEffect() {
    // --- 1. walkSpeedMps ---
    // With Forward held, after 30 ticks the player has moved forward.
    // Doubling walkSpeedMps should at least 1.5× the displacement.
    {
        MovementConfig baseline = MakeTestConfig();
        baseline.walkSpeedMps = 1.6f;  // default
        MovementConfig doubled = baseline;
        doubled.walkSpeedMps = 3.2f;
        Result base = ScenarioWith(baseline, WalkercoreKey::Forward, 30);
        Result fast = ScenarioWith(doubled, WalkercoreKey::Forward, 30);
        const double base_dist = fabs(base.feet.z);
        const double fast_dist = fabs(fast.feet.z);
        EXPECT_GT(base_dist, 0.5);
        EXPECT_GT(fast_dist, base_dist * 1.3);
        EXPECT_EQ(static_cast<int>(base.mode), static_cast<int>(PlayerMode::Walking));
        EXPECT_EQ(static_cast<int>(fast.mode), static_cast<int>(PlayerMode::Walking));
    }

    // --- 2. runSpeedMps ---
    // With Run+Forward, mode = Running. Doubling runSpeedMps should
    // measurably increase displacement. We build the harness manually
    // because ScenarioWith only handles one held key at a time.
    {
        MovementConfig baseline = MakeTestConfig();
        baseline.runSpeedMps = 4.5f;
        MovementConfig doubled = baseline;
        doubled.runSpeedMps = 9.0f;

        Harness hb;
        PressKey(hb.input, WalkercoreKey::Run);
        PressKey(hb.input, WalkercoreKey::Forward);
        hb.loop.SetMovementConfig(baseline);
        hb.RunTicks(30);
        const double base_dist = fabs(hb.loop.player().feet_position.z);

        Harness hf;
        PressKey(hf.input, WalkercoreKey::Run);
        PressKey(hf.input, WalkercoreKey::Forward);
        hf.loop.SetMovementConfig(doubled);
        hf.RunTicks(30);
        const double fast_dist = fabs(hf.loop.player().feet_position.z);

        // Sanity: the player actually moved; doubled config went at
        // least 30 % further; mode is Running (not Walking).
        EXPECT_GT(base_dist, baseline.walkSpeedMps * 30.0 / 60.0 * 0.5);
        EXPECT_GT(fast_dist, base_dist * 1.3);
        EXPECT_EQ(static_cast<int>(hb.loop.player().mode),
                  static_cast<int>(PlayerMode::Running));
    }

    // --- 3. crouchSpeedMps ---
    // With Ctrl+Forward, mode = CrouchWalk. Halving crouchSpeedMps
    // should reduce displacement.
    {
        MovementConfig baseline = MakeTestConfig();
        baseline.crouchSpeedMps = 0.7f;
        MovementConfig halved = baseline;
        halved.crouchSpeedMps = 0.35f;
        Harness h2;
        PressKey(h2.input, WalkercoreKey::Crouch);
        PressKey(h2.input, WalkercoreKey::Forward);
        h2.loop.SetMovementConfig(baseline);
        h2.RunTicks(30);
        const double base_dist = fabs(h2.loop.player().feet_position.z);
        Harness h3;
        PressKey(h3.input, WalkercoreKey::Crouch);
        PressKey(h3.input, WalkercoreKey::Forward);
        h3.loop.SetMovementConfig(halved);
        h3.RunTicks(30);
        const double fast_dist = fabs(h3.loop.player().feet_position.z);
        EXPECT_GT(base_dist, 0.1);
        EXPECT_LT(fast_dist, base_dist * 0.7);
        EXPECT_EQ(static_cast<int>(h2.loop.player().mode),
                  static_cast<int>(PlayerMode::CrouchWalk));
    }

    // --- 4. jumpInitialVelocityMps ---
    // Press Jump once, observe peak feet.y over 30 ticks. Higher
    // initial velocity → higher peak.
    {
        MovementConfig baseline = MakeTestConfig();
        baseline.jumpInitialVelocityMps = 4.5f;
        baseline.gravityMps2 = 1.0f;  // gentle gravity so peak is reachable
        MovementConfig higher = baseline;
        higher.jumpInitialVelocityMps = 9.0f;
        // ScenarioWithJump leaves the Jump key pressed for the whole
        // test; ground stays detached after the first bump because
        // PhaseIntegrateMovement blocks re-jumps while airborne.
        Result base = ScenarioWithJump(baseline, 30);
        Result peak = ScenarioWithJump(higher, 30);
        EXPECT_GT(base.feet.y, 0.0);  // at least got off the ground
        EXPECT_GT(peak.feet.y, base.feet.y);
    }

    // --- 5. gravityMps2 ---
    // Press Jump once. With low gravity, feet.y stays high; with high
    // gravity, feet.y is forced back down faster.
    {
        MovementConfig gentle = MakeTestConfig();
        gentle.jumpInitialVelocityMps = 6.0f;
        gentle.gravityMps2 = 2.0f;
        MovementConfig harsh = gentle;
        harsh.gravityMps2 = 50.0f;
        Result light = ScenarioWithJump(gentle, 30);
        Result heavy = ScenarioWithJump(harsh, 30);
        EXPECT_GT(light.feet.y, heavy.feet.y);
    }

    // --- 6. mouseSensitivityRadPerUnit ---
    // Set mouse_dx = 100 on the first sample. PhaseApplyMouseLook
    // integrates it. Halving sensitivity should halve the yaw delta
    // from initial (which is 0).
    {
        MovementConfig baseline = MakeTestConfig();
        baseline.mouseSensitivityRadPerUnit = 0.0022f;
        MovementConfig halved = baseline;
        halved.mouseSensitivityRadPerUnit = 0.0011f;
        Result base = ScenarioWithMouseDelta(baseline, 100, 0);
        Result low = ScenarioWithMouseDelta(halved, 100, 0);
        EXPECT_LT(base.yaw, 0.0f);  // yaw -= dx * sens; dx positive → yaw negative
        EXPECT_NEAR(low.yaw, base.yaw * 0.5, kEpsSmall);
    }

    // --- 7. eyeHeightStandingM ---
    // With no crouch and no move, eye.y == feet.y + eye_height.
    // Doubling eyeHeightStandingM should almost double eye.y above feet.
    {
        MovementConfig baseline = MakeTestConfig();
        baseline.eyeHeightStandingM = 1.65f;
        MovementConfig doubled = baseline;
        doubled.eyeHeightStandingM = 3.0f;
        Result base = ScenarioWith(baseline, WalkercoreKey::LastKey, 5);
        Result tall = ScenarioWith(doubled, WalkercoreKey::LastKey, 5);
        const double base_above = base.eye.y - base.feet.y;
        const double tall_above = tall.eye.y - tall.feet.y;
        EXPECT_NEAR(base_above, 1.65, 0.05);
        EXPECT_NEAR(tall_above, 3.0, 0.05);
    }

    // --- 8. eyeHeightCrouchingM ---
    // Hold Ctrl to force crouch target. After enough ticks, eye_height
    // converges toward eyeHeightCrouchingM.
    {
        MovementConfig baseline = MakeTestConfig();
        baseline.eyeHeightCrouchingM = 0.85f;
        baseline.crouchBlendMps = 1000.0f;  // fast blend so we measure target, not transition
        MovementConfig lower = baseline;
        lower.eyeHeightCrouchingM = 0.4f;
        Harness h2;
        PressKey(h2.input, WalkercoreKey::Crouch);
        h2.loop.SetMovementConfig(baseline);
        h2.RunTicks(30);
        const float base_h = h2.loop.player().eye_height;
        Harness h3;
        PressKey(h3.input, WalkercoreKey::Crouch);
        h3.loop.SetMovementConfig(lower);
        h3.RunTicks(30);
        const float low_h = h3.loop.player().eye_height;
        EXPECT_NEAR(base_h, 0.85f, 0.05f);
        EXPECT_NEAR(low_h, 0.4f, 0.05f);
        EXPECT_LT(low_h, base_h);
    }

    // --- 9. crouchBlendMps ---
    // With Ctrl held, eye_height converges to eyeHeightCrouchingM at a
    // rate of crouchBlendMps m/s. Slow blend → eye_height is mid-way
    // after 5 ticks; fast blend → already at target.
    {
        MovementConfig slow;
        slow.crouchBlendMps = 1.0f;       // 1 m/s → in 5 ticks @ 60Hz it'd move 5/60 m
        slow.eyeHeightCrouchingM = 0.0f;  // forces a clear from 1.65 default
        MovementConfig fast = slow;
        fast.crouchBlendMps = 100.0f;     // effectively instant
        Harness h2;
        PressKey(h2.input, WalkercoreKey::Crouch);
        h2.loop.SetMovementConfig(slow);
        h2.RunTicks(5);
        const float slow_h = h2.loop.player().eye_height;
        Harness h3;
        PressKey(h3.input, WalkercoreKey::Crouch);
        h3.loop.SetMovementConfig(fast);
        h3.RunTicks(5);
        const float fast_h = h3.loop.player().eye_height;
        EXPECT_LT(slow_h, 1.65f);
        EXPECT_LT(fast_h, slow_h);  // fast blend went further in the same 5 ticks
    }

    // --- 10. headBobVerticalAmpM ---
    // With Forward held, observe the peak-to-trough range of eye_height
    // over 60 ticks. Doubling the amplitude should approximately
    // double the envelope.
    {
        MovementConfig baseline = MakeTestConfig();
        baseline.headBobVerticalAmpM = 0.025f;
        MovementConfig doubled = baseline;
        doubled.headBobVerticalAmpM = 0.050f;
        const double base_env = BobEnvelope(baseline, WalkercoreKey::Forward, 60);
        const double tall_env = BobEnvelope(doubled, WalkercoreKey::Forward, 60);
        EXPECT_GT(base_env, 1e-4);  // some bob happened
        EXPECT_GT(tall_env, base_env * 1.3);
    }

    // --- 11. headBobLateralAmpM ---
    // Same idea: observe lateral bob. PhaseAnimatePlayer applies this
    // via lateral_bob_m_ on top of camera.eye_position.x. We compare
    // peak |x|; doubled amplitude should reach ~2× peak.
    {
        MovementConfig baseline = MakeTestConfig();
        baseline.headBobLateralAmpM = 0.020f;
        MovementConfig doubled = baseline;
        doubled.headBobLateralAmpM = 0.040f;
        Harness h2;
        PressKey(h2.input, WalkercoreKey::Forward);
        h2.loop.SetMovementConfig(baseline);
        float x_max_base = 0.0f;
        for (int i = 0; i < 60; ++i) {
            h2.loop.RunOnce(1.0 / 60.0);
            const float ax = fabsf(h2.loop.camera().eye_position.x);
            if (ax > x_max_base) x_max_base = ax;
        }
        Harness h3;
        PressKey(h3.input, WalkercoreKey::Forward);
        h3.loop.SetMovementConfig(doubled);
        float x_max_tall = 0.0f;
        for (int i = 0; i < 60; ++i) {
            h3.loop.RunOnce(1.0 / 60.0);
            const float ax = fabsf(h3.loop.camera().eye_position.x);
            if (ax > x_max_tall) x_max_tall = ax;
        }
        EXPECT_GT(static_cast<double>(x_max_base), 1e-4);
        EXPECT_GT(static_cast<double>(x_max_tall),
                  static_cast<double>(x_max_base) * 1.3);
    }

    // --- 12. headBobFrequencyHz ---
    // Two configs, one slow one fast. Count zero-crossings of the
    // lateral bob over a fixed window — fast frequency should cross
    // (return-to-zero) more times than the slow one.
    {
        MovementConfig slow = MakeTestConfig();
        slow.headBobFrequencyHz = 0.5f;
        MovementConfig fast = slow;
        fast.headBobFrequencyHz = 5.0f;
        const int slow_cross = LateralBobZeroCrossings(slow, 90);
        const int fast_cross = LateralBobZeroCrossings(fast, 90);
        EXPECT_GT(slow_cross, 0);
        EXPECT_GT(fast_cross, slow_cross);
    }

    // --- 13. maxStepUpM (NO consumer yet — honest probe) ---
    // Currently maxStepUpM is declared in MovementConfig but no code
    // path reads it. PhaseResolveCollision uses a hard-coded 0.35 m
    // sphere radius. Varying the field should produce identical
    // observable results with everything else equal. If a future M3
    // lander hooks this field into PhaseResolveCollision, this assertion
    // breaks — that is the explicit signal to move it out of the
    // stub-list in this file.
    {
        MovementConfig tiny = MakeTestConfig();
        tiny.maxStepUpM = 0.001f;
        MovementConfig giant = MakeTestConfig();
        giant.maxStepUpM = 100.0f;
        Result a = ScenarioWith(tiny, WalkercoreKey::Forward, 30);
        Result b = ScenarioWith(giant, WalkercoreKey::Forward, 30);
        EXPECT_NEAR(a.feet.x, b.feet.x, kEpsSmall);
        EXPECT_NEAR(a.feet.y, b.feet.y, kEpsSmall);
        EXPECT_NEAR(a.feet.z, b.feet.z, kEpsSmall);
        EXPECT_NEAR(a.eye.x, b.eye.x, kEpsSmall);
        EXPECT_NEAR(a.eye.y, b.eye.y, kEpsSmall);
        EXPECT_NEAR(a.eye.z, b.eye.z, kEpsSmall);
    }

    // --- 14. maxSlopeAngleRad (NO consumer yet — honest probe) ---
    // Same as #13. No code path reads maxSlopeAngleRad yet. Varying
    // it must not change any observable.
    {
        MovementConfig zero = MakeTestConfig();
        zero.maxSlopeAngleRad = 0.0f;
        MovementConfig pi_by_2 = MakeTestConfig();
        pi_by_2.maxSlopeAngleRad = 1.5707963f;  // pi/2
        Result a = ScenarioWith(zero, WalkercoreKey::Forward, 30);
        Result b = ScenarioWith(pi_by_2, WalkercoreKey::Forward, 30);
        EXPECT_NEAR(a.feet.x, b.feet.x, kEpsSmall);
        EXPECT_NEAR(a.feet.y, b.feet.y, kEpsSmall);
        EXPECT_NEAR(a.feet.z, b.feet.z, kEpsSmall);
        EXPECT_NEAR(a.eye.x, b.eye.x, kEpsSmall);
        EXPECT_NEAR(a.eye.y, b.eye.y, kEpsSmall);
        EXPECT_NEAR(a.eye.z, b.eye.z, kEpsSmall);
    }
}

}  // namespace walkercore
