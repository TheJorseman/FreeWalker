// WalkerCore — PerTruckDoorTable tests (M5 lander partial extension).
//
// These tests pin three behaviours:
//   1. BuiltIn() returns a table with at least one well-known ATS row.
//   2. Find() returns nullptr for unknown model_id and a non-null
//      pointer for known model_ids.
//   3. ApplyOverride() patches the driver door pivot + cabin box when
//      the row has non-zero values, and is a no-op when row is null.
//
// The test-only constructor (`PerTruckDoorTable(rows, count)`) lets us
// inject a custom table without depending on the disk-resident
// per-truck data file. Adding a row to kBuiltInRows_ does not require
// any test update — the integration path is exercised by
// test_truck_door_config_from_state.cpp::TestTruckDoorConfigAppliesOverride
// which calls `TruckDoorConfigFromTruckState(t)` and walks
// the result.

#include "TestHarness.h"

#include <cstring>
#include <vector>

#include "walkercore/PerTruckDoorTable.h"
#include "walkercore/TruckDoorConfig.h"
#include "walkercore/TruckState.h"

namespace walkercore {

namespace {

// Build a TruckState with cabin + head anchors and a stable model_id.
// Tests use this to feed TruckDoorConfigFromTruckState the minimal
// configuration event payload.
TruckState MakeTruckState(const char *model_id) {
    TruckState t;
    t.has_cabin_position = true;
    t.cabin_position = {3.0, 1.0, -2.0};
    t.has_head_position = true;
    t.head_position = {-0.40, 1.20, -0.20};
    if (model_id != nullptr && model_id[0] != '\0') {
        const size_t n = std::strlen(model_id);
        for (size_t i = 0; i < n && i < TruckState::kModelIdCapacity - 1; ++i) {
            t.model_id[i] = model_id[i];
        }
        t.model_id[n] = '\0';
        t.has_model_id = true;
    }
    return t;
}

}  // namespace

void TestPerTruckDoorTableBuiltInFindsKnownRow() {
    const PerTruckDoorTable &t = PerTruckDoorTable::BuiltIn();
    const TruckDoorConfigOverride *ov = t.Find("vehicle.volvo.vnl_2016");
    EXPECT_TRUE(ov != nullptr);
    if (ov == nullptr) return;
    // VNL 2016 has a high-roof sleeper: cabin_size_y_m is > 1.85.
    EXPECT_TRUE(ov->cabin_size_y_m > 1.85f);
}

void TestPerTruckDoorTableBuiltInUnknownReturnsNull() {
    const PerTruckDoorTable &t = PerTruckDoorTable::BuiltIn();
    EXPECT_TRUE(t.Find("vehicle.no.such.truck") == nullptr);
    EXPECT_TRUE(t.Find("") == nullptr);
    EXPECT_TRUE(t.Find(nullptr) == nullptr);
}

void TestPerTruckDoorTableBuiltInHasMultipleAtsRows() {
    // At least two rows for two distinct brands. The exact list
    // grows with each M5 commit; this guards against accidental
    // table-replacement regressions. Updated whenever a new truck
    // row is appended to kBuiltInRows_ in PerTruckDoorTable.cpp.
    const PerTruckDoorTable &t = PerTruckDoorTable::BuiltIn();
    size_t hits = 0;
    const char *const kKnown[] = {
        // Original six (pre-M5-expansion):
        "vehicle.volvo.vnl_2016",
        "vehicle.kenworth.w900",
        "vehicle.peterbilt.579",
        "vehicle.freightliner.cascadia",
        "vehicle.mack.anthem",
        "vehicle.volvo.vnl",
        // Expansion: more common ATS trucks.
        "vehicle.peterbilt.389",
        "vehicle.kenworth.t680",
        "vehicle.kenworth.t880",
        "vehicle.freightliner.cascadia_2018",
        "vehicle.western_star.49x",
        "vehicle.western_star.4900",
        "vehicle.international.lt",
        "vehicle.international.lone_star",
    };
    for (size_t i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); ++i) {
        if (t.Find(kKnown[i]) != nullptr) {
            hits += 1;
        }
    }
    EXPECT_EQ(hits, static_cast<size_t>(14));
}

void TestPerTruckDoorTableNewRowsAreFindable() {
    // Per-truck-row sanity check for the most-recently-added models.
    // Each row must have non-zero door offsets (otherwise the generic
    // placeholder applies, defeating the purpose of the per-truck
    // table) and a plausible cabin-size box.
    const PerTruckDoorTable &t = PerTruckDoorTable::BuiltIn();
    struct Sample { const char *id; float expected_cabin_y; };
    const Sample kSamples[] = {
        // Cascadia 2018 is 1.90 m (post-refresh).
        {"vehicle.freightliner.cascadia_2018", 1.90f},
        // Western Star 49X has the tallest sleeper at 1.85 m.
        {"vehicle.western_star.49x", 1.85f},
        // International LoneStar matches the Anthem at 1.95 m.
        {"vehicle.international.lone_star", 1.95f},
    };
    for (size_t i = 0; i < sizeof(kSamples) / sizeof(kSamples[0]); ++i) {
        const TruckDoorConfigOverride *ov = t.Find(kSamples[i].id);
        EXPECT_TRUE(ov != nullptr);
        if (ov == nullptr) continue;
        // Door offsets must be non-zero so the override applies.
        EXPECT_TRUE(ov->door_outward_x_m != 0.0f);
        EXPECT_TRUE(ov->door_outward_y_m != 0.0f);
        // Cabin height must match the expected value (regression guard
        // against typos in the table).
        EXPECT_NEAR(ov->cabin_size_y_m, kSamples[i].expected_cabin_y, 1e-5f);
    }
}

void TestApplyOverrideNullIsNoOp() {
    TruckDoorConfig cfg = DefaultTruckDoorConfig();
    const TruckDoorConfig before = cfg;
    PerTruckDoorTable::ApplyOverride(cfg, nullptr, before);
    EXPECT_NEAR(cfg.driver_door.local_position.x, before.driver_door.local_position.x, 1e-5);
    EXPECT_NEAR(cfg.cabin_min.x, before.cabin_min.x, 1e-5);
    EXPECT_NEAR(cfg.cabin_max.y, before.cabin_max.y, 1e-5);
}

void TestApplyOverrideDoorPatchIsSentinelAware() {
    TruckDoorConfig cfg = DefaultTruckDoorConfig();
    // Capture the generic door pivot before override (the
    // Cabin-derived path produces cabin - (0.45, 0.55, 0.40) =
    // (3 - 0.45, 1 + 0.55, -2 - 0.40) = (2.55, 1.55, -2.40)).
    TruckState t = MakeTruckState(nullptr);
    cfg = TruckDoorConfigFromTruckState(t);  // overrides NOT applied (no model_id)
    // Pre-override door position:
    const Vec3 door_before = cfg.driver_door.local_position;

    TruckDoorConfigOverride ov;
    ov.door_outward_x_m = -0.50f;  // door sits 0.05 m further left
    ov.door_outward_y_m = 0.60f;   // door sits 0.05 m higher
    PerTruckDoorTable::ApplyOverride(cfg, &ov, cfg);

    EXPECT_NEAR(cfg.driver_door.local_position.x, door_before.x - 0.05, 1e-5);
    EXPECT_NEAR(cfg.driver_door.local_position.y, door_before.y + 0.05, 1e-5);
    // z left at sentinel (0.0) — must NOT change.
    EXPECT_NEAR(cfg.driver_door.local_position.z, door_before.z, 1e-5);
}

void TestApplyOverrideCabinBoxPatch() {
    TruckDoorConfig cfg = DefaultTruckDoorConfig();
    TruckState t = MakeTruckState(nullptr);
    cfg = TruckDoorConfigFromTruckState(t);
    const float cabin_centre_y = (cfg.cabin_max.y + cfg.cabin_min.y) * 0.5f;

    TruckDoorConfigOverride ov;
    ov.cabin_size_y_m = 1.95f;  // taller sleeper cab
    PerTruckDoorTable::ApplyOverride(cfg, &ov, cfg);

    const float new_centre_y = (cfg.cabin_max.y + cfg.cabin_min.y) * 0.5f;
    EXPECT_NEAR(new_centre_y, cabin_centre_y, 1e-5);
    EXPECT_NEAR(cfg.cabin_max.y - cfg.cabin_min.y, 1.95f, 1e-5);
}

void TestTruckDoorConfigAppliesOverride() {
    // Integration: Build a TruckState with a known model_id and verify
    // TruckDoorConfigFromTruckState applies the per-truck row.
    TruckState t = MakeTruckState("vehicle.kenworth.w900");
    const TruckDoorConfig cfg = TruckDoorConfigFromTruckState(t);
    EXPECT_TRUE(cfg.valid);
    // W900 cabin height is 1.80 m. Find centroid height and box height.
    const float cy = (cfg.cabin_max.y + cfg.cabin_min.y) * 0.5f;
    const float ch = cfg.cabin_max.y - cfg.cabin_min.y;
    EXPECT_NEAR(ch, 1.80f, 1e-5);
    EXPECT_TRUE(cy > 1.0f);  // cabin centre near cabin_position.y
}

}  // namespace walkercore
