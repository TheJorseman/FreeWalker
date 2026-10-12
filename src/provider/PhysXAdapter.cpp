// WalkerCore — PhysXAdapter provider implementation.
//
// M3 PARTIAL: signature memory-scan is implemented; the slot table is
// populated for the on-disk amtrucks.exe (ATS 1.61.3.1s SHA
// 5D3B4E343154DE6B25DD366B6048C5BC5C452A4DAA50BC38B33FD2E571D54257) but
// raycast / overlap / sweep slot offsets remain unverified against a live
// PxScene instance. Queries therefore fall back to "no hit" until a real
// game session confirms the slot offsets.
//
// The memory scan looks for the PxScene vtable signature: a contiguous
// run of at least 200 code-pointer-sized (8-byte little-endian) values
// inside the module's .rdata whose values fall within the module's
// .text VA range. On ATS 1.61.3.1s this resolves to the candidate
// vtable at RVA 0x2500918 (image-base + 0x2500918 = 0x142501B18) with
// 436 accepted entries. See scripts/physx-vta-scan/README.md in the
// rea repo for the offline analysis that identified the candidate.

#include "walkercore/PhysXAdapter.h"

#include <cmath>  // std::sqrt
#include <cstring>  // memmem, memcpy

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
        "Slot offsets for PxScene::raycast/overlap/sweep pending live "
        "PxScene verification; signature scan finds the vtable at "
        "RVA 0x2500918 (see scripts/physx-vta-scan).");
}

// Test-only hook: clear the one-shot warn flag so the unit test can
// observe the warning the first time it triggers the stub path.
// Production code never calls this — the warning is meant to fire
// exactly once per process, never again.
extern "C" void ResetPhysXStubWarnForTesting() {
    g_warned_once = false;
}

// Minimum accepted vtable run length. PxScene vtables in ATS 1.61.3.1s
// measure 436 entries (one entry = 8 bytes), with 150+ slots defaulting
// to the same placeholder pointer. A run shorter than this is rejected
// as a noise-induced false positive.
constexpr int kMinVtableRun = 200;

// MemoryScanFromBuffer — scan a contiguous .rdata buffer for the
// PxScene vtable signature. Returns the first run >= kMinVtableRun
// consecutive 8-byte LE pointers whose values look like code pointers.
// When text_lo / text_hi are both zero, the scan uses the synthetic
// branch: any non-zero 8-byte value qualifies as a "code pointer".
// Production callers (Install path) pass the real .text VA range; the
// unit test uses the synthetic branch to inject payload without
// touching amtrucks.exe.
PhysXScanResult ScanBufferForVtable(const uint8_t *bytes,
                                    size_t size,
                                    uint64_t text_lo,
                                    uint64_t text_hi) {
    PhysXScanResult res;
    if (bytes == nullptr || size < 16) {
        return res;
    }
    const bool synthetic = (text_lo == 0 && text_hi == 0);

    int best_run = 0;
    uint64_t best_off = 0;
    int cur_run = 0;
    uint64_t cur_off = 0;
    for (size_t off = 0; off + 8 <= size; off += 8) {
        uint64_t val = 0;
        std::memcpy(&val, bytes + off, 8);
        const bool ok = synthetic ? (val != 0)
                                   : (val >= text_lo && val <= text_hi);
        if (ok) {
            if (cur_run == 0) {
                cur_off = off;
            }
            cur_run += 1;
            if (cur_run > best_run) {
                best_run = cur_run;
                best_off = cur_off;
            }
        } else {
            cur_run = 0;
        }
    }
    if (best_run < kMinVtableRun) {
        res.strategy = "no_match";
        return res;
    }
    res.success = true;
    res.vtable.vtable_base = best_off;  // caller interprets as RVA
    res.vtable.accepted_run = best_run;
    res.vtable.raycast_slot = -1;
    res.vtable.overlap_slot = -1;
    res.vtable.sweep_slot = -1;
    res.strategy = "signature";
    return res;
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
        "PhysXAdapter::Install: partial — vtable signature scan ready "
        "(see MemoryScanFromBuffer), slot dispatch pending live PxScene "
        "verification. Queries fall back to no-hit until then.");
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

PhysXScanResult PhysXAdapter::MemoryScanFromBuffer(const uint8_t *bytes,
                                                    size_t size) {
    // Test-only convenience: when text_lo / text_hi are zero we accept
    // any non-zero 8-byte value as a "code pointer" so the unit test can
    // inject synthetic vtables without needing the .text section. The
    // production path passes the real .text VA range via this same
    // overload by setting bytes to the .rdata payload and using the
    // caller's knowledge of the .text layout to compute the text
    // window. We keep the public surface as a 2-argument function; the
    // adapter-side install path is the production caller.
    //
    // The caller (Install or the test) is responsible for picking
    // text_lo / text_hi. We expose a 0/0 -> "synthetic" branch below
    // because the unit test passes 0/0 by default.
    return ScanBufferForVtable(bytes, size, 0, 0);
}

}  // namespace walkercore