// WalkerCore — TruckState domain type.
//
// A pure-C++ snapshot of what we know about the player's truck. This is
// the dependency target for FrameLoop, StateMachine, Renderer, and
// anything else that needs to ask "where is the truck right now?".
//
// The ScsTelemetryProvider writes to this struct; everyone else reads.
// No SCS SDK types are referenced from here, so domain modules stay
// portable and can be unit-tested without the SDK.
//
// Layout note: all fields are POD and the struct is a value type. We
// could not break the rule even if we wanted — there are no virtual
// methods, no ownership, no hidden state.

#ifndef WALKERCORE_TRUCK_STATE_H
#define WALKERCORE_TRUCK_STATE_H

#include <stdint.h>

namespace walkercore {

// Axis convention: world space — X east, Y up, Z south (matches SCS SDK).
// We use double precision throughout the domain to avoid cumulative drift
// from float-to-double conversion at every read.
struct Vec3 {
    double x;
    double y;
    double z;
};

// Euler orientation in the same convention as scs_value_euler_t. Heading is
// the unit-range representation (1.0 == 360°). Pitch and roll use their
// own unit ranges documented in scssdk_value.h.
//
// Keeping the same encoding as the SDK lets us memcpy from the SDK value
// into TruckState::placement.orientation without conversion cost.
struct Orientation {
    float heading;
    float pitch;
    float roll;
};

// World-space pose (position + orientation).
struct Placement {
    Vec3 position;
    Orientation orientation;
};

// Per-channel validity flags. We only set the field if the SDK reported
// it; downstream code checks `has_*` before reading.
struct TruckState {
    // Microseconds since SCS telemetry started, copied from the SDK's
    // timestamp when any field was last updated. M0..M1 use this for
    // diagnostic only; FrameLoop will use it for dt calculation in M2+.
    uint64_t last_update_us = 0;

    // Whether each field has been populated at least once. Useful for
    // startup races: until the first callback fires, the snapshot is
    // empty and FrameLoop should treat it as "no truck" rather than
    // reading zeros.
    bool has_placement = false;
    bool has_speed = false;
    bool has_parking_brake = false;
    bool has_trailer_attached = false;
    bool has_fuel = false;
    bool has_adblue = false;
    // M8 Lander: tank capacities come from the SCS_TELEMETRY_CONFIG_*
    // attributes `fuel.capacity` and `adblue.capacity` (delivered in
    // the configuration event, alongside hook_position). Until the
    // configuration event fires these stay at 0 and downstream code
    // (FuelProvider) must check before reading — without the flag,
    // a default 600 L capacity would silently override the real
    // per-truck value when the SDK later delivers a different one.
    bool has_fuel_capacity = false;
    bool has_adblue_capacity = false;

    // M9 truck configuration from SCS_TELEMETRY_EVENT_configuration:
    // the king-pin / fifth-wheel position in truck-local space. Until
    // the configuration event fires, the value is stale (default 0)
    // and downstream coupling code must check has_hook_position
    // before using it. Convention: +X is right of cab, +Y is up, +Z is
    // rear (matches the SCS SDK dvector convention used by
    // SCS_TELEMETRY_CONFIG_ATTRIBUTE_hook_position).
    bool has_hook_position = false;
    Vec3 hook_position{0.0, 0.0, 0.0};

    // M5 truck configuration: cabin + head anchor positions in
    // truck-local space. Used by TruckDoorConfig to compute the
    // per-truck door pivot when the player exits (WalkerFsm
    // TransitionOut Driving → Walking). Same convention as
    // hook_position: dvector, truck-local, +X right / +Y up /
    // +Z rear. Until the configuration event fires, both stay at
    // zero and the per-truck door is skipped in favour of the
    // default placeholder.
    bool has_cabin_position = false;
    Vec3 cabin_position{0.0, 0.0, 0.0};
    bool has_head_position = false;
    Vec3 head_position{0.0, 0.0, 0.0};

    // M5 truck model id (e.g. "vehicle.volvo.vnl_2016" for an ATS
    // VNL). This is the SCS_TELEMETRY_CONFIG_ATTRIBUTE_id from the
    // configuration event, kept as a small inline buffer to avoid
    // a separate std::string allocation in the hot path. The
    // buffer is always null-terminated; if the SDK reports an id
    // longer than kModelIdCapacity, we truncate. Empty string
    // means "no id yet" (configuration event has not fired).
    static constexpr size_t kModelIdCapacity = 64;
    char model_id[kModelIdCapacity] = {0};
    bool has_model_id = false;

    // M9 trailer world placement (telemetry.0.world.placement). The
    // trailer's pose in world space, refreshed every frame. Used by
    // the coupling provider to compute the distance from the player
    // to the trailer's king-pin (front-bottom) and to drive the
    // ApproachToKingPin waypoint. Until the channel fires, the value
    // is stale and has_trailer_placement stays false.
    bool has_trailer_placement = false;
    Placement trailer_placement;

    // Cached scalar speed in m/s, derived from the local velocity vector.
    // Convenience for collision/bob-speed thresholds.
    double speed_mps = 0.0;

    // World placement.
    Placement placement;

    // Local linear velocity (vehicle space).
    Vec3 local_linear_velocity;

    // Booleans.
    bool parking_brake = false;
    bool trailer_attached = false;

    // M8 fuel telemetry. `fuel_amount_l` is the current fuel in liters
    // (mirrors `truck.fuel.amount`); `adblue_amount_l` mirrors `truck.adblue`.
    // Until the corresponding `has_*` flag is set, these values are stale
    // (default 0) — downstream code must check before reading.
    float fuel_amount_l = 0.0f;
    float adblue_amount_l = 0.0f;
    // M8 Lander: per-truck tank capacities in liters. Mirrors
    // `fuel.capacity` / `adblue.capacity` from the SCS configuration
    // event. Real ATS trucks range from 380 L (day-cab Volvo VNL) to
    // 1500+ L (8x4 heavy-haul tractor); AdBlue is consistently ~50 L
    // but the SDK's per-truck value wins once delivered. Used by
    // FuelProvider to clamp the fueling fill so the player can't
    // over-fill a 380 L tank to 600 L.
    float fuel_capacity_l = 0.0f;
    float adblue_capacity_l = 0.0f;
};

// Reset all flags to false but keep allocation. Cheap.
inline void TruckState_Clear(TruckState &s) {
    s.last_update_us = 0;
    s.has_placement = false;
    s.has_speed = false;
    s.has_parking_brake = false;
    s.has_trailer_attached = false;
    s.has_fuel = false;
    s.has_adblue = false;
    s.has_fuel_capacity = false;
    s.has_adblue_capacity = false;
    s.has_hook_position = false;
    s.hook_position = {0.0, 0.0, 0.0};
    s.has_cabin_position = false;
    s.cabin_position = {0.0, 0.0, 0.0};
    s.has_head_position = false;
    s.head_position = {0.0, 0.0, 0.0};
    for (size_t i = 0; i < TruckState::kModelIdCapacity; ++i) s.model_id[i] = '\0';
    s.has_model_id = false;
    s.has_trailer_placement = false;
    s.trailer_placement = {};
    s.speed_mps = 0.0;
    s.placement = {};
    s.local_linear_velocity = {};
    s.parking_brake = false;
    s.trailer_attached = false;
    s.fuel_amount_l = 0.0f;
    s.adblue_amount_l = 0.0f;
    s.fuel_capacity_l = 0.0f;
    s.adblue_capacity_l = 0.0f;
}

}  // namespace walkercore

#endif  // WALKERCORE_TRUCK_STATE_H