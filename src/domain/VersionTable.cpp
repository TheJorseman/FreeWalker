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
    {
        // Steam build of ATS 1.61.3.1 reports SCS SDK API version
        // (major=1, minor=7) instead of the (1, 13..1, 16) range the
        // CD/standalone editions use. The game's display name still
        // says "1.61.3.1s" — see game.log.txt evidence in the M1
        // rollout notes. Without this entry, the build hit
        // "game build not in version table" and most subsystems
        // refused to activate (the user observed F-1/F-10 do nothing
        // in-game).
        //
        // M3 SCAN: amtrucks.exe SHA 5D3B4E343154DE6B25DD366B6048C5BC5C452A4DAA50BC38B33FD2E571D54257.
        // Signature scan identifies the PxScene vtable candidate at
        // RVA 0x2500918 (image-base + 0x2500918 = 0x142501B18); the
        // smaller PxScene vtable that the catalog (release, addActor,
        // simulate, shiftOrigin, sweep) maps to is at RVA 0x2500EA0
        // (118 slots, 0x1425020A0). Strategy = "signature + strings
        // + hybrid" — see PhysXAdapter.cpp for the multi-strategy
        // detection pipeline. Slot offsets for raycast / overlap
        // remain unverified; sweep is cataloged at slot 4 of a 5-slot
        // vtable at RVA 0x2507C08. See
        // docs/observations/ats_1_61_3_1s.physx.functions.md and
        // scripts/physx-vta-scan/README.md in the rea repo for the
        // offline RE that produced this entry. The slot table is
        // filled after a live PxScene instance confirms each slot.
        "ats",
        kVersion(1, 7),
        kVersion(1, 7),
        "ATS 1.61.3.1s (Steam)",
        // CameraPresentHook (M2) + CameraManager (M2) + PhysXScene
        // (M3, RVA 0x2500918, signature+strings+hybrid verified) +
        // LightSourceClass (M7) — see PhysXAdapter.cpp header for the
        // RE evidence and scripts/physx-vta-scan/README.md in the rea
        // repo for the offline scan that produced the M3 RVA. Slot
        // dispatch for PxScene::raycast/overlap remains pending a
        // live PxScene instance; until then the M3 (collision) bit
        // stays OFF.
        { {}, {}, {0x2500918u, true, "signature+strings+hybrid"}, {} },
        // Features the offline RE pipeline has confirmed are reachable
        // on this build. Camera hooking (FeatureFreeCamera) and the
        // M4 walking state are already shipping; FeatureConfigReload
        // (M6) handles INI live reload. FeatureCollision stays OFF
        // until the slot table is verified against a real PxScene.
        FeatureFreeCamera | FeatureWalk | FeatureConfigReload,
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