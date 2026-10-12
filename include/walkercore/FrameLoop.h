// WalkerCore — FrameLoop workflow.
//
// The FrameLoop is the per-frame conductor. It owns the high-level state
// (PlayerState + CameraState) and pulls fresh data from the providers
// (ScsTelemetryProvider, InputHook, IWorldGeometry) to update them.
//
// Frame lifecycle (one iteration of RunOnce()):
//   1. Snapshot TruckState from SCS (already populated by callbacks).
//   2. Sample input from InputHook; check toggle-walk-request.
//   3. If we're not in WALKING mode, do nothing — the game's own camera
//      stays in charge.
//   4. Apply mouse delta to player.yaw/pitch.
//   5. Integrate movement (compute desired velocity, integrate over dt).
//   6. Resolve collision (sphere-sweep against PhysX).
//   7. Animate player (head bob, run/crouch mode transitions).
//   8. Update CameraState from PlayerState.
//   9. Push CameraState.view/projection to the render sink (D3D11 hook).
//
// M4 ships the orchestration + input integration; sub-system methods
// delegate to the bound providers and the movement config.

#ifndef WALKERCORE_FRAME_LOOP_H
#define WALKERCORE_FRAME_LOOP_H

#include "walkercore/CameraState.h"
#include "walkercore/InputState.h"
#include "walkercore/MovementConfig.h"
#include "walkercore/PlayerState.h"

namespace walkercore {

class InputHook;
class IWorldGeometry;
class WalkerFsm;

class FrameLoop {
public:
    FrameLoop();

    // Wire dependencies. The FrameLoop does not own them — composition
    // root (PluginEntry / Main) holds the lifetime.
    void BindInput(InputHook *input);
    void BindWorld(IWorldGeometry *world);
    // Bind the WalkerFsm. Required for RunOnce() to do any movement work —
    // the FrameLoop is the on-foot movement integrator, but the FSM owns
    // the decision of whether we are currently on foot at all. A null
    // binding makes RunOnce() a no-op (except for input sampling).
    void BindFsm(WalkerFsm *fsm);

    // Movement / camera configuration. Defaults match TMRealWalk; M6 INI
    // overrides at runtime.
    void SetMovementConfig(const MovementConfig &cfg);
    const MovementConfig &movementConfig() const { return movement_cfg_; }

    // Hard floor for the player's feet position. The integrator clamps
    // player_.feet_position.y to >= floor_y_ every frame so a player who
    // somehow ends up below ground (e.g. the Walking state where M3
    // PhysX is still a stub and no real ground collision resolves) does
    // not fall to -∞. Default 0 (sea level). Updated each frame from
    // the truck's placement Y by walker_per_frame_tick, so the player
    // stays anchored to the truck's ground reference while walking
    // around it. When the truck has no placement yet (early init) the
    // default sea-level floor prevents the integrator from running
    // away during the first few frames.
    void SetFloorY(double floor_y) { floor_y_ = floor_y; }
    double floorY() const { return floor_y_; }

    // Set the current frame's aspect ratio (width / height). D3D11Hook
    // calls this each frame from the swap-chain description.
    void SetAspect(float aspect);

    // Override the camera state from an external source (M5 transitions).
    // The override is consumed once — RunOnce() clears the marker after
    // adopting it. Calling this twice in one frame has the second call
    // win, which is what the M5 transition tweens want.
    void OverrideCamera(const CameraState &c);

    // One frame of work. Called from M2+ D3D11 hook on Present.
    //
    // The FrameLoop does NOT decide whether we are in walk mode — that is
    // the FSM's responsibility. This method samples input only when no
    // one has already sampled it this frame (see SampleInput / the
    // input_sampled_this_frame_ flag below); the per-frame conductor
    // pre-samples so the FSM can read rising edges on the same frame the
    // user pressed them. RunOnce() then runs the on-foot movement phases
    // only when fsm_->state() == Walking. When the FSM is in Driving /
    // TransitionIn / TransitionOut, RunOnce() is a no-op aside from
    // (possibly) the input sample.
    //
    // `dt_seconds < 0` means "compute dt internally from the wall clock"
    // (legacy behaviour, used by tests that don't have a caller). A
    // non-negative dt is used as-is — the per-frame conductor
    // (walker_per_frame_tick) passes the dt it measured between two Present
    // calls, so the FrameLoop integrates with the same frame delta the
    // renderer sees.
    //
    // NOT idempotent within a frame: calling RunOnce() twice in the
    // same frame (without a SampleInput in between) advances the
    // simulation by the dt you pass in twice. Use SampleInput + RunOnce
    // per Present; the conductor pattern is the canonical idiom.
    void RunOnce(double dt_seconds = -1.0);

    // Sample input only — no movement. The per-frame conductor calls
    // this before WalkerFsm::Update so the FSM can read rising edges
    // (toggle-walk, interact) on the same frame the user pressed them,
    // and so the FSM's camera override takes effect on the same frame
    // it is produced. SampleInput is the canonical sample — it copies
    // the InputHook's accumulated snapshot into last_input_ AND clears
    // the source's edge bits so the next frame's events accumulate
    // fresh.
    //
    // RunOnce() also calls PhaseSampleInput internally for callers
    // that don't go through the per-frame conductor (e.g. the unit-test
    // harness drives RunOnce directly). The conductor-path call and
    // the RunOnce-internal call are not both wanted: the second call
    // would re-copy the now edge-cleared source into last_input_ and
    // wipe the rising edge the downstream providers (fuel / coupling /
    // flashlight) need to see on the same frame the user pressed F.
    // SampleInput() sets the input_sampled_this_frame_ flag, and
    // RunOnce() skips its internal sample when that flag is set.
    // Calling SampleInput() immediately before RunOnce() is therefore
    // safe AND load-bearing — it guarantees RunOnce() leaves the
    // last_input_ edge bits alone for the rest of the frame.
    void SampleInput();

    // Read-only views for diagnostics and tests.
    const PlayerState &player() const { return player_; }
    // Mutable accessor — WalkerFsm::BindPlayerTarget needs to write into
    // the player when starting a transition (snap to door / seat).
    PlayerState &player() { return player_; }
    const CameraState &camera() const { return camera_; }
    // Mutable camera — diagnostic tests write a fixture camera so the
    // focused flashlight/head-tracker dumps have something to surface.
    // Production code never needs this path: OverrideCamera is the
    // canonical write path through RunOnce.
    CameraState &camera() { return camera_; }
    const InputState &lastInput() const { return last_input_; }

private:
    // Per-frame phases. They use the bound providers and the input sample
    // captured in PhaseSampleInput().
    void PhaseSampleInput();
    void PhaseApplyMouseLook();
    void PhaseIntegrateMovement(double dt);
    void PhaseResolveCollision();
    void PhaseAnimatePlayer(double dt);
    void PhasePushCameraToRenderer();

    // Dependencies (non-owning).
    InputHook *input_ = nullptr;
    IWorldGeometry *world_ = nullptr;
    WalkerFsm *fsm_ = nullptr;

    // State.
    PlayerState player_;
    CameraState camera_;
    InputState last_input_;
    // Set by SampleInput() so RunOnce() knows the conductor already
    // pulled the input snapshot this frame. RunOnce() resets it at the
    // end so the next frame's SampleInput / RunOnce pair re-arms it.
    // Without this flag, RunOnce() would unconditionally re-sample,
    // overwriting last_input_ with the (now edge-cleared) source and
    // wiping the rising edge the conductor's downstream providers
    // need to see on the same frame the user pressed F.
    bool input_sampled_this_frame_ = false;
    MovementConfig movement_cfg_;

    uint64_t last_frame_us_ = 0;   // for dt calculation
    double vertical_velocity_ = 0.0; // m/s, +up
    bool grounded_ = true;          // last groundY probe succeeded
    double head_bob_phase_ = 0.0;   // accumulated bob phase (radians, double for precision)
    double lateral_bob_m_ = 0.0;    // applied to camera eye after AdoptPlayer
    double floor_y_ = 0.0;          // hard floor for feet.y (sea level default)

    // Pre-integration position. PhaseResolveCollision sweeps from this to
    // player_.feet_position to apply sliding without losing the start of
    // the sweep.
    Vec3 pre_integration_pos_;

    // Pre-computed sin(yaw) / cos(yaw) for player_.yaw_radians. Written
    // once by PhaseApplyMouseLook after it updates player_.yaw_radians.
    // Read by PhaseIntegrateMovement (saves 1 sinf + 1 cosf per Walking
    // frame) and forwarded into camera_.precomputed_yaw_trig_* so the
    // CameraState's BuildViewRotation path also skips the trig when the
    // rotation cache misses.
    //
    // The pair is valid for the duration of a single RunOnce invocation;
    // anything that mutates player_.yaw_radians outside of PhaseApplyMouseLook
    // (the FSM's snap-to-door / snap-to-seat at transition start) must
    // either recompute or invalidate this cache. PhaseApplyMouseLook
    // always overwrites it, so as long as PhaseApplyMouseLook runs at
    // the start of every RunOnce the cache stays current.
    float cached_sin_yaw_ = 0.0f;
    float cached_cos_yaw_ = 0.0f;

    // Camera override (set by OverrideCamera, consumed by RunOnce).
    bool camera_override_pending_ = false;
    CameraState camera_override_;
};

}  // namespace walkercore

#endif  // WALKERCORE_FRAME_LOOP_H