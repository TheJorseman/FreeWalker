// WalkerCore — SCS telemetry provider implementation.
//
// The SCS SDK calls a registered callback once per frame, per channel.
// We register four channels in M1:
//   - truck.world.placement   (dplacement)
//   - truck.local.velocity.linear (fvector)
//   - truck.brake.parking     (bool)
//   - trailer.connected       (bool)
//
// Each callback just copies the relevant field out of the scs_value_t
// union into TruckState. We do NO computation here: FrameLoop (M2+) is
// where integration happens. The provider is the I/O layer; the workflow
// is the CPU layer.
//
// All callbacks are C-style free functions (extern "C"). The SCS SDK's
// API uses SCSAPIFUNC which is __stdcall on MSVC and the default ABI on
// GCC. We use free functions with C linkage so the SDK's pointer-typed
// registration accepts us cleanly. The "context" parameter is the pointer
// back to the ScsTelemetryProvider instance — that lets one set of
// callbacks serve multiple instances if we ever want to.

#include "walkercore/ScsTelemetryProvider.h"

#include <math.h>
#include <stdint.h>
#include <cstring>
#include <string.h>

// Common-channel headers define the channel-name string constants. The SDK
// is distributed split across common/ and per-game folders; we use
// common/ because both ETS2 and ATS share these names. The configs header
// declares SCS_TELEMETRY_CONFIG_*_attribute constants used by the M9
// configuration-event callback below.
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_trailer_common_channels.h"
#include "common/scssdk_telemetry_common_configs.h"

#include "walkercore/Log.h"
#include "walkercore/TruckState.h"

// Compatibility shim: the SCS SDK defines SCSAPI_RESULT / SCSAPI_VOID /
// SCSAPI_RESULT_FPTR / SCSAPI_VOID_FPTR but not a plain SCSAPI. We use
// SCSAPI as a one-token calling-convention modifier between the return
// type and the function name in our free-function callbacks; define it
// to alias SCSAPIFUNC, which is `__stdcall` on MSVC and empty on GCC.
// Result: `void SCSAPI func(...)` becomes `void __stdcall func(...)` on
// MSVC and `void func(...)` on GCC, both legal.
#ifndef SCSAPI
#define SCSAPI SCSAPIFUNC
#endif

#include "scssdk.h"
#include "scssdk_telemetry.h"
#include "scssdk_telemetry_channel.h"

namespace walkercore {

namespace {

// Pull the SDK function pointer types into a private alias so the rest of
// the file can use short names. Aliases do NOT pollute the public header.
using RegisterFn = scs_telemetry_register_for_channel_t;
using UnregisterFn = scs_telemetry_unregister_from_channel_t;
using RegisterEventFn = scs_telemetry_register_for_event_t;
using UnregisterEventFn = scs_telemetry_unregister_from_event_t;

// ---------- channel callbacks ----------

// truck.world.placement (dplacement).
//
// SDK note: the SCS docs say that fplacement and dplacement also support
// euler-only (orientation) and dvector-only (position). We want both, so
// we register the full dplacement type.
void SCSAPI OnTruckWorldPlacement(const scs_string_t /*name*/,
                                                            const scs_u32_t /*index*/,
                                                            const scs_value_t *const value,
                                                            const scs_context_t context) {
    if (value == nullptr || value->type != SCS_VALUE_TYPE_dplacement) {
        return;
    }
    auto *self = static_cast<ScsTelemetryProvider *>(context);
    TruckState &s = self->mutable_state();
    s.placement.position.x = value->value_dplacement.position.x;
    s.placement.position.y = value->value_dplacement.position.y;
    s.placement.position.z = value->value_dplacement.position.z;
    s.placement.orientation.heading = value->value_dplacement.orientation.heading;
    s.placement.orientation.pitch = value->value_dplacement.orientation.pitch;
    s.placement.orientation.roll = value->value_dplacement.orientation.roll;
    s.has_placement = true;
}

// truck.local.velocity.linear (fvector). Also update the scalar speed cache.
void SCSAPI OnTruckLocalLinearVelocity(const scs_string_t /*name*/,
                                                               const scs_u32_t /*index*/,
                                                               const scs_value_t *const value,
                                                               const scs_context_t context) {
    if (value == nullptr || value->type != SCS_VALUE_TYPE_fvector) {
        return;
    }
    auto *self = static_cast<ScsTelemetryProvider *>(context);
    TruckState &s = self->mutable_state();
    s.local_linear_velocity.x = value->value_fvector.x;
    s.local_linear_velocity.y = value->value_fvector.y;
    s.local_linear_velocity.z = value->value_fvector.z;
    s.has_speed = true;
    // Cached scalar speed. M2+ FrameLoop needs this for head-bob rate and
    // M4+ movement caps.
    const double vx = s.local_linear_velocity.x;
    const double vy = s.local_linear_velocity.y;
    const double vz = s.local_linear_velocity.z;
    s.speed_mps = sqrt(vx * vx + vy * vy + vz * vz);
}

// truck.brake.parking (bool).
void SCSAPI OnTruckParkingBrake(const scs_string_t /*name*/,
                                                     const scs_u32_t /*index*/,
                                                     const scs_value_t *const value,
                                                     const scs_context_t context) {
    if (value == nullptr || value->type != SCS_VALUE_TYPE_bool) {
        return;
    }
    auto *self = static_cast<ScsTelemetryProvider *>(context);
    TruckState &s = self->mutable_state();
    s.parking_brake = value->value_bool.value != 0;
    s.has_parking_brake = true;
}

// trailer.connected (bool). Used by M5 (enter/exit) and M9 (coupling).
void SCSAPI OnTrailerConnected(const scs_string_t /*name*/,
                                                  const scs_u32_t /*index*/,
                                                  const scs_value_t *const value,
                                                  const scs_context_t context) {
    if (value == nullptr || value->type != SCS_VALUE_TYPE_bool) {
        return;
    }
    auto *self = static_cast<ScsTelemetryProvider *>(context);
    TruckState &s = self->mutable_state();
    s.trailer_attached = value->value_bool.value != 0;
    s.has_trailer_attached = true;
}

// trailer.world.placement (dplacement). M9 trailer world pose. Used
// by the coupling provider to compute the distance from the player to
// the trailer's king-pin (front-bottom of the trailer) so the
// ApproachToKingPin waypoint tracks the actual trailer rather than a
// hard-coded 2.5 m offset behind the truck.
void SCSAPI OnTrailerWorldPlacement(const scs_string_t /*name*/,
                                                      const scs_u32_t /*index*/,
                                                      const scs_value_t *const value,
                                                      const scs_context_t context) {
    if (value == nullptr || value->type != SCS_VALUE_TYPE_dplacement) {
        return;
    }
    auto *self = static_cast<ScsTelemetryProvider *>(context);
    TruckState &s = self->mutable_state();
    s.trailer_placement.position.x = value->value_dplacement.position.x;
    s.trailer_placement.position.y = value->value_dplacement.position.y;
    s.trailer_placement.position.z = value->value_dplacement.position.z;
    s.trailer_placement.orientation.heading = value->value_dplacement.orientation.heading;
    s.trailer_placement.orientation.pitch = value->value_dplacement.orientation.pitch;
    s.trailer_placement.orientation.roll = value->value_dplacement.orientation.roll;
    s.has_trailer_placement = true;
}

// truck.fuel.amount (float, liters). M8 fuel pump readout.
void SCSAPI OnTruckFuelAmount(const scs_string_t /*name*/,
                                                  const scs_u32_t /*index*/,
                                                  const scs_value_t *const value,
                                                  const scs_context_t context) {
    if (value == nullptr || value->type != SCS_VALUE_TYPE_float) {
        return;
    }
    auto *self = static_cast<ScsTelemetryProvider *>(context);
    TruckState &s = self->mutable_state();
    s.fuel_amount_l = value->value_float.value;
    s.has_fuel = true;
}

// truck.adblue (float, liters). M8 AdBlue/DEF readout.
void SCSAPI OnTruckAdBlue(const scs_string_t /*name*/,
                                              const scs_u32_t /*index*/,
                                              const scs_value_t *const value,
                                              const scs_context_t context) {
    if (value == nullptr || value->type != SCS_VALUE_TYPE_float) {
        return;
    }
    auto *self = static_cast<ScsTelemetryProvider *>(context);
    TruckState &s = self->mutable_state();
    s.adblue_amount_l = value->value_float.value;
    s.has_adblue = true;
}

// SCSAPI is __stdcall on MSVC, default on GCC. We rely on the macro so
// the same source compiles with both. The `extern "C"` linkage here is
// not strictly needed (free functions already have C++ linkage by
// default unless declared extern "C") but it documents intent.

// M9 lander: SCS_TELEMETRY_EVENT_configuration event handler.
//
// The SDK delivers the initial truck + trailer configuration right
// after scs_telemetry_init, and any subsequent configuration change
// (e.g. player swaps trucks at a service shop) is delivered as a new
// configuration event. We register for this event in
// ScsTelemetryProvider::RegisterEvents (see below).
//
// We only care about the truck configuration. The trailer
// configuration is delivered as `trailer` and `trailer.0` ids; we
// don't pull king-pin offsets from the trailer config because the
// SDK exposes the trailer's world placement via the
// `trailer.world.placement` channel (registered in RegisterAll) —
// combining truck-local hook_position (from this event) with truck
// world placement (from the channel) gives the world king-pin
// location without an extra round-trip into trailer config.
//
// hook_position is a dvector in truck-local space: +X right of the
// cab, +Y up, +Z rear. We store it raw into TruckState::hook_position;
// CouplingProvider transforms it to world space at query time.
void SCSAPI OnTelemetryConfiguration(const scs_event_t event,
                                      const void *const event_info,
                                      const scs_context_t context) {
    if (event != SCS_TELEMETRY_EVENT_configuration || event_info == nullptr) {
        return;
    }
    const auto *cfg = static_cast<const scs_telemetry_configuration_t *>(event_info);
    if (cfg->id == nullptr || cfg->attributes == nullptr) {
        return;
    }
    // We only consume the truck configuration. The trailer config
    // (`cfg->id == "trailer"`) is left for future M9 work; the
    // channel-based trailer.world.placement already covers the
    // trailer pose needs.
    if (std::strcmp(cfg->id, SCS_TELEMETRY_CONFIG_truck) != 0) {
        return;
    }
    auto *self = static_cast<ScsTelemetryProvider *>(context);
    TruckState &s = self->mutable_state();
    for (const scs_named_value_t *attr = cfg->attributes;
         attr != nullptr && attr->name != nullptr;
         ++attr) {
        if (std::strcmp(attr->name,
                        SCS_TELEMETRY_CONFIG_ATTRIBUTE_hook_position) == 0) {
            // scs_named_value_t::value is a scs_value_t by value
            // (not a pointer), unlike the channel-callback path where
            // we receive a `const scs_value_t *const value`. The
            // configuration path hands us the value inline so the
            // SDK doesn't have to keep the storage alive past the
            // event.
            if (attr->value.type == SCS_VALUE_TYPE_dvector) {
                s.hook_position.x = attr->value.value_dvector.x;
                s.hook_position.y = attr->value.value_dvector.y;
                s.hook_position.z = attr->value.value_dvector.z;
                s.has_hook_position = true;
            }
        }
        // cabin_position and head_position would be useful for
        // future M5 (per-truck door geometry) work; we deliberately
        // do not store them here to keep the TruckState surface
        // small. When M5 lander lands, this loop is the obvious
        // place to extract them.
    }
}

}  // namespace

ScsTelemetryProvider::ScsTelemetryProvider() {
    TruckState_Clear(state_);
}

int ScsTelemetryProvider::RegisterAll(void *register_for_channel_ptr) {
    if (register_for_channel_ptr == nullptr) {
        WALKERCORE_LOG_ERROR("ScsTelemetryProvider::RegisterAll: null function pointer");
        return 0;
    }
    auto register_fn = reinterpret_cast<RegisterFn *>(register_for_channel_ptr);

    // SCS_TELEMETRY_CHANNEL_FLAG_each_frame — we want every frame even if
    // the value didn't change, so downstream can compute derivatives.
    const scs_u32_t flags = SCS_TELEMETRY_CHANNEL_FLAG_each_frame;
    int ok_count = 0;
    const scs_context_t ctx = static_cast<scs_context_t>(this);

    struct RegItem {
        const char *name;
            scs_value_type_t type;
            scs_telemetry_channel_callback_t cb;
    };

    const RegItem items[] = {
        {SCS_TELEMETRY_TRUCK_CHANNEL_world_placement,
         SCS_VALUE_TYPE_dplacement, &OnTruckWorldPlacement},
        {SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_velocity,
         SCS_VALUE_TYPE_fvector, &OnTruckLocalLinearVelocity},
        {SCS_TELEMETRY_TRUCK_CHANNEL_parking_brake,
         SCS_VALUE_TYPE_bool, &OnTruckParkingBrake},
        {SCS_TELEMETRY_TRAILER_CHANNEL_connected,
         SCS_VALUE_TYPE_bool, &OnTrailerConnected},
        // M9 trailer world placement. Registered best-effort: builds
        // that do not publish it return SCS_RESULT_unsupported_type
        // and we leave has_trailer_placement = false. The coupling
        // provider's WaypointForStep() then falls back to the truck-
        // local placeholder waypoints.
        {SCS_TELEMETRY_TRAILER_CHANNEL_world_placement,
         SCS_VALUE_TYPE_dplacement, &OnTrailerWorldPlacement},
        // M8 fuel + adblue channels. Registered best-effort: builds that
        // do not publish them return SCS_RESULT_unsupported_type and we
        // simply leave has_fuel/has_adblue = false.
        {SCS_TELEMETRY_TRUCK_CHANNEL_fuel,
         SCS_VALUE_TYPE_float, &OnTruckFuelAmount},
        {SCS_TELEMETRY_TRUCK_CHANNEL_adblue,
         SCS_VALUE_TYPE_float, &OnTruckAdBlue},
    };

    for (const auto &item : items) {
        const scs_result_t r = (*register_fn)(
            item.name, SCS_U32_NIL, item.type, flags, item.cb, ctx);
        if (r == SCS_RESULT_ok) {
            ok_count++;
        } else {
            // Most failures will be SCS_RESULT_unsupported_type when a build
            // does not provide this channel. We log a warning but continue;
            // the corresponding `has_*` flag will stay false and downstream
            // code is required to check before reading.
            WALKERCORE_LOG_WARN(
                "telemetry: failed to register channel '%s' (type %u): result %d",
                item.name, item.type, static_cast<int>(r));
        }
    }

    WALKERCORE_LOG_INFO("telemetry: registered %d/%d channels",
                        ok_count,
                        static_cast<int>(sizeof(items) / sizeof(items[0])));
    return ok_count;
}

void ScsTelemetryProvider::UnregisterAll(void *unregister_from_channel_ptr) {
    if (unregister_from_channel_ptr == nullptr) {
        return;
    }
    auto unregister_fn = reinterpret_cast<UnregisterFn *>(unregister_from_channel_ptr);

    const struct {
        const char *name;
            scs_value_type_t type;
    } items[] = {
        {SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_VALUE_TYPE_dplacement},
        {SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_velocity, SCS_VALUE_TYPE_fvector},
        {SCS_TELEMETRY_TRUCK_CHANNEL_parking_brake, SCS_VALUE_TYPE_bool},
        {SCS_TELEMETRY_TRAILER_CHANNEL_connected, SCS_VALUE_TYPE_bool},
        {SCS_TELEMETRY_TRAILER_CHANNEL_world_placement, SCS_VALUE_TYPE_dplacement},
        {SCS_TELEMETRY_TRUCK_CHANNEL_fuel, SCS_VALUE_TYPE_float},
        {SCS_TELEMETRY_TRUCK_CHANNEL_adblue, SCS_VALUE_TYPE_float},
    };

    int ok_count = 0;
    for (const auto &item : items) {
        const scs_result_t r = (*unregister_fn)(item.name, SCS_U32_NIL, item.type);
        if (r == SCS_RESULT_ok) {
            ok_count++;
        } else {
            // Unregister failures are usually because the channel wasn't
            // registered in the first place (unsupported_type on this
            // build). Log at info, not warn, to avoid noise.
            WALKERCORE_LOG_INFO(
                "telemetry: failed to unregister channel '%s' (type %u): result %d",
                item.name, item.type, static_cast<int>(r));
        }
    }
    WALKERCORE_LOG_INFO("telemetry: unregistered %d/%d channels",
                        ok_count,
                        static_cast<int>(sizeof(items) / sizeof(items[0])));
}

// M9 lander: register the configuration event callback. The SDK
// delivers the initial truck configuration right after
// scs_telemetry_init; we MUST register during init or we miss it
// (the SDK does not replay the event).
bool ScsTelemetryProvider::RegisterEvents(void *register_for_event_ptr) {
    if (register_for_event_ptr == nullptr) {
        WALKERCORE_LOG_ERROR("ScsTelemetryProvider::RegisterEvents: null function pointer");
        return false;
    }
    auto register_fn = reinterpret_cast<RegisterEventFn *>(register_for_event_ptr);
    const scs_context_t ctx = static_cast<scs_context_t>(this);
    const scs_result_t r = (*register_fn)(
        SCS_TELEMETRY_EVENT_configuration, &OnTelemetryConfiguration, ctx);
    if (r == SCS_RESULT_ok) {
        WALKERCORE_LOG_INFO("telemetry: registered configuration event callback");
        return true;
    }
    WALKERCORE_LOG_WARN(
        "telemetry: failed to register configuration event (result %d) — "
        "M9 hook_position will stay unset; coupling falls back to "
        "truck-local placeholders", static_cast<int>(r));
    return false;
}

void ScsTelemetryProvider::UnregisterEvents(void *unregister_from_event_ptr) {
    if (unregister_from_event_ptr == nullptr) {
        return;
    }
    auto unregister_fn = reinterpret_cast<UnregisterEventFn *>(unregister_from_event_ptr);
    const scs_result_t r = (*unregister_fn)(SCS_TELEMETRY_EVENT_configuration);
    if (r == SCS_RESULT_ok) {
        WALKERCORE_LOG_INFO("telemetry: unregistered configuration event callback");
    } else {
        // Unregister failures are usually because the event wasn't
        // registered in the first place. Log at info, not warn, to
        // avoid noise.
        WALKERCORE_LOG_INFO(
            "telemetry: failed to unregister configuration event (result %d)",
            static_cast<int>(r));
    }
}

}  // namespace walkercore