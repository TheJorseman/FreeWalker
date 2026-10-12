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
//   - Fill rate math (cfg_.fill_rate_lps * dt → fuel_amount_l_) advances
//     toward tank_capacity_l_ while fueling.
//   - Station geometry still uses the truck-local placeholder until the
//     prefab/list_station config callback is wired (separate M8 step).

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

void WarnStub() {
    if (g_warned) return;
    g_warned = true;
    WALKERCORE_LOG_WARN(
        "FuelProvider: stub active — station position is a truck-local "
        "placeholder (M8 partial). Real station prefab list lands via "
        "SCS config callback (prefab/list_station) once the SDK "
        "negotiation includes it.");
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
}

FuelProvider::~FuelProvider() = default;

void FuelProvider::SetConfig(const FuelPumpConfig &cfg) {
    cfg_ = cfg;
    station_.trigger_radius_m = cfg_.trigger_radius_m;
}

void FuelProvider::Tick(const TruckState &truck, const PlayerState &player,
                        double dt) {
    WarnStub();

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
        // AdBlue: a much smaller tank, but the same idea.
        const double adblue_capacity_l = 50.0;
        adblue_amount_l_ =
            std::min(adblue_capacity_l, adblue_amount_l_ + cfg_.adblue_rate_lps * dt);
        if (fuel_amount_l_ >= tank_capacity_l_ && adblue_amount_l_ >= adblue_capacity_l) {
            // Both tanks full — auto-end fueling.
            fueling_ = false;
            WALKERCORE_LOG_INFO("FuelProvider: tanks full, fueling complete");
        }
    }
}

}  // namespace walkercore