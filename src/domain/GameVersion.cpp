// WalkerCore — GameIdentity implementation.

#include "walkercore/GameVersion.h"

#include <string.h>

namespace walkercore {

GameIdentity GameIdentity::FromFields(const char *game_id,
                                       uint32_t game_version) {
    GameIdentity id;
    id.game_id = game_id != nullptr ? game_id : "";
    id.game_version = game_version;
    // Explicit narrow on purpose: SDK macro returns unsigned int but the
    // major/minor fields are unsigned short to match the SCS wire format.
    id.major = static_cast<unsigned short>((game_version >> 16) & 0xffffu);
    id.minor = static_cast<unsigned short>(game_version & 0xffffu);
    id.is_ets2 = (strcmp(id.game_id, "eut2") == 0);
    return id;
}

bool operator==(const GameIdentity &a, const GameIdentity &b) {
    return a.game_version == b.game_version &&
           strcmp(a.game_id, b.game_id) == 0;
}

bool operator!=(const GameIdentity &a, const GameIdentity &b) {
    return !(a == b);
}

}  // namespace walkercore