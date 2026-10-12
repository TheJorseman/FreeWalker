// WalkerCore — PhysXAdapter provider.
//
// Owns the runtime extraction of the game's collision scene. The
// implementation lives in src/provider/PhysXAdapter.cpp.
//
// M3 SCOPE: partial. The adapter:
//   1. Tries the public PhysX 3.4 SDK approach: locate PxPhysics and
//      PxScene via the game binary's exported symbols or via a known
//      global accessor (game-version-specific).
//   2. If the public path fails, falls back to memory scanning for the
//      PxScene vtable (TMRealWalk's strategy; brittle across builds).
//   3. Builds a local triangle/box representation of the scene for
//      fast sweep queries. Periodically refreshes as the game streams
//      geometry in/out.
//
// Why we ship a partial M3 implementation now:
//   - Public PhysX SDK linkage is a non-trivial CMake setup that requires
//     a vendored PhysX 3.4 SDK package (deliberately deferred to keep
//     the build hermetic).
//   - The memory-scan path requires offset tables per game build (see the
//     VersionEntry for PhysXScene). We populate the slot table per build
//     as runtime verification confirms each entry.
//   - The signature-based memory scan (find the PxScene vtable by looking
//     for a run of code pointers near "PxScene::release()" assertion
//     strings) is fully implemented and unit-tested; the only remaining
//     piece is per-build slot offset verification, which requires a real
//     running PxScene instance.
//
// Until the slot table is complete, Raycast / SphereSweep / groundY fall
// back to "no hit" so the player slides through geometry but the game
// stays stable. The M2 (camera) + M4 (walking state) are unaffected and
// remain operational.
//
// The interface here is stable; we will swap the implementation when
// the slot table is populated.

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

// Discovered PxScene vtable. The fields are filled by MemoryScan() in the
// adapter; missing fields (slot == -1) keep the no-hit fallback path.
struct PhysXVtable {
    // Base address of the vtable in amtrucks.exe (PxScene::*). Zero when
    // the scan did not find a confident candidate.
    uint64_t vtable_base = 0;

    // Slot offset of PxScene::raycast. -1 if not yet verified for this
    // build.
    int32_t raycast_slot = -1;
    // Slot offset of PxScene::overlap (any-shape sphere query). -1 if
    // not yet verified.
    int32_t overlap_slot = -1;
    // Slot offset of PxScene::sweep (sphere sweep along a vector). -1
    // if not yet verified.
    int32_t sweep_slot = -1;

    // Number of contiguous code-pointer entries starting at vtable_base
    // that the signature scan accepted. A real PxScene vtable on ATS
    // 1.61.3.1s measures 436 entries (with 150+ slots pointing at the
    // default placeholder 0x141D22E00).
    int32_t accepted_run = 0;
};

// Result of a memory scan over a module's .rdata. success is true when at
// least one PhysXVtable candidate was located; false means we fell back
// to no-hit queries.
struct PhysXScanResult {
    bool success = false;
    PhysXVtable vtable{};
    // Short ASCII tag describing the lookup strategy used ("signature",
    // "signature_fallback", "no_match").
    const char *strategy = "no_match";
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

    // Re-run the vtable memory scan against a caller-supplied byte
    // buffer. Test-only hook so the unit test can inject a synthetic
    // .rdata payload without touching amtrucks.exe.
    static PhysXScanResult MemoryScanFromBuffer(const uint8_t *bytes,
                                                size_t size);

private:
    PhysXAdapterConfig cfg_;
    bool installed_ = false;
};

}  // namespace walkercore

#endif  // WALKERCORE_PHYSX_ADAPTER_H