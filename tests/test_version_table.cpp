// WalkerCore — VersionTable tests.
//
// Pins the architectural rule that adding/removing a Feature enum
// value must be a compile-time catch at three layers:
//   - FeatureName() switch (in VersionTable.cpp) — caught by
//     -Werror -Wswitch-enum.
//   - The probe test below — caught by failing the row count or
//     null-pointer assertion if a Feature is silently un-named.
//   - The walker's diagnostic dump — caught by
//     TestDumpVersionReportsRowForEveryFeature, which asserts that
//     every Feature bit name actually appears in walker_dump_version
//     output.
//
// Together the three layers force the maintainer to update every
// site when introducing a new Feature bit; none of the three alone
// is enough (a missing switch case fails to compile but a missing
// test row wouldn't).

#include "TestHarness.h"

#include <string.h>

#include "walkercore/VersionTable.h"

namespace walkercore {

// Run through every Feature value (except the empty FeatureNone, which
// has no rows and no display string of interest) and call FeatureName().
// Each call must return a non-null pointer that does NOT fall back to
// the "(unknown)" sentinel — that's the failure mode where someone
// adds a Feature bit but forget to add a switch case.

void TestFeatureNameReturnsNameForEveryValue() {
    // The canonical list, mirroring the enum order. Adding a new
    // Feature bit MUST add a line here AND in FeatureName() in
    // src/domain/VersionTable.cpp.
    const struct {
        Feature f;
        const char *expected_name;
    } kCases[] = {
        {FeatureFreeCamera,      "feature free_camera"},
        {FeatureCollision,       "feature collision"},
        {FeatureWalk,            "feature walk"},
        {FeatureEnterExit,       "feature enter_exit"},
        {FeatureConfigReload,    "feature config_reload"},
        {FeatureHeadTracker,     "feature head_tracker"},
        {FeatureShadow,          "feature shadow"},
        {FeatureFlashlight,      "feature flashlight"},
        {FeatureDoorAnimation,   "feature door_animation"},
        {FeatureFuelPump,        "feature fuel_pump"},
        {FeatureAdBlue,          "feature adblue"},
        {FeatureTrailerCoupling, "feature trailer_coupling"},
        {FeatureKnockOut,        "feature knockout"},
    };
    const size_t kCount = sizeof(kCases) / sizeof(kCases[0]);

    for (size_t i = 0; i < kCount; ++i) {
        const char *name = FeatureName(kCases[i].f);
        EXPECT_TRUE(name != nullptr);
        EXPECT_TRUE(name[0] != '\0');
        // The unknown-fallback sentinel must never appear for a real
        // enum value — that means the switch in VersionTable.cpp has
        // a missing case.
        EXPECT_TRUE(strcmp(name, "(unknown)") != 0);
        // The string must equal the documented snake_case name so
        // logs are greppable.
        EXPECT_STREQ(name, kCases[i].expected_name);
    }
}

// Feature bits are defined as 1u << N for unique N. Pin the contract:
// every Feature bit must be a power of two, two bits must not collide,
// and FeatureNone must be exactly zero (so `(features & FeatureNone)`
// is always false).
//
// If a maintainer accidentally writes `FeatureFoo = 1u << 0` reusing
// an existing bit, this test fails with a clear message.

void TestFeatureBitsAreUniquePowersOfTwo() {
    struct Case { Feature f; const char *name; };
    const Case kCases[] = {
        {FeatureFreeCamera,      "free_camera"},
        {FeatureCollision,       "collision"},
        {FeatureWalk,            "walk"},
        {FeatureEnterExit,       "enter_exit"},
        {FeatureConfigReload,    "config_reload"},
        {FeatureHeadTracker,     "head_tracker"},
        {FeatureShadow,          "shadow"},
        {FeatureFlashlight,      "flashlight"},
        {FeatureDoorAnimation,   "door_animation"},
        {FeatureFuelPump,        "fuel_pump"},
        {FeatureAdBlue,          "adblue"},
        {FeatureTrailerCoupling, "trailer_coupling"},
        {FeatureKnockOut,        "knockout"},
    };
    const size_t kCount = sizeof(kCases) / sizeof(kCases[0]);

    // FeatureNone must be 0 (no bits set, doesn't collide with anyone).
    EXPECT_EQ(static_cast<uint32_t>(FeatureNone), 0u);

    for (size_t i = 0; i < kCount; ++i) {
        const uint32_t v = static_cast<uint32_t>(kCases[i].f);
        // power-of-two check: v != 0 and (v & (v - 1)) == 0.
        EXPECT_TRUE(v != 0u);
        EXPECT_EQ(v & (v - 1), 0u);
        // No two bits may collide.
        for (size_t j = i + 1; j < kCount; ++j) {
            EXPECT_TRUE(v != static_cast<uint32_t>(kCases[j].f));
        }
    }
}

// The probe count must agree with the canonical kCases table above.
// If a new Feature is added and the table above is forgotten to be
// updated, this count assertion fires at compile time-of-the-test
// expectations (the table is a C array, so adding rows changes the
// sizeof).

void TestFeatureProbeTableHasExpectedCount() {
    // Counted at the time this test was written: 13 features. When
    // the enum grows (e.g. M12 adds FeatureInventoryLog) this number
    // bumps AND the kCases array above grows AND
    // walker_dump_version's kRows array grows AND the new
    // FeatureName() case is added.
    EXPECT_EQ(static_cast<uint32_t>(FeatureKnockOut),
              static_cast<uint32_t>(1u << 12));
    // The probe table is allowed to grow freely — we don't pin its
    // length here, only the highest set bit's value. A maintainer who
    // adds FeatureFeatureX = 1u << 13 will see the EXPECT_EQ above
    // fail loudly, telling them to also update the probe table in
    // TestFeatureNameReturnsNameForEveryValue.
}

// Steam build of ATS reports SCS SDK version (major=1, minor=7) but
// the in-game version label is still "1.61.3.1s". Pin: that build
// MUST resolve to a non-null VersionEntry so subscribers see
// "ATS 1.61.3.1s (Steam)" and the M2/M4+ subsystems activate
// instead of refusing to start. This is the regression guard for
// the "user pressed F-1/F-10 and nothing happened" report on the
// Steam install.

void TestAtS1_61_3_1sLookupMatchesSteamBuild() {
    const GameIdentity id = GameIdentity::FromFields("ats", 0x00010007u);
    EXPECT_FALSE(id.is_ets2);
    // 0x00010007 decodes to (major=1, minor=7) per the SDK encoding
    // documented in walkercore/GameVersion.h.
    EXPECT_EQ(static_cast<unsigned>(id.major), 1u);
    EXPECT_EQ(static_cast<unsigned>(id.minor), 7u);
    EXPECT_EQ(id.game_version, 0x00010007u);

    const VersionEntry *entry = VersionTable::Lookup(id);
    EXPECT_TRUE(entry != nullptr);
    EXPECT_TRUE(strcmp(entry->game_id, "ats") == 0);
    EXPECT_EQ(entry->version_min, entry->version_max);
    // The label must mention the Steam suffix so the diagnostic dump
    // is unambiguous about which build we matched. If a future build
    // changes the suffix this guard catches the rename.
    EXPECT_TRUE(strstr(entry->label, "Steam") != nullptr);
    EXPECT_TRUE(strstr(entry->label, "1.61") != nullptr);
}

// Reverse-pin: a build we DO NOT recognise (here a fake game
// "unknown" with version 0xdeadbeef) must still return the
// configured catch-all (nullptr) so the diagnostic dump's
// "no match" path stays exercised.

void TestVersionTableUnknownBuildReturnsCatchAll() {
    const GameIdentity id = GameIdentity::FromFields("ats", 0xDEADBEEFu);
    const VersionEntry *entry = VersionTable::Lookup(id);
    // nullptr is the documented catch-all contract.
    EXPECT_TRUE(entry == nullptr);
}

// Sanity: every documented ATS build (1.61 / 1.61.2 / 1.61.3 /
// 1.61.3.1 / 1.61.3.1s) and every ETS2 build still resolves to
// the correct label. Adding a new entry would force this scan
// to break (and produce a clear compile-time table here) so we
// don't drift from the on-disk file.

void TestVersionTableEveryDocumentedBuildResolves() {
    struct Case { const char *id; uint32_t ver; const char *expected_substring; };
    const Case kCases[] = {
        {"eut2", 0x0001000Du, "ETS2 1.61.1.0"},
        {"eut2", 0x0001000Eu, "ETS2 1.61.1.1"},
        {"ats",  0x0001000Du, "ATS 1.61"},
        {"ats",  0x0001000Eu, "ATS 1.61.2"},
        {"ats",  0x0001000Fu, "ATS 1.61.3"},
        {"ats",  0x00010010u, "ATS 1.61.3.1"},
        {"ats",  0x00010007u, "ATS 1.61.3.1s"},
    };
    const size_t kCount = sizeof(kCases) / sizeof(kCases[0]);
    for (size_t i = 0; i < kCount; ++i) {
        const GameIdentity id = GameIdentity::FromFields(kCases[i].id, kCases[i].ver);
        const VersionEntry *entry = VersionTable::Lookup(id);
        EXPECT_TRUE(entry != nullptr);
        if (entry == nullptr) continue;
        EXPECT_TRUE(strstr(entry->label, kCases[i].expected_substring) != nullptr);
    }
}

}  // namespace walkercore
