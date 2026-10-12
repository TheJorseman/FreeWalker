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
#include <string.h>

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

// Common-channel headers define the channel-name string constants. The SDK
// is distributed split across common/ and per-game folders; we use
// common/ because both ETS2 and ATS share these names.
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_trailer_common_channels.h"

namespace walkercore {

namespace {

// Pull the SDK function pointer types into a private alias so the rest of
// the file can use short names. Aliases do NOT pollute the public header.
using RegisterFn = scs_telemetry_register_for_channel_t;
using UnregisterFn = scs_telemetry_unregister_from_channel_t;

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

}  // namespace walkercore