// WalkerCore — Fuel pump (M8).
//
// Domain logic for refuelling the truck from a fuel station while on foot.
// Real impl reads:
//   - Fuel station positions from the game's prefab list (or memory scan).
//   - Truck fuel amount from telemetry channel `truck.fuel.capacity` and
//     `truck.fuel.amount` (SCS SDK 1.15).
//   - AdBlue/DEF amount from `truck.adblue.capacity` / `truck.adblue.amount`.
//
// Stub impl: simulates a fuel station at a configurable position relative
// to the truck (or absolute world coords), fills the truck tank at a fixed
// rate when "fueling", and exposes a domain API for the UI to read.

#ifndef WALKERCORE_FUEL_PUMP_H
#define WALKERCORE_FUEL_PUMP_H

#include "walkercore/PlayerState.h"
#include "walkercore/TruckState.h"

namespace walkercore {

// State of the nearest fuel station relative to the player.
struct FuelStationState {
    bool available;            // a station is within range
    Vec3 world_position;       // station anchor (where the player stands)
    float distance_m;          // distance from player to station anchor
    float trigger_radius_m;    // how close the player must be
};

// Configuration. Defaults match TMRealWalk's published rates.
struct FuelPumpConfig {
    // Litres per second when fueling. Real trucks take ~1.5 L/s on the fast
    // pump; TMRealWalk caps at ~1.6 L/s for realism.
    double fill_rate_lps = 1.5;
    // Same for AdBlue / DEF.
    double adblue_rate_lps = 0.05;
    // Distance at which the player can use the pump.
    float trigger_radius_m = 2.5f;
    // Default station offset from the truck (truck-local). Used by the
    // stub when no station positions are available.
    Vec3 default_station_offset = {-3.5, 0.0, 1.0};
};

class FuelProvider {
public:
    FuelProvider();
    ~FuelProvider();

    FuelProvider(const FuelProvider &) = delete;
    FuelProvider &operator=(const FuelProvider &) = delete;

    void SetConfig(const FuelPumpConfig &cfg);
    const FuelPumpConfig &config() const { return cfg_; }

    // One frame: find the nearest fuel station, update `station`, advance
    // fuel fill if isFueling(). Mutates truck's fuel amount in place via
    // a separate channel when SCS is wired; for the stub we mutate
    // `out_truck->placement.fuel_amount` if the field exists in future.
    void Tick(const TruckState &truck, const PlayerState &player, double dt);

    const FuelStationState &station() const { return station_; }

    bool isFueling() const { return fueling_; }
    void StartFueling() { fueling_ = true; }
    void StopFueling() { fueling_ = false; }

    // Cargo peak tank. Read from telemetry when wired; the stub uses 600 L
    // (a typical 8x4 tractor).
    double tank_capacity_l() const { return tank_capacity_l_; }
    void SetTankCapacity(double l) { tank_capacity_l_ = l; }
    double fuel_amount_l() const { return fuel_amount_l_; }
    void SetFuelAmount(double l) { fuel_amount_l_ = l; }
    double adblue_amount_l() const { return adblue_amount_l_; }
    void SetAdblueAmount(double l) { adblue_amount_l_ = l; }
    // M8 Lander: AdBlue/DEF tank capacity. Mirrors the SCS
    // configuration event's `adblue.capacity` when delivered;
    // honest-stub default is 50 L. Symmetric with the fuel
    // capacity accessor above.
    double adblue_capacity_l() const { return adblue_capacity_l_; }
    void SetAdblueCapacity(double l) { adblue_capacity_l_ = l; }

private:
    FuelPumpConfig cfg_;
    FuelStationState station_;
    bool fueling_ = false;
    // M8 Lander: real per-truck tank capacities from the SCS
    // configuration event. Defaults are the honest-stub fallbacks
    // (600 L fuel, 50 L adblue) until the SDK delivers the per-
    // truck value.
    double tank_capacity_l_ = 600.0;
    double adblue_capacity_l_ = 50.0;
    double fuel_amount_l_ = 0.0;
    double adblue_amount_l_ = 0.0;
};

}  // namespace walkercore

#endif  // WALKERCORE_FUEL_PUMP_H