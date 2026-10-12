// WalkerCore — per-truck door / cabin geometry.
//
// M5 scope: a *placeholder* description of where the driver's door is on a
// generic truck cabin, where the seat is, and the cabin bounding box.
//
// Why per-truck data is hard:
//   Real production WalkerCore needs this per-truck (every cabin model has
//   a different door offset). TMRealWalk gets it from a per-truck table
//   shipped with the plugin's installer. We don't have that data yet, so
//   for M5 we ship a "generic cabin" used as a default when the version
//   table has no truck-specific entry.
//
// Coordinate space:
//   All positions are in truck-local coordinates (X right, Y up, Z back).
//   To use in world space, add truck.placement.position after rotating by
//   truck.placement.orientation.heading.
//
// M5 lander:
//   When real per-truck data is collected, populate the table from each
//   truck's model file. The placeholder here is good enough to test the
//   state machine end-to-end against the game's own player position.

#ifndef WALKERCORE_TRUCK_DOOR_CONFIG_H
#define WALKERCORE_TRUCK_DOOR_CONFIG_H

#include <math.h>

#include "walkercore/TruckState.h"

namespace walkercore {

// Door trigger — where the player has to stand to use the door.
struct DoorInfo {
    Vec3 local_position;          // trigger centre, truck-local space
    Vec3 local_enter_anchor;      // where the player stands when entering
    float enter_yaw_offset_rad;   // face this offset from truck heading
    bool is_driver_door;
};

// Cabin bounding box (truck-local). Used to decide "is the player inside
// the cabin?" without doing per-frame collision queries.
struct TruckDoorConfig {
    DoorInfo driver_door;
    DoorInfo passenger_door;
    bool has_passenger_door;

    // Cabin interior bounding box, truck-local space.
    Vec3 cabin_min;
    Vec3 cabin_max;

    // Driver seat world position (truck-local space).
    Vec3 driver_seat_local;

    // Distance to the door trigger at which "press F to enter" shows up.
    float door_activation_distance_m = 1.5f;

    // Distance at which the player is considered "in the cabin".
    float cabin_inside_distance_m = 0.4f;

    // Door animation parameters (M7). The DoorAnimProvider plays a
    // smoothstep tween from 0 (closed) to door_max_open_rad when the
    // player exits the cabin, and back to 0 when they enter. Until the
    // M7 lander injects a real skeletal animation, the angle is exposed
    // via the diagnostic dump and consumed by renderers that want to
    // move a door mesh in lockstep with the transition.
    float door_max_open_rad = 1.5708f;   // ~90° — typical full-swing
    float door_open_duration_s = 0.6f;    // matches transition_seconds_

    // True if this config has been validated for the current truck model.
    // False means "fall back to the default placeholder".
    bool valid = false;
};

// A generic cabin placeholder. Matches the layout of a typical US-style
// daycab — driver on the left, door just forward of the rear axle.
inline TruckDoorConfig DefaultTruckDoorConfig() {
    TruckDoorConfig c;
    c.driver_door.local_position = Vec3{-0.95, 0.55, -1.20};
    c.driver_door.local_enter_anchor = Vec3{-1.40, 0.0, -1.20};
    c.driver_door.enter_yaw_offset_rad = 1.5708f;  // face the truck (90°)
    c.driver_door.is_driver_door = true;

    c.passenger_door = c.driver_door;
    c.passenger_door.local_position = Vec3{0.95, 0.55, -1.20};
    c.passenger_door.local_enter_anchor = Vec3{1.40, 0.0, -1.20};
    c.passenger_door.enter_yaw_offset_rad = -1.5708f;
    c.passenger_door.is_driver_door = false;
    c.has_passenger_door = false;

    c.cabin_min = Vec3{-1.10, 0.0, -1.70};
    c.cabin_max = Vec3{1.10, 1.65, 0.40};

    c.driver_seat_local = Vec3{-0.45, 0.55, -0.50};

    c.valid = true;
    return c;
}

// M5 Lander partial: build a TruckDoorConfig from the live
// TruckState when the SCS configuration event has delivered the
// per-truck cabin + head anchors. The driver_seat_local is
// sourced from head_position (the eye-point inside the cabin
// is what the player is anchored to when seated); the
// driver_door.local_position is computed from cabin_position
// (door sits just forward of the cabin anchor, on the left
// side of the truck for LHD markets).
//
// When the configuration event has not fired yet, the function
// returns the default placeholder. Callers MUST check
// `config.valid` and treat `valid = false` as "fall back to
// DefaultTruckDoorConfig()".
//
// This is a partial M5: a real lander would also have a
// per-truck table that maps `truck.model_id` (e.g.
// "vehicle.volvo.vnl_2016") to door geometry that better matches
// the cabin shape (the SCS cabin_position is the cabin CENTRE,
// but the door pivot is offset from the centre by a per-truck
// amount). The real lander table is shipped in a separate data
// file; this function is the integration point the table feeds.
inline TruckDoorConfig TruckDoorConfigFromTruckState(const TruckState &t) {
    if (!t.has_cabin_position || !t.has_head_position) {
        // Configuration event has not fired (or this is a build
        // that does not publish these attributes). Return the
        // default; the caller checks `valid` and applies the
        // same fallback. We deliberately return the default
        // here rather than a "valid=false" sentinel so a
        // per-truck table that is partially populated still
        // produces a usable config.
        return DefaultTruckDoorConfig();
    }
    TruckDoorConfig c;
    // Driver door: 0.45 m to the left of the cabin anchor
    // (driver's side for LHD markets) and 0.4 m forward
    // (toward the front of the cab). Y=0.55 m is waist
    // height for a typical driver.
    c.driver_door.local_position = Vec3{
        t.cabin_position.x - 0.45,
        t.cabin_position.y + 0.55,
        t.cabin_position.z - 0.40};
    // Enter anchor: 0.5 m further left, ground level.
    c.driver_door.local_enter_anchor = Vec3{
        t.cabin_position.x - 0.95,
        0.0,
        t.cabin_position.z - 0.40};
    c.driver_door.enter_yaw_offset_rad = 1.5708f;
    c.driver_door.is_driver_door = true;

    // Passenger door: mirror the driver door on the right side.
    c.passenger_door = c.driver_door;
    c.passenger_door.local_position = Vec3{
        t.cabin_position.x + 0.45,
        t.cabin_position.y + 0.55,
        t.cabin_position.z - 0.40};
    c.passenger_door.local_enter_anchor = Vec3{
        t.cabin_position.x + 0.95,
        0.0,
        t.cabin_position.z - 0.40};
    c.passenger_door.enter_yaw_offset_rad = -1.5708f;
    c.passenger_door.is_driver_door = false;
    c.has_passenger_door = false;

    // Cabin bounding box: a 2.0 × 1.65 × 1.4 m box centred on
    // the cabin anchor. This is the same shape as the default
    // placeholder's box, just translated to the per-truck
    // cabin anchor. The real lander's per-truck table would
    // override this for trucks with non-rectangular cabins
    // (e.g. cab-overs with a flat-front).
    c.cabin_min = Vec3{
        t.cabin_position.x - 1.10,
        t.cabin_position.y,
        t.cabin_position.z - 1.70};
    c.cabin_max = Vec3{
        t.cabin_position.x + 1.10,
        t.cabin_position.y + 1.65,
        t.cabin_position.z + 0.40};

    // Driver seat local: derived from the head anchor (the
    // seated player's eye-point). Y stays as reported by the
    // SDK; for non-day-cab trucks (e.g. high-cab sleepers)
    // this is correct; the camera eye-height adjustment
    // happens in CameraState_AdoptPlayer.
    c.driver_seat_local = t.head_position;

    c.valid = true;
    return c;
}

// Rotate a truck-local point into world space using the truck's heading.
// Roll/pitch are ignored for door geometry — trucks are nominally level.
inline Vec3 TruckLocalToWorld(Vec3 local, double heading_radians) {
    const double c = cos(heading_radians);
    const double s = sin(heading_radians);
    // Heading 0 = facing +Z. Rotation by heading around +Y.
    return Vec3{
        c * local.x + s * local.z,
        local.y,
        -s * local.x + c * local.z,
    };
}

// Inverse of TruckLocalToWorld: world → truck-local.
inline Vec3 WorldToTruckLocal(Vec3 world, double heading_radians) {
    const double c = cos(heading_radians);
    const double s = sin(heading_radians);
    return Vec3{
        c * world.x - s * world.z,
        world.y,
        s * world.x + c * world.z,
    };
}

}  // namespace walkercore

#endif  // WALKERCORE_TRUCK_DOOR_CONFIG_H