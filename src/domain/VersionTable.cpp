// WalkerCore — VersionTable implementation.
//
// M0.5 ships with the table schema and the lookup logic, populated with
// placeholder entries for the builds that TMRealWalk's strings confirm
// (ETS2 1.61.1.0..1.61.1.1, ATS 1.61..1.61.3.1). Offsets are zero and
// features are off — that's fine for M0.5. As each subsystem lands (M2,
// M3, ...) we fill in the offset and set verified = true for the build
// that we test against.
//
// When the user runs WalkerCore against a build not in the table, we
// return nullptr and emit a log line. That matches TMRealWalk's behaviour
// where unknown builds cause features to switch off automatically
// (README §NOTES).

#include "walkercore/VersionTable.h"
#include "walkercore/Log.h"

#include <cstring>  // strcmp

namespace walkercore {

namespace {

// Known builds. Order: most specific first; the catch-all is the fallback.
// We cover ETS2 and ATS 1.61.x because that is what TMRealWalk's changelog
// mentions as the supported version range and what we tested our build
// config against.
//
// Version encoding: (major << 16) | minor. We replicate the SCS encoding
// inline so this file stays SDK-free (the public VersionTable header uses
// plain uint32_t, not scs_u32_t). Major is the SDK API major; minor is
// the per-release increment the game ships with.
constexpr uint32_t kVersion(uint32_t major, uint32_t minor) {
    return (major << 16) | (minor & 0xffffu);
}

// game_id, version_min, version_max, label, [SubsystemEntry x 4], features
constexpr VersionEntry kEntries[] = {
    {
        "eut2",
        kVersion(1, 13),  // 1.61.1.0
        kVersion(1, 13),
        "ETS2 1.61.1.0",
        {{}, {}, {}, {}},
        0,  // no features validated yet — M2+ will flip these on per build
    },
    {
        "eut2",
        kVersion(1, 14),
        kVersion(1, 14),
        "ETS2 1.61.1.1",
        {{}, {}, {}, {}},
        0,
    },
    {
        "ats",
        kVersion(1, 13),
        kVersion(1, 13),
        "ATS 1.61",
        {{}, {}, {}, {}},
        0,
    },
    {
        "ats",
        kVersion(1, 14),
        kVersion(1, 14),
        "ATS 1.61.2",
        {{}, {}, {}, {}},
        0,
    },
    {
        "ats",
        kVersion(1, 15),
        kVersion(1, 15),
        "ATS 1.61.3",
        {{}, {}, {}, {}},
        0,
    },
    {
        "ats",
        kVersion(1, 16),
        kVersion(1, 16),
        "ATS 1.61.3.1",
        {{}, {}, {}, {}},
        0,
    },
};

constexpr const VersionEntry *kCatchAllEntry = nullptr;

}  // namespace

const VersionEntry *VersionTable::Lookup(const GameIdentity &id) {
    for (const auto &entry : kEntries) {
        if (strcmp(entry.game_id, id.game_id) != 0) {
            continue;
        }
        if (id.game_version >= entry.version_min &&
            id.game_version <= entry.version_max) {
            return &entry;
        }
    }
    return kCatchAllEntry;
}

void VersionTable::DumpToLog() {
    WALKERCORE_LOG_INFO("version table dump (%u entries):", sizeof(kEntries) / sizeof(kEntries[0]));
    for (const auto &entry : kEntries) {
        WALKERCORE_LOG_INFO("  %s [%u.%u..%u.%u] features=0x%08X",
                            entry.label,
                            (entry.version_min >> 16) & 0xffffu,
                            entry.version_min & 0xffffu,
                            (entry.version_max >> 16) & 0xffffu,
                            entry.version_max & 0xffffu,
                            entry.features_enabled);
    }
}

// FeatureName — exhaustive switch over Feature values. Adding a new
// Feature bit without updating this switch makes the file fail to
// compile under -Werror because of -Wswitch-enum. That is the
// behaviour we want: the dump and the test must stay in sync with
// the enum.
const char *FeatureName(Feature f) {
    switch (f) {
        case FeatureNone:            return "feature none";
        case FeatureFreeCamera:      return "feature free_camera";
        case FeatureCollision:       return "feature collision";
        case FeatureWalk:            return "feature walk";
        case FeatureEnterExit:       return "feature enter_exit";
        case FeatureConfigReload:    return "feature config_reload";
        case FeatureHeadTracker:     return "feature head_tracker";
        case FeatureShadow:          return "feature shadow";
        case FeatureFlashlight:      return "feature flashlight";
        case FeatureDoorAnimation:   return "feature door_animation";
        case FeatureFuelPump:        return "feature fuel_pump";
        case FeatureAdBlue:          return "feature adblue";
        case FeatureTrailerCoupling: return "feature trailer_coupling";
        case FeatureKnockOut:        return "feature knockout";
    }
    return "(unknown)";
}

}  // namespace walkercore