// WalkerCore — FuelProvider M8 telemetry-sync tests.
//
// Verifies the M8 partial: when TruckState has fuel_amount_l set
// (i.e. the SCS telemetry channel has fired), the provider's local
// counter is overwritten by the telemetry value. When the telemetry
// value is missing, the local counter advances from its previous value
// via the fill rate.

#include "TestHarness.h"
#include "TestStubReset.h"
#include "walkercore/FuelPump.h"
#include "walkercore/TruckState.h"

namespace walkercore {

namespace {

PlayerState MakePlayer() {
    PlayerState p;
    PlayerState_Reset(p);
    p.feet_position = {10.0, 0.0, 20.0};  // arbitrary
    return p;
}

TruckState MakeTruckWithFuel(double fuel_amount, double adblue_amount) {
    TruckState t;
    TruckState_Clear(t);
    t.fuel_amount_l = static_cast<float>(fuel_amount);
    t.adblue_amount_l = static_cast<float>(adblue_amount);
    t.has_fuel = true;
    t.has_adblue = true;
    return t;
}

}  // namespace

void TestFuelProviderSyncFromTelemetry() {
    // When telemetry reports fuel/adblue, the provider's local counters
    // match the telemetry value at the end of Tick.
    FuelProvider provider;
    provider.SetTankCapacity(600.0);
    provider.SetFuelAmount(0.0);   // stale local view of the world
    provider.SetAdblueAmount(0.0);

    TruckState t = MakeTruckWithFuel(123.5, 12.25);
    PlayerState p = MakePlayer();

    provider.Tick(t, p, 0.0);
    EXPECT_NEAR(provider.fuel_amount_l(), 123.5, 1e-4);
    EXPECT_NEAR(provider.adblue_amount_l(), 12.25, 1e-4);
}

void TestFuelProviderRetainsLocalWhenTelemetryMissing() {
    // has_fuel = false / has_adblue = false → the provider must NOT
    // overwrite its local counters with zeros. Stale-but-valid local
    // counts should be kept until telemetry arrives.
    FuelProvider provider;
    provider.SetTankCapacity(600.0);
    provider.SetFuelAmount(456.0);
    provider.SetAdblueAmount(30.0);

    TruckState t;  // default state: has_fuel / has_adblue false
    TruckState_Clear(t);
    PlayerState p = MakePlayer();

    provider.Tick(t, p, 0.0);
    EXPECT_NEAR(provider.fuel_amount_l(), 456.0, 1e-4);
    EXPECT_NEAR(provider.adblue_amount_l(), 30.0, 1e-4);
}

void TestFuelProviderFuelingAdvancesAfterTelemetry() {
    // Sequence: tick once with telemetry to seed the counter, then
    // begin fueling, then tick without telemetry — fill rate still
    // applies from the seeded value.
    FuelProvider provider;
    provider.SetTankCapacity(600.0);
    provider.SetFuelAmount(0.0);
    provider.SetAdblueAmount(0.0);

    FuelPumpConfig cfg;
    cfg.fill_rate_lps = 10.0;     // 10 L/s
    cfg.adblue_rate_lps = 1.0;
    provider.SetConfig(cfg);

    TruckState t_full = MakeTruckWithFuel(100.0, 5.0);
    PlayerState p = MakePlayer();
    provider.Tick(t_full, p, 0.0);

    // Begin fueling (the public API is BeginFueling / EndFueling — if
    // they exist; otherwise we mutate via the implementation detail).
    // We rely on Tick(dt > 0) advancing fuel_amount_l_ when fueling_ is
    // true; the production provider exposes StartFueling/StopFueling.
    provider.StartFueling();

    // Tick for 1 second without telemetry — fill rate still applies.
    TruckState t_empty;  // has_fuel / has_adblue default false
    TruckState_Clear(t_empty);
    provider.Tick(t_empty, p, 1.0);

    // 100.0 (seeded) + 10.0 (1s * 10 L/s) = 110.0
    EXPECT_NEAR(provider.fuel_amount_l(), 110.0, 1e-3);
}

// -----------------------------------------------------------------------------
// FuelPump Tick-then-read contract.
//
// FuelPump::Tick() OVERWRITES the internal station_ on every call
// (it recomputes in-range based on the new truck and player positions).
// Consumers that read station() for a decision on the same frame MUST
// call Tick() FIRST — otherwise the const reference returned by
// station() is bound to the PREVIOUS frame's station_ state, and a
// player who walks INTO the station and presses F on the same frame
// would see station.available == false from last frame's Tick and
// the StartFueling branch would not fire.
//
// The per-frame conductor used to read station() first (one-frame
// stale) and call Tick() afterwards. This test pins the corrected
// order (Tick-then-read) for the same-frame-arrive scenario.
// -----------------------------------------------------------------------------

void TestFuelStationReadAfterTickReflectsCurrentPlayerPosition() {
    ResetFuelPumpStubWarnForTesting();

    FuelProvider provider;
    provider.SetTankCapacity(600.0);
    provider.SetFuelAmount(0.0);
    provider.SetAdblueAmount(0.0);

    FuelPumpConfig cfg;
    cfg.fill_rate_lps = 10.0;
    cfg.adblue_rate_lps = 1.0;
    cfg.trigger_radius_m = 2.5f;
    provider.SetConfig(cfg);

    // Truck at the origin. Default fuel-station offset = (-3.5, 0, 1.0)
    // → station anchor at (-3.5, 0, 1.0) in world space.
    TruckState truck = MakeTruckWithFuel(100.0, 5.0);
    truck.placement.position = Vec3{0, 0, 0};
    truck.placement.orientation.heading = 0.0;
    truck.placement.orientation.pitch = 0.0;
    truck.placement.orientation.roll = 0.0;
    truck.has_placement = true;

    // Player FAR from the station. Prime one Tick to seed the
    // station_=false state.
    PlayerState player = MakePlayer();
    player.feet_position = Vec3{10, 0, 10};  // far away
    provider.Tick(truck, player, 0.016);
    EXPECT_FALSE(provider.station().available);

    // Now move the player INTO the station's trigger radius. Without
    // calling Tick again, station().available is still false (stale
    // from the previous frame's Tick).
    player.feet_position = Vec3{-3.5, 0, 1.0};  // exact station anchor
    EXPECT_FALSE(provider.station().available);

    // CORRECT conductor pattern: Tick FIRST, then read station(). The
    // Tick recomputes station_ based on the player's new position;
    // the subsequent station() read sees the fresh, in-range state.
    provider.Tick(truck, player, 0.016);
    const FuelStationState &station = provider.station();
    EXPECT_TRUE(station.available);
    EXPECT_LE(station.distance_m, 0.5);  // should be ~0 at the anchor

    // Drive the StartFueling branch — should succeed on the same frame
    // the player arrived at the station, no need to wait one frame.
    EXPECT_FALSE(provider.isFueling());
    provider.StartFueling();
    EXPECT_TRUE(provider.isFueling());

    // -------------------------------------------------------------------------
    // Negative control: the BUGGY pre-fix pattern (read station() BEFORE
    // Tick()) MUST still see a stale value, confirming the contract.
    //
    // Reset the player far away and Tick once to clear station_.
    // -------------------------------------------------------------------------
    player.feet_position = Vec3{10, 0, 10};
    provider.Tick(truck, player, 0.016);
    EXPECT_FALSE(provider.station().available);

    // Move player INTO range. If we read station() BEFORE calling Tick,
    // we see the previous frame's value (false). This is the bug the
    // pre-fix conductor exhibited — the cached ref binds to station_
    // which still holds the previous frame's value.
    player.feet_position = Vec3{-3.5, 0, 1.0};
    // No Tick call between move and read — this is the buggy ordering.
    EXPECT_FALSE(provider.station().available);

    // After Tick(), station() sees the fresh value. (Conductor's fix.)
    provider.Tick(truck, player, 0.016);
    EXPECT_TRUE(provider.station().available);
}

// -----------------------------------------------------------------------------
// M8 Lander: per-truck tank capacities from the SCS configuration event.
//
// The ScsTelemetryProvider parses `fuel.capacity` and `adblue.capacity`
// (delivered as SCS_TELEMETRY_CONFIG_ATTRIBUTE_* during
// SCS_TELEMETRY_EVENT_configuration) and stores them in
// TruckState::fuel_capacity_l / adblue_capacity_l. FuelProvider's
// Tick consumes those values and replaces the honest-stub 600 L
// (fuel) / 50 L (adblue) defaults with the per-truck numbers. These
// probes pin the contract:
//   1. With has_fuel_capacity = true, the next Tick after a config
//      event has the provider report the per-truck capacity.
//   2. The fill rate clamps to the real per-truck capacity so a
//      380 L day-cab cannot be over-filled to the 600 L default.
//   3. With has_fuel_capacity = false, the stub 600 L default
//      holds (the M8 stub contract).
//   4. Symmetric probes for AdBlue (380 → 600 L clamp + 50 L
//      default).
// -----------------------------------------------------------------------------

void TestFuelProviderCapacityFromConfigEventOverridesStub() {
    ResetFuelPumpStubWarnForTesting();
    FuelProvider provider;
    // Pre-condition: stub default is 600 L fuel, 50 L adblue.
    EXPECT_NEAR(provider.tank_capacity_l(), 600.0, 1e-6);
    EXPECT_NEAR(provider.adblue_capacity_l(), 50.0, 1e-6);

    TruckState truck;
    TruckState_Clear(truck);
    // Simulate the SCS configuration event delivering per-truck
    // capacities (a Volvo VNL day-cab, 380 L fuel + 50 L adblue).
    truck.fuel_capacity_l = 380.0f;
    truck.has_fuel_capacity = true;
    truck.adblue_capacity_l = 50.0f;
    truck.has_adblue_capacity = true;

    PlayerState player = MakePlayer();
    provider.Tick(truck, player, 0.016);
    // The provider's tank_capacity_l_ now reflects the per-truck
    // value. AdBlue stays at 50 L because the SDK's value matches
    // the stub default; we still verify the wiring by reading
    // through.
    EXPECT_NEAR(provider.tank_capacity_l(), 380.0, 1e-6);
    EXPECT_NEAR(provider.adblue_capacity_l(), 50.0, 1e-6);
}

void TestFuelProviderFuelingClampsToRealPerTruckCapacity() {
    // The M8 Lander contract: the fill rate clamps to the per-truck
    // capacity, not the 600 L stub default. A 380 L day-cab filled
    // at 100 L/s must stop at 380 L, not 600 L.
    //
    // Pattern: tick WITH telemetry once to seed the local counter
    // and apply the per-truck capacity, then tick WITHOUT telemetry
    // (so the local counter is not overwritten each frame) to
    // accumulate the fill. This matches the production behaviour
    // where the SCS configuration event fires once per truck swap
    // (capacity stable) and the per-frame fuel channel reports
    // each frame.
    ResetFuelPumpStubWarnForTesting();
    FuelProvider provider;
    TruckState truck;
    TruckState_Clear(truck);
    truck.fuel_capacity_l = 380.0f;
    truck.has_fuel_capacity = true;
    truck.adblue_capacity_l = 50.0f;
    truck.has_adblue_capacity = true;
    // Seed fuel level at 0 via telemetry on the first tick.
    truck.fuel_amount_l = 0.0f;
    truck.has_fuel = true;
    truck.adblue_amount_l = 0.0f;
    truck.has_adblue = true;

    PlayerState player = MakePlayer();
    // Tick 1: WITH telemetry → seeds local counter from 0 and
    // applies per-truck capacity.
    provider.Tick(truck, player, 0.0);
    EXPECT_NEAR(provider.tank_capacity_l(), 380.0, 1e-6);
    EXPECT_NEAR(provider.fuel_amount_l(), 0.0, 1e-6);

    FuelPumpConfig cfg;
    cfg.fill_rate_lps = 100.0;  // 100 L/s — fills 380 L in 3.8 s
    cfg.adblue_rate_lps = 10.0;
    cfg.trigger_radius_m = 2.5f;
    provider.SetConfig(cfg);
    provider.StartFueling();

    // Subsequent ticks WITHOUT telemetry so the local counter
    // accumulates (production code path: the SCS configuration
    // event fires once and the per-frame fuel channel keeps
    // reporting, so the local counter advances monotonically).
    TruckState truck_no_telemetry;
    TruckState_Clear(truck_no_telemetry);
    for (int i = 0; i < 5; ++i) {
        provider.Tick(truck_no_telemetry, player, 1.0);
    }
    EXPECT_NEAR(provider.fuel_amount_l(), 380.0, 1e-6);
    // Auto-stop when full.
    EXPECT_FALSE(provider.isFueling());
    // AdBlue is at 50 L capacity * 10 L/s = full in 5 s.
    EXPECT_NEAR(provider.adblue_amount_l(), 50.0, 1e-6);
}

void TestFuelProviderStubCapacityHoldsWithoutConfigEvent() {
    // M8 stub contract: with has_fuel_capacity = false (the
    // configuration event has not fired yet — boot, pre-init, or
    // the SDK call failed), the honest-stub defaults of 600 L fuel
    // / 50 L adblue remain. The fill rate clamps to 600 L, not
    // the per-truck value.
    ResetFuelPumpStubWarnForTesting();
    FuelProvider provider;
    TruckState truck;
    TruckState_Clear(truck);
    // has_fuel_capacity and has_adblue_capacity stay false.
    // The fuel level is reported via telemetry so the
    // local counter is non-zero after the first Tick.
    truck.fuel_amount_l = 0.0f;
    truck.has_fuel = true;
    truck.adblue_amount_l = 0.0f;
    truck.has_adblue = true;

    PlayerState player = MakePlayer();
    provider.Tick(truck, player, 0.0);
    EXPECT_NEAR(provider.tank_capacity_l(), 600.0, 1e-6);
    EXPECT_NEAR(provider.adblue_capacity_l(), 50.0, 1e-6);

    FuelPumpConfig cfg;
    cfg.fill_rate_lps = 200.0;  // would fill 600 L in 3 s
    cfg.adblue_rate_lps = 50.0;  // would fill 50 L in 1 s
    cfg.trigger_radius_m = 2.5f;
    provider.SetConfig(cfg);
    provider.StartFueling();
    // Subsequent ticks WITHOUT telemetry so the local counter
    // accumulates.
    TruckState truck_no_telemetry;
    TruckState_Clear(truck_no_telemetry);
    for (int i = 0; i < 4; ++i) {
        provider.Tick(truck_no_telemetry, player, 1.0);
    }
    EXPECT_NEAR(provider.fuel_amount_l(), 600.0, 1e-6);
    EXPECT_FALSE(provider.isFueling());
    EXPECT_NEAR(provider.adblue_amount_l(), 50.0, 1e-6);
}

void TestFuelProviderAdblueCapacityFromConfigEventOverridesStub() {
    // Symmetric probe for AdBlue: a hypothetical SCS build that
    // reports a 25 L DEF tank (light-duty truck) must clamp the
    // fill to 25 L, not 50 L.
    ResetFuelPumpStubWarnForTesting();
    FuelProvider provider;
    TruckState truck;
    TruckState_Clear(truck);
    truck.fuel_capacity_l = 600.0f;
    truck.has_fuel_capacity = true;
    truck.adblue_capacity_l = 25.0f;
    truck.has_adblue_capacity = true;
    truck.adblue_amount_l = 0.0f;
    truck.has_adblue = true;

    PlayerState player = MakePlayer();
    provider.Tick(truck, player, 0.0);
    EXPECT_NEAR(provider.adblue_capacity_l(), 25.0, 1e-6);

    FuelPumpConfig cfg;
    cfg.fill_rate_lps = 1.0;
    cfg.adblue_rate_lps = 10.0;  // would fill 25 L in 2.5 s
    cfg.trigger_radius_m = 2.5f;
    provider.SetConfig(cfg);
    provider.StartFueling();
    // Subsequent ticks WITHOUT telemetry so the local counter
    // accumulates.
    TruckState truck_no_telemetry;
    TruckState_Clear(truck_no_telemetry);
    for (int i = 0; i < 4; ++i) {
        provider.Tick(truck_no_telemetry, player, 1.0);
    }
    // 4 s at 10 L/s = 40 L attempted; clamps at 25 L.
    EXPECT_NEAR(provider.adblue_amount_l(), 25.0, 1e-6);
    // The fuel side runs at 1 L/s for 4 s = 4 L (well under 600 L
    // capacity); confirm it stays small.
    EXPECT_NEAR(provider.fuel_amount_l(), 4.0, 1e-6);
}

}  // namespace walkercore