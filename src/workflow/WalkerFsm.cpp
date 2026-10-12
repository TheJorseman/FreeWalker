// WalkerCore — WalkerFsm implementation.

#include "walkercore/WalkerFsm.h"

#include <math.h>

#include "walkercore/Log.h"

namespace walkercore {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline double NormalizeAngle(double a) {
    while (a > kPi) a -= 2.0 * kPi;
    while (a < -kPi) a += 2.0 * kPi;
    return a;
}

inline Vec3 LerpVec3(Vec3 a, Vec3 b, double t) {
    return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t};
}

inline float LerpAngle(float a, float b, double t) {
    // Shortest-path angle lerp.
    double diff = NormalizeAngle(static_cast<double>(b) - static_cast<double>(a));
    return a + static_cast<float>(diff * t);
}

}  // namespace

WalkerFsm::WalkerFsm() {
    state_ = WalkerFsmState::Driving;
    transition_t_ = 0.0;
}

// WalkerFsmStateName — exhaustive switch. Adding a new
// WalkerFsmState without updating this switch fails to compile under
// -Werror because of -Wswitch-enum, and TestWalkerFsmStateName…* below
// fails because the probe table expects each case.
const char *WalkerFsmStateName(WalkerFsmState s) {
    switch (s) {
        case WalkerFsmState::Driving:        return "Driving";
        case WalkerFsmState::Walking:        return "Walking";
        case WalkerFsmState::TransitionIn:   return "TransitionIn";
        case WalkerFsmState::TransitionOut:  return "TransitionOut";
    }
    return "(unknown)";
}

void WalkerFsm::BindPlayerTarget(PlayerState *player) {
    player_target_ = player;
}

void WalkerFsm::SetDoorConfig(const TruckDoorConfig &cfg) {
    door_cfg_ = cfg;
}

void WalkerFsm::PlacePlayerAtDoor(PlayerState &player,
                                   const TruckState &truck,
                                   const DoorInfo &door) const {
    if (!truck.has_placement) return;
    const double heading = static_cast<double>(truck.placement.orientation.heading) *
                            (2.0 * kPi);  // unit-range → radians
    const Vec3 world_anchor =
        Vec3{truck.placement.position.x +
                 TruckLocalToWorld(door.local_enter_anchor, heading).x,
             truck.placement.position.y +
                 door.local_enter_anchor.y,
             truck.placement.position.z +
                 TruckLocalToWorld(door.local_enter_anchor, heading).z};
    player.feet_position = world_anchor;
    player.yaw_radians = static_cast<float>(heading) + door.enter_yaw_offset_rad;
    player.pitch_radians = 0.0f;
    player.eye_height = 1.65f;
    player.mode = PlayerMode::Standing;
    player.has_feet = true;
}

void WalkerFsm::PlacePlayerAtSeat(PlayerState &player,
                                   const TruckState &truck) const {
    if (!truck.has_placement) return;
    const double heading = static_cast<double>(truck.placement.orientation.heading) *
                            (2.0 * kPi);
    const Vec3 world_seat =
        Vec3{truck.placement.position.x +
                 TruckLocalToWorld(door_cfg_.driver_seat_local, heading).x,
             truck.placement.position.y + door_cfg_.driver_seat_local.y,
             truck.placement.position.z +
                 TruckLocalToWorld(door_cfg_.driver_seat_local, heading).z};
    player.feet_position = world_seat;
    player.yaw_radians = static_cast<float>(heading);
    player.pitch_radians = 0.0f;
    player.eye_height = 0.0f;  // seated; camera uses seat position directly
    player.mode = PlayerMode::Idle;
    player.has_feet = true;
}

void WalkerFsm::PlacePlayerExited(PlayerState &player,
                                   const TruckState &truck,
                                   const DoorInfo &door) const {
    PlacePlayerAtDoor(player, truck, door);
}

bool WalkerFsm::PlayerCanEnterDoor(const PlayerState &player,
                                    const TruckState &truck,
                                    const DoorInfo &door) const {
    if (!truck.has_placement) return false;
    const double heading = static_cast<double>(truck.placement.orientation.heading) *
                            (2.0 * kPi);
    const Vec3 world_door =
        Vec3{truck.placement.position.x +
                 TruckLocalToWorld(door.local_position, heading).x,
             truck.placement.position.y + door.local_position.y,
             truck.placement.position.z +
                 TruckLocalToWorld(door.local_position, heading).z};
    const double dx = player.feet_position.x - world_door.x;
    const double dy = player.feet_position.y - world_door.y;
    const double dz = player.feet_position.z - world_door.z;
    const double dist = sqrt(dx * dx + dy * dy + dz * dz);
    return dist <= static_cast<double>(door_cfg_.door_activation_distance_m);
}

bool WalkerFsm::PlayerIsInsideCabin(const PlayerState &player,
                                     const TruckState &truck) const {
    if (!truck.has_placement) return false;
    const double heading = static_cast<double>(truck.placement.orientation.heading) *
                            (2.0 * kPi);
    const Vec3 local = WorldToTruckLocal(
        Vec3{player.feet_position.x - truck.placement.position.x,
             player.feet_position.y - truck.placement.position.y,
             player.feet_position.z - truck.placement.position.z},
        heading);
    return (local.x >= door_cfg_.cabin_min.x && local.x <= door_cfg_.cabin_max.x &&
            local.y >= door_cfg_.cabin_min.y && local.y <= door_cfg_.cabin_max.y &&
            local.z >= door_cfg_.cabin_min.z && local.z <= door_cfg_.cabin_max.z);
}

void WalkerFsm::ComputeTransitionTarget(const TruckState *truck,
                                         CameraState *out) const {
    // The "from" position is captured at the start of the transition. The
    // "to" position depends on which way we're going and on the truck's
    // current placement.
    const Vec3 from_feet = transition_from_player_.feet_position;
    const float from_yaw = transition_from_player_.yaw_radians;
    const float from_eye_height = transition_from_player_.eye_height;
    const float from_pitch = transition_from_player_.pitch_radians;

    Vec3 to_feet = from_feet;
    float to_yaw = from_yaw;
    float to_eye_height = from_eye_height;
    if (truck->has_placement) {
        const double heading =
            static_cast<double>(truck->placement.orientation.heading) *
            (2.0 * kPi);
        if (state_ == WalkerFsmState::TransitionIn) {
            // Walking → seat.
            const Vec3 world_seat = Vec3{
                truck->placement.position.x +
                    TruckLocalToWorld(door_cfg_.driver_seat_local, heading).x,
                truck->placement.position.y + door_cfg_.driver_seat_local.y,
                truck->placement.position.z +
                    TruckLocalToWorld(door_cfg_.driver_seat_local, heading).z};
            to_feet = world_seat;
            to_yaw = static_cast<float>(heading);
            to_eye_height = 0.0f;  // seated; camera origin = feet position
        } else {
            // Driving → door anchor.
            const DoorInfo &door = door_cfg_.driver_door;
            const Vec3 world_anchor = Vec3{
                truck->placement.position.x +
                    TruckLocalToWorld(door.local_enter_anchor, heading).x,
                truck->placement.position.y + door.local_enter_anchor.y,
                truck->placement.position.z +
                    TruckLocalToWorld(door.local_enter_anchor, heading).z};
            to_feet = world_anchor;
            to_yaw = static_cast<float>(heading) + door.enter_yaw_offset_rad;
            to_eye_height = 1.65f;
        }
    }

    // Lerp.
    const Vec3 feet = LerpVec3(from_feet, to_feet, transition_t_);
    const float yaw = LerpAngle(from_yaw, to_yaw, transition_t_);
    const float pitch = from_pitch * (1.0f - static_cast<float>(transition_t_));
    const float eye_height = from_eye_height +
        (to_eye_height - from_eye_height) * static_cast<float>(transition_t_);

    out->eye_position = Vec3{feet.x, feet.y + static_cast<double>(eye_height), feet.z};
    out->yaw_radians = yaw;
    out->pitch_radians = pitch;
}

void WalkerFsm::Update(const WalkerFsmInputs &inputs, double dt,
                       CameraState *out_camera_target) {
    // Thin wrapper around UpdateFromPointers. Kept so tests (and ad-hoc
    // tools) that already have a WalkerFsmInputs by value can drive the
    // FSM without manually pulling out the pointers. The struct fields
    // are read once each; the function does not modify them.
    UpdateFromPointers(inputs.truck, inputs.player, inputs.interact_pressed,
                       inputs.toggle_walk_pressed, dt, out_camera_target);
}

void WalkerFsm::UpdateFromPointers(const TruckState *truck,
                                   const PlayerState *player,
                                   bool interact_pressed,
                                   bool toggle_walk_pressed,
                                   double dt,
                                   CameraState *out_camera_target) {
    if (out_camera_target == nullptr) {
        return;
    }
    // Defensive null guards on the truck/player pointers. The conductor
    // always passes non-null today (a file-static kEmptyTruckState
    // fallback when telemetry is null), but PlacePlayerAtDoor /
    // PlacePlayerExited / ComputeTransitionTarget dereference *truck
    // without checking — a null here would crash the per-frame loop.
    // The early return leaves out_camera_target->valid=false (set below)
    // so the renderer falls back to the previous camera; this matches
    // the behaviour when out_camera_target itself is null.
    if (truck == nullptr || player == nullptr) {
        out_camera_target->valid = false;
        return;
    }

    // Defaults — most states don't override the camera; the render layer
    // uses ShouldRenderOverride() to decide.
    out_camera_target->valid = false;

    switch (state_) {
        case WalkerFsmState::Driving:
            // Listen for F (Interact) or F10 (ToggleWalk) to exit. We
            // allow exit at any time; the spatial check used to live here
            // for F but was moved to the Walking side (PlayerCanEnterDoor)
            // so a Walking player who wandered too far away from the door
            // simply can't press F to re-enter.
            if (interact_pressed || toggle_walk_pressed) {
                transition_from_player_ = *player;
                // Snap the FrameLoop's player to the door anchor NOW so
                // that when TransitionOut completes and state goes to
                // Walking, the player is already standing at the door.
                // The per-frame conductor wires player_target_ to the
                // same memory as inputs.player, so this placement also
                // mutates *player. We must NOT re-read *player after
                // the placement to refresh transition_from_player_ —
                // that would overwrite the cabin-seat "from" snapshot
                // with the post-placement (door anchor) state, turning
                // the cabin-to-door camera tween into a no-op lerp
                // (from=door, to=door). The earlier capture above
                // already holds the cabin-seat values we want as the
                // tween origin.
                if (player_target_ != nullptr) {
                    PlacePlayerExited(*player_target_, *truck,
                                      door_cfg_.driver_door);
                }
                transition_t_ = 0.0;
                state_ = WalkerFsmState::TransitionOut;
                WALKERCORE_LOG_INFO(
                    "WalkerFsm: Driving → TransitionOut (tween %.2fs, source=%s)",
                    transition_seconds_,
                    toggle_walk_pressed ? "F10" : "F");
            }
            break;

        case WalkerFsmState::Walking: {
            // Two ways to re-enter the truck:
            //   - F while near a door (spatial check).
            //   - F10 from anywhere (the "I want to drive now" key).
            // Both start a TransitionIn tween to the seat.
            const bool f_near_door =
                interact_pressed &&
                PlayerCanEnterDoor(*player, *truck,
                                   door_cfg_.driver_door);
            const bool f10_anywhere = toggle_walk_pressed;
            if (f_near_door || f10_anywhere) {
                transition_from_player_ = *player;
                // Snap the FrameLoop's player to the seat NOW so the
                // player is at the seat by the time TransitionIn ends
                // and the game takes over the camera.
                if (player_target_ != nullptr) {
                    PlacePlayerAtSeat(*player_target_, *truck);
                }
                transition_t_ = 0.0;
                state_ = WalkerFsmState::TransitionIn;
                WALKERCORE_LOG_INFO(
                    "WalkerFsm: Walking → TransitionIn (tween %.2fs, source=%s)",
                    transition_seconds_,
                    f10_anywhere ? "F10" : "F");
            }
            break;
        }

        case WalkerFsmState::TransitionIn:
            transition_t_ += dt / transition_seconds_;
            if (transition_t_ >= 1.0) {
                transition_t_ = 1.0;
                state_ = WalkerFsmState::Driving;
                WALKERCORE_LOG_INFO("WalkerFsm: TransitionIn complete → Driving");
            }
            // Camera tween from walking position to seat.
            ComputeTransitionTarget(truck, out_camera_target);
            out_camera_target->valid = true;
            break;

        case WalkerFsmState::TransitionOut:
            transition_t_ += dt / transition_seconds_;
            if (transition_t_ >= 1.0) {
                transition_t_ = 1.0;
                state_ = WalkerFsmState::Walking;
                WALKERCORE_LOG_INFO("WalkerFsm: TransitionOut complete → Walking");
            }
            // Camera tween from seat/whatever the game had to the door
            // anchor; FrameLoop takes over with normal walking logic
            // once t reaches 1.0.
            ComputeTransitionTarget(truck, out_camera_target);
            out_camera_target->valid = true;
            break;
    }
}

bool WalkerFsm::ShouldRenderOverride() const {
    // Walking always. TransitionOut uses the walker camera until midway;
    // TransitionIn hands control back to the game after midway.
    if (state_ == WalkerFsmState::Walking) return true;
    if (state_ == WalkerFsmState::TransitionOut) return transition_t_ < 0.6;
    return false;
}

}  // namespace walkercore