// WalkerCore — ScsTelemetryProvider probes.
//
// ScsTelemetryProvider is the I/O layer between the SCS SDK callbacks
// (C-style free functions, declared in src/provider/ScsTelemetryProvider.cpp)
// and the WalkerCore domain (TruckState snapshot, read-only). It is the
// ONLY provider that had zero direct test coverage before this commit —
// every other module has a field-exhaustive probe or a stub-warn probe.
// The file here pins:
//
//   1. Default state invariants:
//        - Every has_* (placement, speed, parking brake, trailer, fuel,
//          adblue) is false on a fresh provider.
//        - The snapshot's vector/orientation fields are zero-initialised
//          (no NaN, no garbage, no leftover stack data from later
//          allocations).
//
//   2. Reference-stability contract:
//        - The const state() accessor returns the same address across
//          calls, and writes through mutable_state() are visible through
//          subsequent const state() reads.
//
//   3. Null-pointer safety:
//        - RegisterAll(nullptr) returns 0 and does not crash. PluginEntry
//          can call this defensively during a malformed scs_telemetry_init.
//        - UnregisterAll(nullptr) is a safe no-op and does not mutate
//          state_.
//
// We cannot exercise the real SCS SDK callbacks here (no game, no SDK
// runtime), but we CAN pin everything about the provider's static
// shape. Anything dynamic (the SDK actually invoking OnTruckWorldPlacement
// and our callback updating state_) requires the SDK loop and is
// covered indirectly by tests/test_diagnostic_dump.cpp (which exercises
// the full DiagnosticDump stack that reads the TruckState mirror).
//
// Note: the SCS channel-name macros (SCS_TELEMETRY_TRUCK_CHANNEL_*, etc.)
// are NOT covered here — they're pulled in only by the provider .cpp
// via the common channel headers, and adding the SDK include to the
// test exe is more invasive than the assertion is worth. The provider
// itself would fail to compile if any of those macros went missing,
// so the channel-name stability is effectively pinned by the build.

#include "TestHarness.h"

#include "walkercore/ScsTelemetryProvider.h"
#include "walkercore/TruckState.h"

namespace walkercore {

// -------------------------------------------------------------------------
// Default-state invariants
// -------------------------------------------------------------------------

void TestScsTelemetryProviderFreshStateAllHasFlagsAreFalse() {
    // After construction but before any Register / callback, every
    // `has_*` flag must be false. Downstream code (WalkerFsm, FrameLoop,
    // KnockoutProvider, FuelProvider, etc.) branches on these flags.
    // A regression that initialised one to true (e.g. by initialising
    // placement to a non-zero default) would silently treat the truck
    // as "has telemetry" before any callback fired.
    ScsTelemetryProvider provider;
    const TruckState &s = provider.state();
    EXPECT_FALSE(s.has_placement);
    EXPECT_FALSE(s.has_speed);
    EXPECT_FALSE(s.has_parking_brake);
    EXPECT_FALSE(s.has_trailer_attached);
    EXPECT_FALSE(s.has_fuel);
    EXPECT_FALSE(s.has_adblue);
}

void TestScsTelemetryProviderFreshStateScalarFieldsAreZero() {
    // The placement / velocity / etc. fields are POD; verify they start
    // zeroed so no random stack residue leaks into the camera math when
    // the first SDK callback arrives mid-frame.
    ScsTelemetryProvider provider;
    const TruckState &s = provider.state();

    EXPECT_EQ(s.placement.position.x, 0.0);
    EXPECT_EQ(s.placement.position.y, 0.0);
    EXPECT_EQ(s.placement.position.z, 0.0);
    EXPECT_EQ(s.placement.orientation.heading, 0.0f);
    EXPECT_EQ(s.placement.orientation.pitch, 0.0f);
    EXPECT_EQ(s.placement.orientation.roll, 0.0f);

    EXPECT_EQ(s.local_linear_velocity.x, 0.0f);
    EXPECT_EQ(s.local_linear_velocity.y, 0.0f);
    EXPECT_EQ(s.local_linear_velocity.z, 0.0f);
    EXPECT_EQ(s.speed_mps, 0.0);

    EXPECT_FALSE(s.parking_brake);
    EXPECT_FALSE(s.trailer_attached);
    EXPECT_EQ(s.fuel_amount_l, 0.0f);
    EXPECT_EQ(s.adblue_amount_l, 0.0f);
    EXPECT_EQ(s.last_update_us, 0ull);
}

void TestScsTelemetryProviderStateIsStableAcrossConstAccess() {
    // The const state() accessor must return a stable reference. Calling
    // it twice in a row returns the same address (same instance field),
    // and writes via mutable_state() are visible through subsequent
    // const state() reads. This pins the contract that the provider
    // doesn't accidentally copy the snapshot on each access.
    ScsTelemetryProvider provider;
    const TruckState &s1 = provider.state();
    const TruckState &s2 = provider.state();
    EXPECT_EQ(&s1, &s2);

    // Write through mutable_state(); the const accessor must see it.
    provider.mutable_state().speed_mps = 12.5;
    EXPECT_EQ(provider.state().speed_mps, 12.5);

    // has_* flags are user-writable through mutable_state() too.
    provider.mutable_state().has_speed = true;
    EXPECT_TRUE(provider.state().has_speed);
}

// -------------------------------------------------------------------------
// Null-pointer safety
// -------------------------------------------------------------------------

void TestScsTelemetryProviderRegisterAllNullIsSafe() {
    // PluginEntry may pass the SDK function pointer through multiple
    // layers; if it ever passes nullptr (e.g. SDK init failed), the
    // provider must log an error and return 0 without dereferencing.
    ScsTelemetryProvider provider;
    const int ok_count = provider.RegisterAll(nullptr);
    EXPECT_EQ(ok_count, 0);
    // State stays at defaults (no callbacks fired).
    EXPECT_FALSE(provider.state().has_placement);
}

void TestScsTelemetryProviderUnregisterAllNullIsSafe() {
    // The shutdown path may run with a nullptr SDK function pointer if
    // init never completed. The unregister path must be a safe no-op.
    ScsTelemetryProvider provider;
    // Mutate to detect any accidental re-touch of state.
    provider.mutable_state().speed_mps = 7.5;
    provider.mutable_state().has_speed = true;

    provider.UnregisterAll(nullptr);

    // State must be untouched by the null unregister.
    EXPECT_EQ(provider.state().speed_mps, 7.5);
    EXPECT_TRUE(provider.state().has_speed);
}

// -------------------------------------------------------------------------
// Channel-list stability note (covered by the build, not by a runtime
// assertion). The provider .cpp includes
//   common/scssdk_telemetry_truck_common_channels.h
//   common/scssdk_telemetry_trailer_common_channels.h
// and references SCS_TELEMETRY_TRUCK_CHANNEL_world_placement,
// SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_velocity,
// SCS_TELEMETRY_TRUCK_CHANNEL_parking_brake,
// SCS_TELEMETRY_TRAILER_CHANNEL_connected,
// SCS_TELEMETRY_TRUCK_CHANNEL_fuel,
// SCS_TELEMETRY_TRUCK_CHANNEL_adblue
// directly. If any of those macros disappear from a future SCS SDK
// update, the provider .cpp fails to compile — that's a stronger
// pin than any runtime check could provide.
// -------------------------------------------------------------------------

}  // namespace walkercore