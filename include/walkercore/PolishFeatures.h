// WalkerCore — polish features (M7).
//
// Head tracker, shadow, flashlight, door animation. These all share a
// common shape: a domain type + a small provider that computes state. The
// render layer (M2+ D3D11 hook) consumes the state to draw the effect.
//
// Each feature is split into:
//   - State struct (in this header) — what the domain exposes.
//   - Provider (declared here, implemented in src/provider/):
//       * HeadTrackerProvider — TrackIR / opentrack / mouse smoothing
//       * ShadowProvider       — projection circle on ground
//       * FlashlightProvider   — point-light position/direction
//       * DoorAnimationProvider — door swing angle over time
//
// Stub vs real:
//   Each provider has a "Stub" implementation that returns sane default
//   state without doing real work. The M7 lander fills in the real impls
//   once the D3D11 hook + memory-scan path are in place. Until then the
//   game log warns once per provider that the stub is active.

#ifndef WALKERCORE_POLISH_FEATURES_H
#define WALKERCORE_POLISH_FEATURES_H

#include "walkercore/CameraState.h"
#include "walkercore/PlayerState.h"
#include "walkercore/TruckState.h"

namespace walkercore {

// -----------------------------------------------------------------------------
// Head tracker (M7).
// -----------------------------------------------------------------------------

// Source of yaw/pitch deltas for camera look. Multiple sources can be
// active; the HeadTrackerProvider sums them with weights.
enum class HeadTrackerSource : unsigned {
    Mouse = 0,
    TrackIR,        // FreeTrackClient.dll
    Opentrack,      // opentrack protocol (UDP)
    LastSource,
};

// Human-readable name for a HeadTrackerSource. Used by diagnostic dumps
// and log output. The string is a static literal — callers don't own it.
inline const char *HeadTrackerSourceName(HeadTrackerSource s) {
    switch (s) {
        case HeadTrackerSource::Mouse:     return "Mouse";
        case HeadTrackerSource::TrackIR:   return "TrackIR";
        case HeadTrackerSource::Opentrack: return "Opentrack";
        case HeadTrackerSource::LastSource: return "LastSource";
    }
    return "(unknown)";
}

// Sample from one tracker source. Deltas in radians since last sample.
struct HeadTrackerSample {
    float yaw_delta_rad;
    float pitch_delta_rad;
    float roll_delta_rad;  // not used by WalkerCore but exposed for trackers
};

// Head tracker state. Output of HeadTrackerProvider per frame.
struct HeadTrackerState {
    bool active;          // true if at least one source is producing data
    HeadTrackerSource dominant_source;
    float yaw_radians;    // accumulated yaw offset
    float pitch_radians;  // accumulated pitch offset
};

// -----------------------------------------------------------------------------
// Shadow (M7).
// -----------------------------------------------------------------------------

// Blob projection under the player. Real impl reads the sun direction
// from the game and projects the player capsule. Stub uses the configured
// sun direction.
struct PlayerShadowState {
    bool active;
    // Centre of the blob in world space (on the ground).
    Vec3 worldCentre;
    // Radius (m) — proportional to capsule_radius and shadow scale.
    float radius;
    // Soft edge width (m).
    float softness;
    // Hex colour (RGBA, packed). 0 means "use the game's shadow colour".
    uint32_t rgba;
};

}  // namespace walkercore

#endif  // WALKERCORE_POLISH_FEATURES_H