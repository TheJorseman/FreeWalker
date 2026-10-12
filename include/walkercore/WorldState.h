// WalkerCore — WorldState domain type + IWorldGeometry abstraction.
//
// M3 design:
//
// The world is a black box for the domain layer. Domain code asks
// "could the player move from A to B without going through geometry?"
// and gets back the resolved position + the normal of what was hit.
// This lets us swap implementations without touching anything that
// consumes WorldState (FrameLoop, Animation, etc.).
//
// Implementations of IWorldGeometry:
//   - PhysXAdapter (provider) — reads PxScene from the game at runtime.
//     Two strategies, in order of preference:
//       1. Public PhysX 3.4 SDK: hook into the game's PxPhysics instance
//          and walk the scene graph. Stable across game versions.
//       2. Memory scan: search the game's .text for the PxScene vtable
//          pattern. Used by TMRealWalk; fragile across versions.
//   - StaticWorldGeometry (test stub) — a sphere at origin. Used by unit
//     tests that need deterministic collision responses.
//
// WorldState caches metadata that downstream code may want:
//   - Whether a valid geometry is loaded.
//   - Last query stats (time, hit count) for the F12 dump.
//   - Game-build-specific flags (e.g. "the PxScene is read-only because
//     the game is paused").
//
// Threading: same as TruckState — single-threaded for M3, mutex comes
// when M3+ adds background workers.

#ifndef WALKERCORE_WORLD_STATE_H
#define WALKERCORE_WORLD_STATE_H

#include <stdint.h>

#include "walkercore/TruckState.h"  // for Vec3

namespace walkercore {

// Result of a movement query. Always populated; `hit` indicates whether
// the geometry was touched.
struct MoveResult {
    Vec3 resolved_position;   // where the player ends up after collision
    Vec3 contact_normal;      // normal at the contact point; (0,0,0) if no hit
    float contact_distance;    // distance along the sweep direction; 0 if no hit
    bool hit;                 // true if geometry blocked the sweep
};

// Abstract interface. Implementations live in src/provider/.
class IWorldGeometry {
public:
    virtual ~IWorldGeometry() = default;

    // Raycast from `origin` along `direction` (must be normalised) up to
    // `max_distance`. Returns true if anything was hit. Writes hit_point
    // and hit_normal when returning true.
    virtual bool Raycast(Vec3 origin,
                         Vec3 direction,
                         double max_distance,
                         Vec3 *hit_point,
                         Vec3 *hit_normal) = 0;

    // Sphere-sweep: move a sphere of `radius` from `origin` along
    // `displacement` (magnitude = intended travel distance). Resolves
    // the displacement against the geometry; returns the resulting
    // position + the surface normal of the contact. If the sweep is
    // fully blocked, the contact_distance is the magnitude of `displacement`
    // and the resolved_position is `origin`.
    //
    // Implementations should approximate sliding on slopes: the resolved
    // position should not have arbitrary upward motion unless the slope
    // was actually hit at that height.
    virtual MoveResult SphereSweep(Vec3 origin,
                                    double radius,
                                    Vec3 displacement) = 0;

    // Quick ground test at a horizontal position. Returns the ground Y
    // (height) at the x,z of `probe`. If the probe is far above ground
    // (e.g. falling), returns false.
    virtual bool groundY(Vec3 probe, double *out_y) = 0;
};

struct WorldState {
    // Backing geometry. nullptr until the provider installs one. Domain
    // code MUST check `geometry != nullptr` before calling.
    IWorldGeometry *geometry = nullptr;

    // Whether the backing implementation completed initialisation. Until this
    // is true, sphere-sweep results are undefined.
    bool ready = false;

    // Diagnostic stats, updated per query. Cheap to read.
    uint64_t query_count = 0;
    uint64_t total_query_us = 0;
    uint64_t last_query_us = 0;
};

// Reset stats and clear geometry pointer. Cheap; safe to call any time.
inline void WorldState_Reset(WorldState &w) {
    w.geometry = nullptr;
    w.ready = false;
    w.query_count = 0;
    w.total_query_us = 0;
    w.last_query_us = 0;
}

// Helper for callers: do a sphere sweep with timing into WorldState.
inline MoveResult WorldState_SweepTimed(WorldState &w,
                                       Vec3 pos,
                                       double radius,
                                       Vec3 displacement) {
    if (w.geometry == nullptr) {
        // No geometry — caller is in walk mode without a world loaded.
        // Return the input unchanged so the player does not phase through
        // everything; gameplay code should treat "no geometry" as a
        // configuration error and disable the walker.
        return MoveResult{pos, {0, 0, 0}, 0.0f, false};
    }
    // Timing wrapper. Keep it branchless on the geometry path so the
    // hot loop is not affected by diagnostics.
    const uint64_t t0 = 0;  // M3 real impl: QueryPerformanceCounter
    MoveResult r = w.geometry->SphereSweep(pos, radius, displacement);
    (void)t0;
    w.query_count++;
    // last_query_us intentionally not filled until M3 real impl is wired.
    return r;
}

}  // namespace walkercore

#endif  // WALKERCORE_WORLD_STATE_H