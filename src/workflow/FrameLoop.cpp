// WalkerCore — FrameLoop workflow implementation.
//
// M4 scope: complete per-frame orchestration. Each phase does real work
// against the bound providers; the stubs from M2 have been retired.
//
// Phases (RunOnce):
//   1. PhaseSampleInput         — pull InputHook sample, consume toggle
//   2. PhaseApplyMouseLook      — yaw/pitch from accumulated mouse delta
//   3. PhaseIntegrateMovement   — desired velocity → proposed position
//   4. PhaseResolveCollision    — sphere-sweep with axis-by-axis sliding
//   5. PhaseAnimatePlayer       — head bob, mode transitions, eye height
//   6. PhasePushCameraToRenderer — placeholder for D3D11 hook consumer

#include "walkercore/FrameLoop.h"

#include <math.h>

#include "walkercore/InputHook.h"
#include "walkercore/Log.h"
#include "walkercore/Time.h"
#include "walkercore/TruckState.h"
#include "walkercore/WalkerFsm.h"
#include "walkercore/WorldState.h"

namespace walkercore {

namespace {
// 2π with double precision, used by the head-bob phase accumulator
// in PhaseAnimatePlayer. The literal would otherwise appear 3 times
// in the wrap logic (increment, threshold, decrement) — naming the
// constant removes the per-call magic-number repetition and matches
// the kTwoPi pattern used in WalkerFsm.cpp and FuelPump.cpp.
constexpr double kTwoPi = 6.28318530717958647692;
}  // namespace

FrameLoop::FrameLoop() {
    PlayerState_Reset(player_);
    CameraState_Reset(camera_);
    camera_.aspect = 16.0f / 9.0f;  // sensible default; M2 reads real size
    // Reset camera_override_ too so its precomputed_yaw_trig_valid_ (and
    // every other field) starts in a known-good state. The override path
    // copies camera_override_ into camera_ on a state-change frame; if
    // camera_override_ were left uninitialized (the struct's POD default
    // is undefined), the assignment could leak an arbitrary
    // precomputed_yaw_trig_valid_ into camera_, which CameraState_Update
    // then honours as "cache hit" with whatever stale sin/cos happened
    // to sit in those bytes. CameraState_Reset sets the valid bit to
    // false (its freshly-cleared state), so BuildViewRotation recomputes
    // fresh on the override frame, which is exactly what we want.
    CameraState_Reset(camera_override_);
}

void FrameLoop::BindInput(InputHook *input) {
    input_ = input;
}

void FrameLoop::BindWorld(IWorldGeometry *world) {
    world_ = world;
}

void FrameLoop::BindFsm(WalkerFsm *fsm) {
    fsm_ = fsm;
}

void FrameLoop::SetMovementConfig(const MovementConfig &cfg) {
    movement_cfg_ = cfg;
}

void FrameLoop::SetAspect(float aspect) {
    if (aspect > 0.0f && aspect != camera_.aspect) {
        camera_.aspect = aspect;
        // Mark the projection cache dirty so the next RunOnce() rebuilds
        // projection[] with the new aspect. The cache also detects the
        // change via the field comparison inside CameraState_Update, but
        // setting the flag explicitly makes the intent obvious and saves a
        // couple of nanoseconds on the resize frame.
        //
        // The aspect != camera_.aspect guard is the "no-op when unchanged"
        // contract: a caller (D3D11Hook, INI reload, test fixture) that
        // hands in the current aspect must NOT dirty the cache, otherwise
        // we rebuild projection every frame for no reason. The
        // TestFrameLoopSetAspectIsNoopWhenUnchanged probe pins this
        // contract.
        camera_.projection_dirty_ = true;
    }
}

void FrameLoop::OverrideCamera(const CameraState &c) {
    camera_override_ = c;
    camera_override_pending_ = true;
}

void FrameLoop::SampleInput() {
    // The canonical frame sample. Pull the accumulated input snapshot
    // into last_input_ and clear the source's edge bits so the next
    // frame starts with a fresh accumulator. Mark the frame as sampled
    // so RunOnce()'s internal fallback sample (used by callers that
    // don't go through the per-frame conductor) becomes a no-op.
    PhaseSampleInput();
    input_sampled_this_frame_ = true;
}

void FrameLoop::RunOnce(double dt_seconds) {
    // Phase 1 — sample input. The per-frame conductor (walker_per_frame_tick)
    // calls SampleInput() immediately before RunOnce() so the FSM can
    // observe rising edges (F10 toggle-walk, F interact) on the same
    // frame the user pressed them, and so the downstream providers
    // (fuel / coupling / flashlight) read the cached `in` reference on
    // the same frame.
    //
    // If the conductor already sampled this frame, skip the internal
    // sample: re-running PhaseSampleInput would re-copy the now edge-
    // cleared source into last_input_ and wipe the rising edge the
    // downstream providers need to see. The input_sampled_this_frame_
    // flag is reset at the end of RunOnce() so the next frame's
    // SampleInput / RunOnce pair re-arms it.
    //
    // For callers that drive RunOnce() directly without going through
    // the per-frame conductor (e.g. the unit-test harness, which calls
    // RunOnce in a tight loop with pre-pressed keys), the flag is
    // false and RunOnce() does the canonical sample itself. The held
    // bit survives across frames because the & 0x01 clear-on-sample
    // preserves it; only the edge bits get cleared.
    if (!input_sampled_this_frame_) {
        PhaseSampleInput();
    }
    input_sampled_this_frame_ = false;

    if (fsm_ == nullptr || fsm_->state() != WalkerFsmState::Walking) {
        // Reset frame-time tracking so the next RunOnce() in Walking mode
        // doesn't see a multi-second dt on its first call (which would
        // launch the player into orbit on the very first frame).
        last_frame_us_ = 0;
        return;
    }

    // Resolve dt. The per-frame conductor (walker_per_frame_tick) passes
    // the dt it measured between two Present calls; tests that don't have
    // such a caller pass dt_seconds < 0 and we fall back to the wall
    // clock. The wall clock fallback produces a dt = 0 on the very first
    // frame after entering Walking (no baseline to subtract from).
    double dt = dt_seconds;
    if (dt < 0.0) {
        const uint64_t now_us = NowMicros();
        dt = (last_frame_us_ == 0) ? 0.0
                                    : DeltaSeconds(now_us, last_frame_us_);
        last_frame_us_ = now_us;
    }

    // Phase 2 — apply mouse delta to yaw/pitch.
    PhaseApplyMouseLook();

    // Phase 3 — integrate movement (proposed displacement only).
    PhaseIntegrateMovement(dt);

    // Phase 4 — resolve collision (apply proposed displacement with sliding).
    PhaseResolveCollision();

    // Phase 5 — animate (mode, eye height, head bob).
    PhaseAnimatePlayer(dt);

    // Phase 6 — adopt player transform into camera, apply lateral bob,
    // recompute matrices. If a transition (M5) supplied an override this
    // frame, use that instead.
    if (camera_override_pending_) {
        // Override path: the camera's yaw comes from camera_override_,
        // not from the player, so the precomputed sin/cos of player.yaw
        // do not apply. The cache fields are zeroed explicitly so a
        // future refactor that fires OverrideCamera with a target built
        // without CameraState_Reset (current production calls the FSM
        // with a stack `CameraState target;`) can't feed garbage
        // precomputed_yaw_trig_valid_ into BuildViewRotation. We also
        // copy the override-relevant fields (eye_position, yaw,
        // pitch) by hand instead of using camera_ = camera_override_
        // — the latter would clobber camera_.aspect with whatever
        // bytes the FSM's uninitialized target happened to have on
        // that stack frame, breaking the projection matrix on the
        // override frame. (This is a pre-existing concern; the FSM's
        // Update writes eye/yaw/pitch but not aspect/near/far/fov,
        // and OverrideCamera copies the entire CameraState. Field-
        // by-field copy keeps the projection inputs intact.)
        camera_.eye_position = camera_override_.eye_position;
        camera_.yaw_radians = camera_override_.yaw_radians;
        camera_.pitch_radians = camera_override_.pitch_radians;
        // Force BuildViewRotation's rotation cache miss on the override
        // frame (yaw / pitch may have moved). The precomputed_yaw_trig_valid_
        // is invalidated so BuildViewRotation recomputes sin/cos fresh.
        camera_.view_rotation_dirty_ = true;
        camera_.precomputed_yaw_trig_valid_ = false;
        camera_override_pending_ = false;
        CameraState_Update(camera_);
    } else {
        CameraState_AdoptPlayer(camera_, player_);
        camera_.eye_position.x += lateral_bob_m_;
        // Forward the cached sin/cos from PhaseApplyMouseLook into the
        // CameraState so BuildViewRotation's cache-miss path can skip
        // the local sinf/cosf for yaw (pitch still computes locally —
        // its trig is only used here). The cache claim stays valid for
        // the rest of this RunOnce; RunOnce is the only mutator of
        // camera_.yaw_radians between this write and the next
        // RunOnce's PhaseApplyMouseLook.
        camera_.precomputed_sin_yaw_ = cached_sin_yaw_;
        camera_.precomputed_cos_yaw_ = cached_cos_yaw_;
        camera_.precomputed_yaw_trig_valid_ = true;
        CameraState_Update(camera_);
    }

    // Phase 7 — push to renderer. Stub until D3D11 hook lands.
    PhasePushCameraToRenderer();
}

void FrameLoop::PhaseSampleInput() {
    if (input_ == nullptr) {
        InputState_Reset(last_input_);
        return;
    }
    // Sample the current keyboard + mouse state into last_input_. Rising
    // edges (IsKeyPressed) are set by the WH_KEYBOARD_LL callback in
    // InputHook and are visible on the very next Sample() — the WalkerFsm
    // reads the rising edge directly from last_input_ each frame, no
    // explicit consume step needed.
    input_->Sample(last_input_);
}

void FrameLoop::PhaseApplyMouseLook() {
    // Sensitivity in config is "radians per raw input unit". Each unit is
    // roughly 1/1200 of an 800 DPI mouse sweep; the default gives ~0.13 rad
    // per inch of mouse movement, close to TMRealWalk's published value.
    //
    // Sign convention (D3D view space used by CameraState_BuildViewMatrix):
    //   yaw=0     → forward=(0, 0, -1), camera looks -Z
    //   yaw=+π/2  → forward=(1, 0, 0),  camera turns CCW from above
    //   pitch=+π/2→ forward.y=+1,       camera looks straight up
    //
    // Raw input: mouse_dx>0 means mouse moved right (turning the player
    // right means turning the camera CW from above, which is yaw -= dx).
    // mouse_dy>0 means mouse moved DOWN (looking down means pitch -= dy).
    const float sens = movement_cfg_.mouseSensitivityRadPerUnit;
    player_.yaw_radians -= static_cast<float>(last_input_.mouse_dx) * sens;
    player_.pitch_radians -= static_cast<float>(last_input_.mouse_dy) * sens;
    PlayerState_ClampPitch(player_);

    // Compute sin(yaw)/cos(yaw) once and cache them on the FrameLoop.
    // PhaseIntegrateMovement reads the cache instead of recomputing
    // (saves ~20-40 ns/frame in Walking mode — the sinf+cosf pair is
    // each ~10-20 ns on x64). The same pair is also forwarded into
    // camera_.precomputed_sin_yaw_ / camera_.precomputed_cos_yaw_ in
    // the Phase 6 AdoptPlayer path so the CameraState's
    // BuildViewRotation block also skips the trig when the rotation
    // cache misses (this is the second consumer; sharing means we
    // compute these values exactly once per RunOnce in Walking mode).
    cached_sin_yaw_ = sinf(player_.yaw_radians);
    cached_cos_yaw_ = cosf(player_.yaw_radians);
}

void FrameLoop::PhaseIntegrateMovement(double dt) {
    if (dt <= 0.0) {
        // Still record the pre-integration pos so the collision phase (next
        // frame) has a reference. Mode update below still works.
        pre_integration_pos_ = player_.feet_position;
        return;
    }

    // Determine desired horizontal speed from input + crouch/run modifiers.
    const bool run = IsKeyHeld(last_input_, WalkercoreKey::Run);
    const bool crouch = IsKeyHeld(last_input_, WalkercoreKey::Crouch);
    float speed = movement_cfg_.walkSpeedMps;
    if (crouch) {
        speed = movement_cfg_.crouchSpeedMps;
    } else if (run) {
        speed = movement_cfg_.runSpeedMps;
    }

    // Compute intent vector in the player's local horizontal plane.
    // Forward = (sin(yaw), 0, cos(yaw)) (yaw=0 looks toward +Z, +X is east).
    // Note that D3D view uses (-cos(yaw)*cos(pitch)) for forward.z but for
    // ground-plane motion we want the camera-aligned forward projected to
    // the ground, which is (sin(yaw), 0, cos(yaw)).
    float forward_intent = 0.0f;
    float strafe_intent = 0.0f;
    if (IsKeyHeld(last_input_, WalkercoreKey::Forward)) forward_intent += 1.0f;
    if (IsKeyHeld(last_input_, WalkercoreKey::Backward)) forward_intent -= 1.0f;
    if (IsKeyHeld(last_input_, WalkercoreKey::StrafeRight)) strafe_intent += 1.0f;
    if (IsKeyHeld(last_input_, WalkercoreKey::StrafeLeft)) strafe_intent -= 1.0f;

    // Normalize so diagonal movement isn't faster. Each input component is
    // -1, 0, or +1, so |intent| is either 0 (no keys), 1 (one key), or
    // sqrt(2) (one forward/back key AND one strafe key). The sqrt is only
    // needed in the diagonal case; comparing the squared magnitude
    // against 1.0 lets us skip the transcendental call in the common
    // single-key path (which is most walking frames). sqrtf is ~10-30
    // ns on x64; saving it on every forward-only / strafe-only / idle
    // frame adds up. TestFrameLoopDiagonalMotionIsSpeedBounded pins the
    // diagonal case (intents still sum to walkSpeed after normalisation);
    // TestPhaseIntegrateMovementSkipsSqrtWhenIntentIsOnAxis pins the
    // single-key case (the no-sqrt contract).
    const float intent_mag_sq = forward_intent * forward_intent +
                                strafe_intent * strafe_intent;
    if (intent_mag_sq > 1.0f) {
        const float intent_mag = sqrtf(intent_mag_sq);
        forward_intent /= intent_mag;
        strafe_intent /= intent_mag;
    }

    // Pre-compute the squared intent for the mode check below. The
    // |intent| < 0.01 test ("is the player trying to move?") can be
    // expressed equivalently as |intent|² < 0.0001 — we don't need the
    // actual magnitude, just the "near zero" decision. This keeps the
    // no-sqrt contract complete: the per-frame sqrt is now reserved
    // for the diagonal case where the normalised velocity actually
    // requires it.

    // sin(yaw) / cos(yaw) computed once in PhaseApplyMouseLook and
    // cached on the FrameLoop. Reading from the cache skips the local
    // sinf/cosf pair (each ~10-20 ns on x64). The cache is fresh
    // because PhaseApplyMouseLook always runs at the start of
    // RunOnce, immediately before this phase.
    const float cos_yaw = cached_cos_yaw_;
    const float sin_yaw = cached_sin_yaw_;

    // World-space velocity on the horizontal plane.
    const float vx = (sin_yaw * forward_intent + cos_yaw * strafe_intent) * speed;
    const float vz = (cos_yaw * forward_intent - sin_yaw * strafe_intent) * speed;

    // Apply gravity / jump.
    const bool jump_pressed = IsKeyPressed(last_input_, WalkercoreKey::Jump);
    if (jump_pressed && grounded_ && !crouch) {
        vertical_velocity_ = static_cast<double>(movement_cfg_.jumpInitialVelocityMps);
        grounded_ = false;
    }
    vertical_velocity_ -= static_cast<double>(movement_cfg_.gravityMps2) * dt;

    // Cap |vertical_velocity_| to MovementConfig::vertical_velocity_max_mps.
    // The cap is applied AFTER the gravity step so legitimate short-
    // duration gravity runs (a fresh jump that falls back, a one- or
    // two-second freefall before re-entry) pass through unaffected —
    // only the unbounded-accumulation case (multiple seconds of Walking
    // without a collision-resolution floor) is intercepted. The clamp
    // happens before the y integration below so the per-frame |dy| the
    // downstream KnockoutProvider reads through player_.feet_position is
    // guaranteed to be at most max × dt ≤ 0.167 m. See MovementConfig
    // for the rationale; this single comparison pair is the cheapest
    // fix to the post-transition spurious-knockout problem without
    // requiring a working collision engine.
    const double vy_max =
        static_cast<double>(movement_cfg_.vertical_velocity_max_mps);
    const double vy_before_cap = vertical_velocity_;
    if (vertical_velocity_ > vy_max) {
        vertical_velocity_ = vy_max;
    } else if (vertical_velocity_ < -vy_max) {
        vertical_velocity_ = -vy_max;
    }
    // DIAGNOSTIC (M11 follow-up #2): emit one log line per Walking
    // tick so we can pin the actual vy / dt / dy values the
    // KnockoutProvider sees. The game log on ATS 1.61.3.1s shows KO
    // triggers with dy ≈ 0.2 m at dt ≈ 0.007 s — that's velocity ≈ 28
    // m/s, well above the 9.81 m/s cap. Either vy is being set
    // higher than the cap elsewhere, or PhaseIntegrateMovement is
    // receiving a different dt than KnockoutProvider::Tick. The log
    // resolves which. Rate-limited via a tick counter so we don't
    // flood the game log at 110-200 Hz.
    static uint64_t s_diag_tick = 0;
    if ((s_diag_tick++ % 30) == 0) {
        WALKERCORE_LOG_INFO(
            "FrameLoop::PhaseIntegrateMovement: dt=%.4f, vy=%.3f "
            "(pre-cap=%g), vy_max=%.3f, vy*dt=%.4f m",
            dt, vertical_velocity_, vy_before_cap, vy_max,
            vertical_velocity_ * dt);
    }

    // Capture pre-integration position for the collision phase.
    pre_integration_pos_ = player_.feet_position;

    // Integrate. PhaseResolveCollision sweeps from pre_integration_pos_ to
    // player_.feet_position (the proposed new position) and applies sliding.
    player_.feet_position.x += static_cast<double>(vx) * dt;
    player_.feet_position.z += static_cast<double>(vz) * dt;
    player_.feet_position.y += static_cast<double>(vertical_velocity_) * dt;

    // Hard floor clamp. Until M3 lander ships real PhysX ground collision
    // (PxScene::sphereCast live), PhaseResolveCollision's vertical sweep
    // returns no-hit every frame and the integrator would otherwise drop
    // the player to Y = -∞. With the floor, the player lands on the
    // floor every frame the integrator tries to go below, vertical
    // velocity zeroes, and grounded_ flips to true so the bob / jump
    // gates see the player as standing on something. The floor is
    // refreshed each frame by walker_per_frame_tick from the truck's
    // placement Y (so the player walks around at truck ground level),
    // and defaults to 0 (sea level) before the first telemetry update.
    // This is the "salir del vehículo" floor: when the user F10s out
    // of the cabin, the integrator can no longer send the player's
    // POV below the truck's wheels, so the SCS developer free camera
    // the GameFreeCameraShim dispatches has a usable eye Y.
    if (player_.feet_position.y < floor_y_) {
        player_.feet_position.y = floor_y_;
        vertical_velocity_ = 0.0;
        grounded_ = true;
    }

    // Track mode for head bob selection (running uses less bob).
    // See the comment above the intent_mag_sq computation: the
    // "near-zero" decision is on the squared magnitude, avoiding the
    // sqrt entirely. 0.01² = 0.0001.
    if (intent_mag_sq < 0.0001f) {
        if (crouch) player_.mode = PlayerMode::CrouchIdle;
        else player_.mode = PlayerMode::Idle;
    } else {
        if (crouch) player_.mode = PlayerMode::CrouchWalk;
        else if (run) player_.mode = PlayerMode::Running;
        else player_.mode = PlayerMode::Walking;
    }
}

void FrameLoop::PhaseResolveCollision() {
    if (world_ == nullptr) {
        return;  // no collision configured; player passes through
    }

    // Sphere-sweep along each axis independently so the player slides along
    // walls instead of getting stuck. We use pre_integration_pos_ as the
    // sweep origin (set by PhaseIntegrateMovement) and player_.feet_position
    // as the proposed destination.
    const double radius = 0.35;  // capsule radius — matches M3 config
    const Vec3 &origin = pre_integration_pos_;
    Vec3 proposed = player_.feet_position;

    // Horizontal (X + Z together). One sweep is enough at our speed caps.
    const double dx = proposed.x - origin.x;
    const double dz = proposed.z - origin.z;
    if (fabs(dx) > 1e-6 || fabs(dz) > 1e-6) {
        Vec3 disp{dx, 0.0, dz};
        MoveResult r = world_->SphereSweep(origin, radius, disp);
        proposed.x = r.resolved_position.x;
        proposed.z = r.resolved_position.z;
    }

    // Vertical — independent sweep so gravity/jump resolve separately from
    // horizontal collision.
    const double dy = proposed.y - origin.y;
    if (fabs(dy) > 1e-6) {
        Vec3 vdisp{0.0, dy, 0.0};
        Vec3 vorigin{proposed.x, origin.y, proposed.z};
        MoveResult r = world_->SphereSweep(vorigin, radius, vdisp);
        if (r.hit) {
            if (vertical_velocity_ <= 0.0) {
                // Landed.
                proposed.y = r.resolved_position.y;
                vertical_velocity_ = 0.0;
                grounded_ = true;
            } else {
                // Bonked head (rare without a ceiling mesh).
                proposed.y = r.resolved_position.y;
                vertical_velocity_ = 0.0;
                grounded_ = false;
            }
        } else {
            grounded_ = false;
        }
    }

    player_.feet_position = proposed;
}

void FrameLoop::PhaseAnimatePlayer(double dt) {
    // Eye height transitions (crouch).
    const bool crouch = IsKeyHeld(last_input_, WalkercoreKey::Crouch);
    const float target = crouch ? movement_cfg_.eyeHeightCrouchingM
                                : movement_cfg_.eyeHeightStandingM;
    const float blend = movement_cfg_.crouchBlendMps * static_cast<float>(dt);
    if (player_.eye_height < target) {
        player_.eye_height += blend;
        if (player_.eye_height > target) player_.eye_height = target;
    } else if (player_.eye_height > target) {
        player_.eye_height -= blend;
        if (player_.eye_height < target) player_.eye_height = target;
    }

    // Head bob: phase accumulates at (frequency * 2π) per second of walking.
    // We only bob when grounded + actually moving horizontally.
    const bool moving = (player_.mode == PlayerMode::Walking) ||
                        (player_.mode == PlayerMode::Running) ||
                        (player_.mode == PlayerMode::CrouchWalk);
    if (moving && grounded_) {
        // Running bob is 1.6× the walking bob frequency. The literal is float to
    // avoid an implicit double-promotion in the ternary result (the
    // branches need to be the same type or we get -Wdouble-promotion).
    const double freq = (player_.mode == PlayerMode::Running)
                                ? static_cast<double>(movement_cfg_.headBobFrequencyHz) * 1.6
                                : static_cast<double>(movement_cfg_.headBobFrequencyHz);
        // Wrap the phase continuously by [0, 2π) on every increment,
        // rather than running a rare-event fmod at 1e6. The fmod path
        // (a) spends hundreds of ns when it finally fires (a multi-
        // hour-running session), and (b) loses float32 precision in
        // the argument before the modulo — at head_bob_phase_ ~ 1e6
        // only ~7 significant digits remain, so the reduced angle can
        // be off by a few percent. Continuous wrap keeps phase
        // bounded at all times so std::sin / std::cos downstream are
        // always evaluated in their accurate-input range, and the
        // cost is one compare-and-subtract per frame (~1 ns)
        // regardless of how long the session has been running.
        head_bob_phase_ += kTwoPi * freq * dt;
        if (head_bob_phase_ >= kTwoPi) {
            head_bob_phase_ -= kTwoPi;
        }
    } else {
        // Decay bob back to zero so the player doesn't drift.
        head_bob_phase_ *= 0.9;
    }

    // Apply bob to a temporary offset that the camera eye position will pick
    // up. We store it in player_.eye_height by adding the bob on top of the
    // standing eye height. Sideways bob goes into the horizontal eye offset
    // by adjusting the camera (not player) so ground collision isn't shaken.
    //
    // Sin/cos short-circuit: when `moving` is false the result is
    // multiplied by 0 anyway, so we skip the transcendental calls.
    // std::sin/std::cos are tens of ns on x64 (and hundreds on slow CI
    // VMs), and the per-frame loop hits PhaseAnimatePlayer every walking
    // frame, so a not-moving player (the common case when the user
    // pauses to look at a building, the truck, or simply stands still)
    // was paying for work whose result was discarded. The test
    // TestPhaseAnimatePlayerSkipsSinCosWhenNotMoving pins the
    // "vertical_bob == 0 && lateral_bob == 0" contract for that
    // branch.
    //
    // Note: when moving but not grounded (jumping / falling), we still
    // pay the sin/cos cost because the original behaviour applied a
    // non-zero bob with the decayed phase. The cached `moving` flag
    // is exactly what the previous `(moving ? 1.0 : 0.0)` multiplier
    // was using, so the steady-state and jump-state branches are
    // equivalent to the pre-optimisation code path.
    float vertical_bob = 0.0f;
    float lateral_bob = 0.0f;
    if (moving) {
        vertical_bob = static_cast<float>(std::sin(head_bob_phase_) *
                                           static_cast<double>(movement_cfg_.headBobVerticalAmpM));
        lateral_bob = static_cast<float>(std::cos(head_bob_phase_ * 0.5) *
                                          static_cast<double>(movement_cfg_.headBobLateralAmpM));
    }

    // Stash the bob for the camera-layer to apply after AdoptPlayer. We
    // encode the vertical bob into eye_height (which CameraState_AdoptPlayer
    // adds to feet_position), and remember the lateral bob to add on top
    // of the camera's eye position once it's derived.
    player_.eye_height += vertical_bob;
    lateral_bob_m_ = lateral_bob;
}

void FrameLoop::PhasePushCameraToRenderer() {
    // M2/M4: the D3D11 hook reads camera_.view / camera_.projection each
    // Present. The hook is bound in D3D11Hook::Install() during M2 startup.
    // For M4 without D3D11Hook compiled in, this is a no-op.
}

}  // namespace walkercore