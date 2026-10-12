// WalkerCore — PhysXAdapter provider implementation.
//
// M3 SCOPE: STUB. See header for the algorithm contract.
//
// The stub keeps the rest of the build green so M4+ (input, walking)
// can be developed and tested against a no-op collision layer. When the
// real implementation lands, this file's behaviour becomes:
//   - Raycast / SphereSweep: query the local triangle/box cache built
//     from the game's PxScene.
//   - groundY: a downward raycast at the probe's x,z, up to 5 m.
//
// Until then: every query returns "no hit" so the player slides through
// the world. The FrameLoop logs a single warning on first use so the
// missing collision is obvious in the game log.

#include "walkercore/PhysXAdapter.h"

#include <cmath>  // std::sqrt

#include "walkercore/Log.h"

namespace walkercore {

namespace {

// One-shot warning flag. We log exactly once per session to avoid spam.
bool g_warned_once = false;

void WarnCollisionNotImplemented() {
    if (g_warned_once) {
        return;
    }
    g_warned_once = true;
    WALKERCORE_LOG_WARN(
        "PhysXAdapter: stub — collision queries return no-hit. "
        "Player will pass through world geometry. "
        "Real adapter requires public PhysX 3.4 SDK linkage or memory scan; "
        "both land in M3 proper.");
}

// Test-only hook: clear the one-shot warn flag so the unit test can
// observe the warning the first time it triggers the stub path. Production
// code never calls this — the warning is meant to fire exactly once per
// process, never again.
extern "C" void ResetPhysXStubWarnForTesting() {
    g_warned_once = false;
}

}  // namespace

PhysXAdapter::PhysXAdapter() = default;

PhysXAdapter::~PhysXAdapter() {
    if (installed_) {
        Uninstall();
    }
}

void PhysXAdapter::SetConfig(const PhysXAdapterConfig &cfg) {
    cfg_ = cfg;
}

bool PhysXAdapter::Install() {
    if (installed_) {
        return false;
    }
    installed_ = true;
    WALKERCORE_LOG_WARN(
        "PhysXAdapter::Install: stub — no collision geometry loaded. "
        "Install strategy will be (1) PublicPhysXSDK, fall back to (2) MemoryScan.");
    return true;
}

void PhysXAdapter::Uninstall() {
    if (!installed_) {
        return;
    }
    installed_ = false;
}

bool PhysXAdapter::Raycast(Vec3 origin,
                            Vec3 direction,
                            double max_distance,
                            Vec3 *hit_point,
                            Vec3 *hit_normal) {
    WarnCollisionNotImplemented();
    (void)origin;
    (void)direction;
    (void)max_distance;
    if (hit_point != nullptr) *hit_point = {0, 0, 0};
    if (hit_normal != nullptr) *hit_normal = {0, 0, 0};
    return false;
}

MoveResult PhysXAdapter::SphereSweep(Vec3 origin,
                                     double radius,
                                     Vec3 displacement) {
    WarnCollisionNotImplemented();
    (void)radius;
    // No hit: resolved = origin + displacement.
    return MoveResult{
        {origin.x + displacement.x,
         origin.y + displacement.y,
         origin.z + displacement.z},
        {0.0, 0.0, 0.0},
        static_cast<float>(std::sqrt(displacement.x * displacement.x +
                                      displacement.y * displacement.y +
                                      displacement.z * displacement.z)),
        false};
}

bool PhysXAdapter::groundY(Vec3 probe, double *out_y) {
    WarnCollisionNotImplemented();
    (void)probe;
    if (out_y != nullptr) {
        *out_y = probe.y;  // ground at the probe's height
    }
    return false;
}

}  // namespace walkercore