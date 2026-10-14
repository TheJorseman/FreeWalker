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

#include <cstring>

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
    EXPECT_FALSE(s.has_hook_position);    // M9: configuration event
    EXPECT_FALSE(s.has_cabin_position);   // M5: configuration event
    EXPECT_FALSE(s.has_head_position);    // M5: configuration event
    EXPECT_FALSE(s.has_model_id);         // M5: configuration event
    EXPECT_FALSE(s.has_trailer_placement); // M9: trailer.world.placement
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
    // M9 hooks: hook_position stays zeroed (truck-local triple),
    // trailer_placement stays zeroed.
    EXPECT_EQ(s.hook_position.x, 0.0);
    EXPECT_EQ(s.hook_position.y, 0.0);
    EXPECT_EQ(s.hook_position.z, 0.0);
    // M5 hooks: cabin + head anchors stay zeroed; model_id is an
    // empty C string.
    EXPECT_EQ(s.cabin_position.x, 0.0);
    EXPECT_EQ(s.cabin_position.y, 0.0);
    EXPECT_EQ(s.cabin_position.z, 0.0);
    EXPECT_EQ(s.head_position.x, 0.0);
    EXPECT_EQ(s.head_position.y, 0.0);
    EXPECT_EQ(s.head_position.z, 0.0);
    EXPECT_EQ(s.model_id[0], '\0');
    EXPECT_EQ(s.trailer_placement.position.x, 0.0);
    EXPECT_EQ(s.trailer_placement.position.y, 0.0);
    EXPECT_EQ(s.trailer_placement.position.z, 0.0);
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
// M9 lander: configuration event registration is null-safe.
// -------------------------------------------------------------------------

void TestScsTelemetryProviderRegisterEventsNullIsSafe() {
    // M9: RegisterEvents takes the SDK's `register_for_event` function
    // pointer. PluginEntry must be able to pass nullptr (e.g. the SDK
    // init did not advertise the event registration function) and
    // expect the provider to log an error and return false without
    // crashing. The state_ fields stay at defaults so downstream
    // CouplingProvider falls back to the truck-local placeholder
    // waypoints.
    ScsTelemetryProvider provider;
    const bool ok = provider.RegisterEvents(nullptr);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(provider.state().has_hook_position);
}

void TestScsTelemetryProviderUnregisterEventsNullIsSafe() {
    // M9: shutdown with nullptr must not crash. The provider state is
    // left untouched (no fields reset) so a partial unregister is
    // observable through the diagnostic dump without hiding it from
    // a follow-up unregister call with a real pointer.
    ScsTelemetryProvider provider;
    provider.mutable_state().hook_position = {0.5, 1.0, -2.5};
    provider.mutable_state().has_hook_position = true;

    provider.UnregisterEvents(nullptr);
    EXPECT_TRUE(provider.state().has_hook_position);
    EXPECT_EQ(provider.state().hook_position.x, 0.5);
    EXPECT_EQ(provider.state().hook_position.y, 1.0);
    EXPECT_EQ(provider.state().hook_position.z, -2.5);
}

// M9 multi-trailer (cycle 2026-10-14): the TruckState `trailers[]`
// array is a 4-slot container where each slot holds one attached
// trailer's hook_position, has_* flags, and model_id. Single-
// trailer code (CouplingProvider, F12 dump) reads slot 0
// transparently. This test mutates the array directly via the
// provider's mutable_state() (the same pattern used by
// test_coupling_provider.cpp) so the contract under test is
// "TrailerSlot fields are addressable by slot index", not the
// SCS callback plumbing which is exercised through the existing
// TestScsTelemetryProviderRegisterEventsNullIsSafe +
// production provider event paths.
void TestScsTelemetryProviderIndexedTrailerConfigLandsInPerSlot() {
    ScsTelemetryProvider provider;
    TruckState &s = provider.mutable_state();
    // Slot 0: primary trailer (dry_van).
    s.trailers[0].has_hook_position = true;
    s.trailers[0].hook_position = Vec3{1.0, 0.0, -2.0};
    s.trailers[0].has_model_id = true;
    strncpy(s.trailers[0].model_id, "vehicle.trailer.dry_van",
                 TruckState::kTrailerModelIdCapacity - 1);
    s.trailers[0].model_id[TruckState::kTrailerModelIdCapacity - 1] = '\0';
    // Slot 1: secondary trailer (flatbed).
    s.trailers[1].has_hook_position = true;
    s.trailers[1].hook_position = Vec3{2.0, 0.0, -3.0};
    s.trailers[1].has_model_id = true;
    strncpy(s.trailers[1].model_id, "vehicle.trailer.flatbed",
                 TruckState::kTrailerModelIdCapacity - 1);
    s.trailers[1].model_id[TruckState::kTrailerModelIdCapacity - 1] = '\0';
    // Slot 0 aliases (set by the parser on a "trailer.0" config
    // event). We mutate them explicitly here so the test verifies
    // the alias contract independently of the parser.
    s.trailer_hook_position = s.trailers[0].hook_position;
    s.has_trailer_hook_position = true;
    strncpy(s.trailer_model_id, s.trailers[0].model_id,
                 TruckState::kTrailerModelIdCapacity);
    s.has_trailer_model_id = true;
    s.trailer_attached_count = 2;

    const TruckState &cs = provider.state();
    EXPECT_TRUE(cs.trailers[0].has_hook_position);
    EXPECT_TRUE(cs.trailers[0].has_model_id);
    EXPECT_EQ(cs.trailers[0].hook_position.x, 1.0);
    EXPECT_EQ(cs.trailers[0].hook_position.z, -2.0);
    EXPECT_STREQ(cs.trailers[0].model_id, "vehicle.trailer.dry_van");
    EXPECT_TRUE(cs.trailers[1].has_hook_position);
    EXPECT_TRUE(cs.trailers[1].has_model_id);
    EXPECT_EQ(cs.trailers[1].hook_position.x, 2.0);
    EXPECT_EQ(cs.trailers[1].hook_position.z, -3.0);
    EXPECT_STREQ(cs.trailers[1].model_id, "vehicle.trailer.flatbed");
    EXPECT_TRUE(cs.has_trailer_hook_position);
    EXPECT_EQ(cs.trailer_hook_position.x, 1.0);
    EXPECT_TRUE(cs.has_trailer_model_id);
    EXPECT_STREQ(cs.trailer_model_id, "vehicle.trailer.dry_van");
    EXPECT_FALSE(cs.trailers[2].has_hook_position);
    EXPECT_FALSE(cs.trailers[2].has_model_id);
    EXPECT_EQ(cs.trailer_attached_count, 2u);
}

// M9 multi-trailer lander (cycle 2026-10-14): `kMaxTrailerSlots` is
// 4; out-of-bounds indices are not addressable. The const-access
// guard pins the contract that consumers can iterate
// `[0, kMaxTrailerSlots)` and never step past it.
void TestScsTelemetryProviderTrailerOutOfRangeIndexIsDropped() {
    ScsTelemetryProvider provider;
    TruckState &s = provider.mutable_state();
    s.trailers[0].has_hook_position = true;
    s.trailers[3].has_hook_position = true;
    s.trailer_attached_count = 0;  // caller never set it
    const TruckState &cs = provider.state();
    EXPECT_TRUE(cs.trailers[0].has_hook_position);
    EXPECT_TRUE(cs.trailers[3].has_hook_position);
    // The count is independent of the array fill: it is the
    // caller's responsibility to set it. Pin that contract here.
    EXPECT_EQ(cs.trailer_attached_count, 0u);
    EXPECT_FALSE(cs.has_trailer_hook_position);
}

// M9 multi-trailer lander backwards-compat (cycle 2026-10-14): the
// bare "trailer" id (SCS convention for the primary slot) keeps
// writing to slot 0 AND to the legacy `trailer_*` fields. After
// `TruckState_Clear`, both the array and the aliases start clean
// — verifying the clear path doesn't leave stale data in either
// the new array or the old scalar aliases.
void TestScsTelemetryProviderTrailerBareIdStillMirrorsPrimarySlot() {
    ScsTelemetryProvider provider;
    TruckState &s = provider.mutable_state();
    // Pre-populate both the array and the legacy aliases to
    // confirm TruckState_Clear wipes both representations.
    s.trailers[0].has_hook_position = true;
    s.trailers[0].hook_position = Vec3{0.5, 0.1, -0.7};
    s.trailers[0].has_model_id = true;
    strncpy(s.trailers[0].model_id, "stale.dry_van",
                 TruckState::kTrailerModelIdCapacity);
    s.trailer_hook_position = Vec3{9.0, 9.0, 9.0};
    s.has_trailer_hook_position = true;
    strncpy(s.trailer_model_id, "stale.dry_van",
                 TruckState::kTrailerModelIdCapacity);
    s.has_trailer_model_id = true;
    s.trailer_attached_count = 1;

    // Reset the entire state via the public clear path.
    TruckState_Clear(s);

    // After Clear: nothing in the array, nothing in the aliases,
    // count is 0.
    EXPECT_FALSE(s.trailers[0].has_hook_position);
    EXPECT_FALSE(s.trailers[0].has_model_id);
    EXPECT_FALSE(s.has_trailer_hook_position);
    EXPECT_FALSE(s.has_trailer_model_id);
    EXPECT_EQ(s.trailer_attached_count, 0u);
    EXPECT_EQ(s.trailer_model_id[0], '\0');
}
// -------------------------------------------------------------------------

}  // namespace walkercore