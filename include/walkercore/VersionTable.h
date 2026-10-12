// WalkerCore — per-game-build offset table.
//
// TMRealWalk's strings reveal that the plugin keeps an allow-list of
// known game builds and degrades gracefully for unknown builds:
//
//   ETS2 1.61.1.0
//   ETS2 1.61.1.1
//   ATS 1.61
//   ATS 1.61.2
//   ATS 1.61.3
//   ATS 1.61.3.1
//
//   [resolve] dxgi_present not found by signature; the known address of
//   this ATS build is used
//
// We mirror that pattern but with a structured table per build. Each
// entry can hold:
//   - offsets for runtime discovery (e.g. dxgi_present address for
//     camera hooking in M2, PxScene vtable pattern for collision in M3)
//   - feature flags (which features have been validated for this build)
//
// M0.5 ships with the schema only; we start filling entries when M2 needs
// them.

#ifndef WALKERCORE_VERSION_TABLE_H
#define WALKERCORE_VERSION_TABLE_H

#include <stdint.h>

#include "walkercore/GameVersion.h"

namespace walkercore {

// Subsystems that depend on per-build offsets or signatures. Adding a new
// subsystem here forces a value in every entry of the table; that's what
// we want — better to know we don't know than to silently fall back to
// guess.
enum class Subsystem : unsigned {
    CameraPresentHook = 0,  // M2: signature of IDXGISwapChain::Present
    CameraManager,          // M2: how the game stores its camera manager
    PhysXScene,             // M3: signature to locate PxScene vtable
    LightSourceClass,       // M7: signature of the game's light_source
};

// What a subsystem needs from the table. Subsystems with no special needs
// are listed in VersionEntry::features so they can be flagged off.
struct SubsystemEntry {
    // Address relative to the .exe base. SCS gives us the game_id but not
    // a module handle, so we read this at runtime via GetModuleHandle.
    uint64_t rva = 0;

    // Whether we have a validated address for this build. False means
    // "we tried and failed; do not enable this subsystem".
    bool verified = false;

    // Short ASCII tag describing the lookup strategy. For the camera
    // hook, for example: "vtable_search", "signature_4bytes", "direct".
    const char *strategy = "unknown";
};

// Per-build configuration. Held by VersionTable by value (cheap).
struct VersionEntry {
    // Match key. We match on game_id + a closed range [version_min,
    // version_max]; the first matching entry wins. Use {0, 0} for a
    // catch-all entry that covers any future build.
    const char *game_id;
    uint32_t version_min;
    uint32_t version_max;

    // Short human-readable tag for the diagnostic dump. Pure display.
    const char *label;

    // Per-subsystem configuration.
    SubsystemEntry subsystems[4];

    // Bitmask of features that have been validated end-to-end on this
    // build. When a feature fails, we log and clear its bit; the user
    // can re-enable via INI once we have M6.
    //
    // We use plain uint32_t to keep the table constexpr-friendly. Bits
    // are documented in Feature below.
    uint32_t features_enabled;
};

// Feature bits. Add new bits at the top, do not renumber.
enum Feature : uint32_t {
    FeatureNone            = 0,
    FeatureFreeCamera      = 1u << 0,  // M2: free camera control
    FeatureCollision       = 1u << 1,  // M3: world collision
    FeatureWalk            = 1u << 2,  // M4: walking
    FeatureEnterExit       = 1u << 3,  // M5: enter/exit truck
    FeatureConfigReload    = 1u << 4,  // M6: INI live reload
    FeatureHeadTracker     = 1u << 5,  // M7: TrackIR/opentrack
    FeatureShadow          = 1u << 6,  // M7: soft shadow
    FeatureFlashlight      = 1u << 7,  // M7: flashlight
    FeatureDoorAnimation   = 1u << 8,  // M7: door animation
    FeatureFuelPump        = 1u << 9,  // M8: full fuel pump flow
    FeatureAdBlue          = 1u << 10, // M8: AdBlue/DEF
    FeatureTrailerCoupling = 1u << 11, // M9: step-by-step coupling
    FeatureKnockOut        = 1u << 12, // M10: vehicle impact knock-out
};

// Stable, exhaustive name lookup for every Feature value. Used by
// walker_dump_version() to surface one row per Feature bit so the
// diagnostic log is greppable by feature name. New Feature entries
// MUST add a case here (-Wswitch-enum catches the omission at compile
// time) and MUST be added to the FeatureNameReturnsNameForEveryValue
// probe test in test_version_table.cpp so the test registers the new
// bit under its expected display name.
//
// Returns the literal "feature <snake_name>" prefix that the dump
// uses as the row label. Falls back to "(unknown)" for any
// FeatureNotInRange-or-out-of-range value — the function is meant to
// be safe to call from any log path.
const char *FeatureName(Feature f);

// The table itself. Static data — no allocation, no mutex needed.
class VersionTable {
public:
    // Lookup. Returns nullptr if no entry matches. The first matching
    // entry wins; we order entries most-specific first.
    static const VersionEntry *Lookup(const GameIdentity &id);

    // Bit test helpers.
    static bool FeatureIsOn(const VersionEntry &entry, Feature f) {
        return (entry.features_enabled & static_cast<uint32_t>(f)) != 0;
    }

    // Diagnostic: print all entries to the log. Used by the F12 dump.
    static void DumpToLog();
};

}  // namespace walkercore

#endif  // WALKERCORE_VERSION_TABLE_H