// WalkerCore — PlayerState domain type.
//
// The "player" in WalkerCore is the body we control when on foot. It is
// distinct from the truck (which lives in TruckState) and from the
// camera (CameraState). The three are linked: the camera follows the
// player's head, the player cannot pass through walls (collision in M3+),
// and getting back into the truck means teleporting this PlayerState to
// match the cabin seat (M5).
//
// Coordinate space:
//   - World position is the same as TruckState's (X east, Y up, Z south,
//     doubles for position).
//   - Head position is derived from world position + a vertical offset
//     (1.65 m by default, lowered when crouching).
//   - Rotation is yaw + pitch only — we never roll the player. Stored as
//     radians (the SDK's Euler uses unit-range; we use radians because
//     it's what the renderer and trig routines consume).

#ifndef WALKERCORE_PLAYER_STATE_H
#define WALKERCORE_PLAYER_STATE_H

#include <math.h>

#include "walkercore/TruckState.h"  // for Vec3

namespace walkercore {

// Movement modes the FSM can be in. Walking has sub-modes for stamina /
// bob amount; falling is independent of jump intent because the physics
// might lift the player off the ground anyway (e.g. hit by a vehicle).
enum class PlayerMode : unsigned {
    Standing = 0,    // not in walk mode; irrelevant until M5
    Idle,            // standing on the ground, no input
    Walking,
    Running,
    CrouchIdle,
    CrouchWalk,
    Jumping,
    Falling,         // airborne without jumping intent (driven by M3+ physics)
    Stumbling,       // walking into a wall at speed (M4+)
    KnockedOut,      // vehicle impact (M10)
};

struct PlayerState {
    // World position of the player's feet on the ground. Camera derives
    // from this + head_offset.
    Vec3 feet_position;

    // World-space orientation: yaw (around Y) and pitch (around X).
    // Pitch is clamped to ~[-89°, +89°] to avoid gimbal lock at the
    // poles. Roll is always zero — we don't roll the walker.
    float yaw_radians;
    float pitch_radians;

    // Vertical offset from feet to eyes (head height). Defaults to 1.65 m
    // (typical eye level for a 1.75 m adult). Crouching halves this.
    // Jumping reduces it (the camera lowers as the body folds in flight).
    float eye_height;

    // Current movement mode. Drives animation (M4+) and physics intents
    // (M3+).
    PlayerMode mode;

    // When the FSM is not in WALKING mode, has_feet is false and the rest
    // of the struct is stale.
    bool has_feet;
};

// Reset to a known standing-on-ground state.
inline void PlayerState_Reset(PlayerState &p) {
    p.feet_position = {0.0, 0.0, 0.0};
    p.yaw_radians = 0.0f;
    p.pitch_radians = 0.0f;
    p.eye_height = 1.65f;
    p.mode = PlayerMode::Standing;
    p.has_feet = false;
}

// Human-readable name for a PlayerMode. Used by diagnostic dumps and
// log output. The string is a static literal — callers don't own it.
// Returns "(unknown)" for a value that is outside the defined enum range.
inline const char *PlayerModeName(PlayerMode m) {
    switch (m) {
        case PlayerMode::Standing:    return "Standing";
        case PlayerMode::Idle:        return "Idle";
        case PlayerMode::Walking:     return "Walking";
        case PlayerMode::Running:     return "Running";
        case PlayerMode::CrouchIdle:  return "CrouchIdle";
        case PlayerMode::CrouchWalk:  return "CrouchWalk";
        case PlayerMode::Jumping:     return "Jumping";
        case PlayerMode::Falling:     return "Falling";
        case PlayerMode::Stumbling:   return "Stumbling";
        case PlayerMode::KnockedOut:  return "KnockedOut";
    }
    return "(unknown)";
}

// Compute eye (camera anchor) position from feet + eye_height + view pitch.
// Used by CameraState to derive its origin.
inline Vec3 PlayerState_EyePosition(const PlayerState &p) {
    // Pure vertical offset; the pitch rotation is applied later when
    // computing the camera forward vector (M2 work).
    return {p.feet_position.x,
            p.feet_position.y + static_cast<double>(p.eye_height),
            p.feet_position.z};
}

// Convenience constants in radians for clamping.
constexpr float kMaxPitchRadians = 1.5533f;   // ~89°
constexpr float kMinPitchRadians = -1.5533f;

inline void PlayerState_ClampPitch(PlayerState &p) {
    if (p.pitch_radians > kMaxPitchRadians) p.pitch_radians = kMaxPitchRadians;
    if (p.pitch_radians < kMinPitchRadians) p.pitch_radians = kMinPitchRadians;
}

}  // namespace walkercore

#endif  // WALKERCORE_PLAYER_STATE_H