// WalkerCore — TruckDoorConfigFromTruckState probes (M5 lander partial).
//
// Pins the M5 Lander partial: when the SCS configuration event has
// delivered the per-truck cabin + head anchors, the door config
// builder must produce a config whose driver_door and cabin
// bounding box are anchored to the per-truck cabin_position. The
// default placeholder (DefaultTruckDoorConfig) is used as a
// fallback when the configuration event has not fired.

#include "TestHarness.h"

#include "walkercore/TruckDoorConfig.h"
#include "walkercore/TruckState.h"

namespace walkercore {

void TestTruckDoorConfigFromStateFallsBackWhenAnchorMissing() {
    // With neither cabin_position nor head_position set, the
    // builder must return the default placeholder. The default
    // sets `valid = true` and uses the hard-coded day-cab anchor;
    // we verify the result is identical to DefaultTruckDoorConfig
    // by comparing the driver_door.local_position (the most
    // discriminating field).
    TruckState t;
    TruckState_Clear(t);
    EXPECT_FALSE(t.has_cabin_position);
    EXPECT_FALSE(t.has_head_position);

    const TruckDoorConfig from_state = TruckDoorConfigFromTruckState(t);
    const TruckDoorConfig fallback = DefaultTruckDoorConfig();
    EXPECT_TRUE(from_state.valid);
    EXPECT_TRUE(fallback.valid);
    EXPECT_NEAR(from_state.driver_door.local_position.x,
                fallback.driver_door.local_position.x, 1e-9);
    EXPECT_NEAR(from_state.driver_door.local_position.y,
                fallback.driver_door.local_position.y, 1e-9);
    EXPECT_NEAR(from_state.driver_door.local_position.z,
                fallback.driver_door.local_position.z, 1e-9);
}

void TestTruckDoorConfigFromStateAnchorsDoorToCabin() {
    // With cabin_position set, the driver door must be at
    // (cabin.x - 0.45, cabin.y + 0.55, cabin.z - 0.40). The
    // M5 contract documents this offset; the test pins it.
    TruckState t;
    TruckState_Clear(t);
    t.cabin_position = Vec3{0.5, 1.0, -0.2};
    t.has_cabin_position = true;
    t.head_position = Vec3{-0.3, 1.4, -0.6};
    t.has_head_position = true;

    const TruckDoorConfig c = TruckDoorConfigFromTruckState(t);
    EXPECT_TRUE(c.valid);
    EXPECT_NEAR(c.driver_door.local_position.x, 0.5 - 0.45, 1e-9);
    EXPECT_NEAR(c.driver_door.local_position.y, 1.0 + 0.55, 1e-9);
    EXPECT_NEAR(c.driver_door.local_position.z, -0.2 - 0.40, 1e-9);
    EXPECT_NEAR(c.passenger_door.local_position.x, 0.5 + 0.45, 1e-9);
    EXPECT_NEAR(c.passenger_door.local_position.y, 1.0 + 0.55, 1e-9);
    EXPECT_NEAR(c.passenger_door.local_position.z, -0.2 - 0.40, 1e-9);
}

void TestTruckDoorConfigFromStateDriverSeatFromHeadPosition() {
    // driver_seat_local must equal the TruckState::head_position
    // exactly (the seated player's eye-point is the head anchor
    // reported by the SDK). This pins the contract that the
    // CameraState_AdoptPlayer call in the FSM TransitionOut
    // produces the right camera transform on per-truck cabins.
    TruckState t;
    TruckState_Clear(t);
    t.cabin_position = Vec3{0.0, 0.5, 0.0};
    t.has_cabin_position = true;
    t.head_position = Vec3{-0.4, 1.45, -0.55};
    t.has_head_position = true;

    const TruckDoorConfig c = TruckDoorConfigFromTruckState(t);
    EXPECT_NEAR(c.driver_seat_local.x, -0.4, 1e-9);
    EXPECT_NEAR(c.driver_seat_local.y, 1.45, 1e-9);
    EXPECT_NEAR(c.driver_seat_local.z, -0.55, 1e-9);
}

void TestTruckDoorConfigFromStateOnlyCabinSetFallsBack() {
    // If only one of the two anchors is set, the builder must
    // still fall back to the default. A partial config (e.g. a
    // build that publishes cabin_position but not head_position)
    // would put the camera at the wrong height; the default is
    // safer until both are known.
    TruckState t;
    TruckState_Clear(t);
    t.cabin_position = Vec3{0.5, 1.0, -0.2};
    t.has_cabin_position = true;
    // head_position NOT set.

    const TruckDoorConfig c = TruckDoorConfigFromTruckState(t);
    const TruckDoorConfig fallback = DefaultTruckDoorConfig();
    EXPECT_TRUE(c.valid);
    EXPECT_NEAR(c.driver_door.local_position.x,
                fallback.driver_door.local_position.x, 1e-9);
    EXPECT_NEAR(c.driver_door.local_position.z,
                fallback.driver_door.local_position.z, 1e-9);
}

}  // namespace walkercore
