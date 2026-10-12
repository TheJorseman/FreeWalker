// WalkerCore — WalkerFsm workflow.
//
// Owns the user-facing state machine that decides what the walker is doing
// right now: in the truck driving (game camera), outside walking (walker
// camera), or transitioning between the two.
//
// State diagram:
//
//   Driving ──F──> TransitionOut ──done──> Walking
//   Walking ──F──> TransitionIn  ──done──> Driving
//
// In Driving and Walking the camera is owned by its respective system (the
// game, or the walker). Transition states tween the camera matrices and
// the player transform over a short fixed duration.
//
// Detection:
//   - The F (Interact) key triggers the transition only when "valid":
//       From Walking: player must be within `door_activation_distance_m`
//                     of a door trigger AND facing the truck.
//       From Driving: no spatial check needed; the user can always exit.
//   - "Is player inside cabin" is recomputed each frame from truck-local
//     space + the cabin bounding box.
//
// M5 SCOPE:
//   - This is the FSM that produces state and a transition factor (0..1).
//   - The render layer (D3D11 hook) decides whether to override the game's
//     view based on `ShouldRenderOverride()`.
//   - Door/cabin geometry uses TruckDoorConfig — defaults until M5 lander
//     collects per-truck data.

#ifndef WALKERCORE_WALKER_FSM_H
#define WALKERCORE_WALKER_FSM_H

#include "walkercore/CameraState.h"
#include "walkercore/PlayerState.h"
#include "walkercore/TruckDoorConfig.h"
#include "walkercore/TruckState.h"

namespace walkercore {

enum class WalkerFsmState : unsigned {
    Driving,         // game owns the camera; truck follows telemetry
    Walking,         // walker owns the camera; player moves on foot
    TransitionIn,    // tweens from Walking to Driving
    TransitionOut,   // tweens from Driving to Walking
};

// Stable, exhaustive name lookup for every WalkerFsmState value.
// Used by walker_dump_fsm() and the diagnostic dump's mirror label.
// New state enum entries MUST add a case here (-Werror -Wswitch-enum
// catches the omission at compile time) and MUST be added to the
// probe test TestWalkerFsmStateNameReturnsNameForEveryValue so the
// test registers the new bit under its expected display name.
//
// Falls back to "(unknown)" only for an out-of-range value — the
// function is meant to be safe to call from any log path.
const char *WalkerFsmStateName(WalkerFsmState s);

// Inputs from the per-frame workflow.
//
// The truck and player fields are non-owning pointers to the source
// snapshots held by the providers (ScsTelemetryProvider and
// FrameLoop respectively). The per-frame conductor (walker_per_frame_tick)
// populates these with the addresses of the live provider state — no
// copy. The FSM reads them through the pointer in ComputeTransitionTarget,
// PlacePlayerAtSeat, PlayerCanEnterDoor, etc.
//
// Lifetime contract: the pointers must remain valid for the duration of
// the WalkerFsm::Update call. In the per-frame conductor that is the
// FrameLoop's player_ member and the ScsTelemetryProvider's state_
// member, both of which outlive the call. Tests are responsible for
// keeping their fixtures alive across the Update call (a stack-local
// TruckState / PlayerState in the test function works because the
// pointer is used only inside the call).
//
// Why pointers, not values: the previous value-based struct forced the
// conductor to copy 104 bytes of truck + 48 bytes of player into the
// struct on every frame, then re-read them by const ref into the
// helper calls. Switching to pointers keeps the conductor's stack
// frame to two pointer stores and a couple of bools, and the helpers
// still receive const refs through the pointer dereference. Net
// per-frame saving: ~136 bytes of copy in the conductor (152 → 16).
struct WalkerFsmInputs {
    // Snapshot of the truck at the start of the frame. Must be non-null;
    // pass a default-constructed fallback (e.g. a file-static zero-init
    // TruckState) when the telemetry provider is not bound.
    const TruckState *truck;
    // Snapshot of the player. Must be non-null; the per-frame conductor
    // always has a FrameLoop bound with a valid player_, so production
    // never passes null here.
    const PlayerState *player;
    // Did the user press the interact key (F by default)?
    bool interact_pressed;
    // Did the user press the toggle-walk key (F10 by default)?
    bool toggle_walk_pressed;
};

class WalkerFsm {
public:
    WalkerFsm();

    // Wire dependencies. The FSM is the SINGLE authority on walk mode
    // (Driving vs Walking) and on the player placement around the truck.
    //
    // BindPlayerTarget points at the FrameLoop's PlayerState; the FSM
    // writes into it whenever a transition starts or completes so that
    // the FrameLoop picks up the correct feet position on its next
    // integration step. Without this binding, the FSM still computes the
    // camera tween, but the player transform is decoupled.
    void BindPlayerTarget(PlayerState *player);

    // Configuration. Defaults are sensible; M6 INI overrides them.
    void SetDoorConfig(const TruckDoorConfig &cfg);
    const TruckDoorConfig &doorConfig() const { return door_cfg_; }

    void SetTransitionSeconds(double s) {
        transition_seconds_ = (s > 0.05) ? s : 0.05;
    }

    // One frame of work. Updates state based on inputs, advances the
    // transition tween, and writes the camera target (where the camera
    // should be at end-of-frame) into out_camera_target.
    //
    // The render layer reads `state()` and `out_camera_target` to decide
    // whether to draw using walker's matrices (Walking, TransitionOut)
    // or hand control back to the game (Driving, TransitionIn).
    void Update(const WalkerFsmInputs &inputs, double dt,
                CameraState *out_camera_target);

    // Direct pointer variant of Update. The per-frame conductor uses
    // this to avoid the 104+48 byte TruckState/PlayerState copy that
    // a struct-by-value WalkerFsmInputs would require. Behaviour is
    // identical to Update(inputs, dt, out) — the struct overload is a
    // thin wrapper that constructs a WalkerFsmInputs with these
    // pointers and forwards. Callers (tests, ad-hoc tools) that already
    // have a struct can keep using Update; production code that has
    // access to the live provider state should prefer this overload.
    void UpdateFromPointers(const TruckState *truck,
                            const PlayerState *player,
                            bool interact_pressed,
                            bool toggle_walk_pressed,
                            double dt,
                            CameraState *out_camera_target);

    // True if the walker should drive the view matrix this frame.
    // Walking always; TransitionOut half-way through; never in Driving /
    // TransitionIn. The D3D11 hook uses this to know whether to push our
    // matrices or call the original Present.
    bool ShouldRenderOverride() const;

    WalkerFsmState state() const { return state_; }

    // 0..1 progress through a transition. 0 outside transitions.
    float transitionProgress() const {
        return static_cast<float>(transition_t_);
    }

private:
    // Recompute the player's feet/eye/yaw based on the truck + door config.
    // Writes to player in place.
    void PlacePlayerAtDoor(PlayerState &player, const TruckState &truck,
                          const DoorInfo &door) const;
    void PlacePlayerAtSeat(PlayerState &player, const TruckState &truck) const;
    void PlacePlayerExited(PlayerState &player, const TruckState &truck,
                           const DoorInfo &door) const;

    // True if `player` is within `door_activation_distance_m` of `door`,
    // AND roughly facing the truck.
    bool PlayerCanEnterDoor(const PlayerState &player,
                            const TruckState &truck,
                            const DoorInfo &door) const;

    // True if `player` is inside the cabin bounding box.
    bool PlayerIsInsideCabin(const PlayerState &player,
                            const TruckState &truck) const;

    // Compute camera target for the start/middle/end of a transition.
    // Takes a pointer to the live truck state (see UpdateFromPointers
    // for the lifetime contract).
    void ComputeTransitionTarget(const TruckState *truck,
                                 CameraState *out) const;

    WalkerFsmState state_ = WalkerFsmState::Driving;
    TruckDoorConfig door_cfg_ = DefaultTruckDoorConfig();
    double transition_seconds_ = 0.6;
    double transition_t_ = 0.0;

    // Snapshot of the player at the moment a transition began.
    // ComputeTransitionTarget reads feet_position, yaw_radians,
    // pitch_radians, and eye_height from this struct as the tween
    // origin. The pre-fix code also captured a CameraState snapshot
    // here ("transition_from_camera_") for an aborted idea of
    // tweening the view matrix from the live camera; the field was
    // never read and has been removed. The camera tween uses
    // ComputeTransitionTarget's Compute() block to derive the
    // from/to positions from the player snapshot alone.
    PlayerState transition_from_player_;

    // Optional non-owning pointer to FrameLoop's PlayerState. Written
    // when a transition starts (player snaps to the destination anchor)
    // and when a transition completes (player stays at the anchor). May
    // be null in tests / before PluginEntry wires things up.
    PlayerState *player_target_ = nullptr;
};

}  // namespace walkercore

#endif  // WALKERCORE_WALKER_FSM_H