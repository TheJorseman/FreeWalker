// WalkerCore — PhysXAdapter provider.
//
// Owns the runtime extraction of the game's collision scene. The
// implementation lives in src/provider/PhysXAdapter.cpp.
//
// M3 SCOPE: STUB. The actual adapter:
//   1. Tries the public PhysX 3.4 SDK approach: locate PxPhysics and
//      PxScene via the game binary's exported symbols or via a known
//      global accessor (game-version-specific).
//   2. If the public path fails, falls back to memory scanning for the
//      PxScene vtable (TMRealWalk's strategy; brittle).
//   3. Builds a local triangle/box representation of the scene for
//      fast sweep queries. Periodically refreshes as the game streams
//      geometry in/out.
//
// Why we don't ship the implementation in M3:
//   - Public PhysX SDK linkage is a non-trivial CMake setup.
//   - The memory-scan path requires offset tables per game build (see the
//     VersionTable pattern).
//   - Both paths can crash the game if mis-implemented.
//
// The interface here is stable; we will swap the implementation when
// M3 lands.

#ifndef WALKERCORE_PHYSX_ADAPTER_H
#define WALKERCORE_PHYSX_ADAPTER_H

#include <stdint.h>

#include "walkercore/WorldState.h"

namespace walkercore {

// Configuration knobs. Read from M6 INI; defaults match TMRealWalk's
// ground-snap parameters.
struct PhysXAdapterConfig {
    // Player capsule radius. TMRealWalk defaults to ~0.35 m.
    double capsule_radius = 0.35;

    // Player capsule total height (top of head to bottom of feet).
    // Used for "where on the body did the collision happen".
    double capsule_height = 1.85;

    // Maximum forward sweep per movement step (clamping).
    double max_sweep_distance = 1.0;

    // Slope angle limit in radians (climbing up beyond this slides the
    // player back down). TMRealWalk uses ~50° (≈ 0.87 rad).
    double max_slope_angle = 0.87;

    // If true, ignore collisions while descending / pending.
    bool disable_collisions = false;
};

class PhysXAdapter : public IWorldGeometry {
public:
    PhysXAdapter();
    ~PhysXAdapter() override;

    PhysXAdapter(const PhysXAdapter &) = delete;
    PhysXAdapter &operator=(const PhysXAdapter &) = delete;

    // Configure from a config block. Safe to call before Install.
    void SetConfig(const PhysXAdapterConfig &cfg);

    // Install the adapter. Returns true on success.
    // Logs which strategy was used (public SDK vs memory scan) and the
    // time taken to build the local representation.
    bool Install();

    // Idempotent.
    void Uninstall();

    bool isInstalled() const { return installed_; }

    // IWorldGeometry overrides. M3 stub: returns "no hit" so the player
    // does not get stuck waiting for collision.
    bool Raycast(Vec3 origin,
                Vec3 direction,
                double max_distance,
                Vec3 *hit_point,
                Vec3 *hit_normal) override;

    MoveResult SphereSweep(Vec3 origin,
                           double radius,
                           Vec3 displacement) override;

    bool groundY(Vec3 probe, double *out_y) override;

private:
    PhysXAdapterConfig cfg_;
    bool installed_ = false;
};

}  // namespace walkercore

#endif  // WALKERCORE_PHYSX_ADAPTER_H