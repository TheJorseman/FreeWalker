// WalkerCore — FuelProvider implementation (M8).
//
// M8 Lander — partial.
//
// What IS implemented:
//   - SCS fuel + adblue channels are registered in ScsTelemetryProvider
//     and land in TruckState::fuel_amount_l / TruckState::adblue_amount_l.
//     When `truck.has_fuel` is true the provider syncs its internal
//     `fuel_amount_l_` to the telemetry value once per tick (source of
//     truth is the game).
//   - M8 Lander partial: per-truck tank capacities are read from the SCS
//     configuration event (`fuel.capacity` and `adblue.capacity` config
//     attributes) and stored in TruckState. The fuel fill clamps to the
//     real per-truck capacity instead of the honest-stub 600 L default
//     so a 380 L day-cab cannot be over-filled to 600 L.
//   - Fill rate math (cfg_.fill_rate_lps * dt → fuel_amount_l_) advances
//     toward the resolved capacity (real if the SDK delivered it,
//     otherwise the 600 L stub default).
//   - Station geometry still uses the truck-local placeholder until the
//     prefab/list_station prefab is RE'd from the game (RE-blocked;
//     requires a live debugger against amtrucks.exe to locate the
//     SCS internal prefab registry). Until that RE lands, the player
//     must park the truck in the same spot the placeholder is
//     anchored to (3.5 m truck-left, 1 m rear).

#include "walkercore/FuelPump.h"

#include <math.h>

#include <algorithm>

#include "walkercore/Log.h"
#include "walkercore/TruckDoorConfig.h"
#include "walkercore/TruckState.h"

namespace walkercore {

namespace {

// One-shot warning flag. Avoids spamming the game log every frame.
// File-scope (not function-static) so the test host can reset it via
// the extern "C" hook below, matching the convention used by the other
// honest-stub providers.
bool g_warned = false;

// Honest-stub default tank capacity. Used when the SCS configuration
// event has not delivered `fuel.capacity` (boot, pre-init, or the
// SDK call failed). 600 L is a typical 8x4 tractor — a reasonable
// fallback for the per-truck capacity gate.
constexpr double kStubFuelCapacityL = 600.0;
// AdBlue is consistently ~50 L across ATS trucks; we still gate on
// has_adblue_capacity for the per-truck override.
constexpr double kStubAdblueCapacityL = 50.0;

void WarnStub() {
    if (g_warned) return;
    g_warned = true;
    WALKERCORE_LOG_WARN(
        "FuelProvider: stub active — station position is a truck-local "
        "placeholder (M8 partial). Real station prefab list requires "
        "RE of the SCS internal prefab registry (prefab/list_station) "
        "in amtrucks.exe, which needs a live debugger to locate. "
        "Tank capacities ARE real when the SCS configuration event "
        "delivers `fuel.capacity` / `adblue.capacity` — the fill "
        "rate clamps to the per-truck value.");
}

}  // namespace

extern "C" void ResetFuelPumpStubWarnForTesting() {
    g_warned = false;
}

FuelProvider::FuelProvider() {
    station_.available = false;
    station_.world_position = {0, 0, 0};
    station_.distance_m = 0;
    station_.trigger_radius_m = cfg_.trigger_radius_m;
    tank_capacity_l_ = kStubFuelCapacityL;
    adblue_capacity_l_ = kStubAdblueCapacityL;
}

FuelProvider::~FuelProvider() = default;

void FuelProvider::SetConfig(const FuelPumpConfig &cfg) {
    cfg_ = cfg;
    station_.trigger_radius_m = cfg_.trigger_radius_m;
}

void FuelProvider::Tick(const TruckState &truck, const PlayerState &player,
                        double dt) {
    WarnStub();

    // M8 Lander partial: when SCS provides per-truck tank capacities
    // via the configuration event, replace the honest-stub defaults
    // with the real values. The fill rate below clamps to whichever
    // is in `tank_capacity_l_` at the time of the fill, so a 380 L
    // day-cab cannot be over-filled to the 600 L default.
    if (truck.has_fuel_capacity && truck.fuel_capacity_l > 0.0f) {
        tank_capacity_l_ = static_cast<double>(truck.fuel_capacity_l);
    }
    if (truck.has_adblue_capacity && truck.adblue_capacity_l > 0.0f) {
        adblue_capacity_l_ = static_cast<double>(truck.adblue_capacity_l);
    }

    // M8 partial: when SCS provides fuel + adblue amounts, sync the local
    // counters from telemetry. The local counters still own the fill-rate
    // math below (we advance them during fueling), but the baseline is
    // always the game's number.
    if (truck.has_fuel) {
        fuel_amount_l_ = static_cast<double>(truck.fuel_amount_l);
    }
    if (truck.has_adblue) {
        adblue_amount_l_ = static_cast<double>(truck.adblue_amount_l);
    }

    // Find the "nearest" fuel station. Stub: one station at
    // truck + cfg_.default_station_offset (truck-local → world).
    station_.available = false;
    station_.distance_m = 1e9f;
    if (truck.has_placement) {
        const double heading =
            static_cast<double>(truck.placement.orientation.heading) *
            (2.0 * 3.14159265358979323846);
        const Vec3 world_anchor = Vec3{
            truck.placement.position.x +
                TruckLocalToWorld(cfg_.default_station_offset, heading).x,
            truck.placement.position.y + cfg_.default_station_offset.y,
            truck.placement.position.z +
                TruckLocalToWorld(cfg_.default_station_offset, heading).z};
        station_.world_position = world_anchor;

        const double dx = player.feet_position.x - world_anchor.x;
        const double dy = player.feet_position.y - world_anchor.y;
        const double dz = player.feet_position.z - world_anchor.z;
        const double dist = sqrt(dx * dx + dy * dy + dz * dz);
        station_.distance_m = static_cast<float>(dist);
        station_.available = (dist <= static_cast<double>(cfg_.trigger_radius_m));
    }

    // If fueling, advance the fuel amount at fill_rate_lps.
    if (fueling_ && dt > 0.0) {
        fuel_amount_l_ =
            std::min(tank_capacity_l_, fuel_amount_l_ + cfg_.fill_rate_lps * dt);
        // AdBlue: a much smaller tank, but the same idea. The
        // capacity now tracks the SCS configuration event's
        // `adblue.capacity` when delivered, otherwise the 50 L
        // honest-stub default.
        adblue_amount_l_ =
            std::min(adblue_capacity_l_, adblue_amount_l_ + cfg_.adblue_rate_lps * dt);
        if (fuel_amount_l_ >= tank_capacity_l_ && adblue_amount_l_ >= adblue_capacity_l_) {
            // Both tanks full — auto-end fueling.
            fueling_ = false;
            WALKERCORE_LOG_INFO("FuelProvider: tanks full, fueling complete");
        }
    }
}

}  // namespace walkercore