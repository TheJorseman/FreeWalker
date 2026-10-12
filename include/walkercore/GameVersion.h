// WalkerCore — game version identification.
//
// We need to identify which game build is running so the rest of the
// plugin can pick the right offsets and feature availability. The SCS
// SDK gives us two pieces of information:
//   - `game_id`     — "eut2" or "ats" (lowercase, never null)
//   - `game_version` — u32, encoded as (major << 16) | minor
//
// The game_id is stable; the game_version is the SDK-reported API version
// (does NOT match the game's display version — see SCS forum post cited
// in docs/observations.md).
//
// The header is SDK-free on purpose: the SCS types are a private
// implementation detail of the adapter layer (PluginEntry.cpp). The
// public surface takes POD fields so domain code can construct a
// GameIdentity without ever including scssdk.h.
//
// M0.5 scope:
//   - Identify the game_id and game_version from the SDK params.
//   - Provide a lookup key for VersionTable.
//   - M0.5 does NOT yet scan game memory to read real offsets; that's
//     done lazily when M3 (collision) needs them.

#ifndef WALKERCORE_GAME_VERSION_H
#define WALKERCORE_GAME_VERSION_H

#include <stdint.h>

namespace walkercore {

// Stable identity of the running game. Cheap to copy.
struct GameIdentity {
    // The SCS-provided game id: "eut2" or "ats". Never null when
    // constructed from SDK params; we keep a non-owning pointer because
    // the SDK guarantees the string outlives scs_telemetry_shutdown.
    const char *game_id;

    // The SCS-provided SDK API version, encoded as SCS_MAKE_VERSION.
    uint32_t game_version;

    // Cached: SCS_GET_MAJOR_VERSION(game_version) and minor.
    unsigned short major;
    unsigned short minor;

    // True if game_id == "eut2"; false if "ats".
    bool is_ets2;

    // Construct from the SDK-free fields the adapter layer extracts from
    // scs_sdk_init_params_v100_t. game_id must outlive the identity
    // (the SDK guarantees this for the lifetime of scs_telemetry_init
    // through scs_telemetry_shutdown).
    static GameIdentity FromFields(const char *game_id, uint32_t game_version);
};

// Compare two identities for equality. Used as a lookup key in VersionTable.
bool operator==(const GameIdentity &a, const GameIdentity &b);
bool operator!=(const GameIdentity &a, const GameIdentity &b);

}  // namespace walkercore

#endif  // WALKERCORE_GAME_VERSION_H