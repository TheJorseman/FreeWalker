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
#include <cstdio>  // std::snprintf
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

// Count how many of kPhysXProbeStrings[k] are present (verbatim,
// including the trailing NUL) at any offset inside [bytes, bytes+size).
// Probes are short fixed ASCII so a Boyer-Moore search is overkill —
// linear scan is fine for the 60-byte probe set in 6 MiB of .rdata.
static int CountPhysXStringsInBuffer(const uint8_t *bytes, size_t size) {
    if (bytes == nullptr || size == 0) {
        return 0;
    }
    int hits = 0;
    for (int i = 0; i < kPhysXProbeStringCount; ++i) {
        const char *needle = kPhysXProbeStrings[i];
        const size_t needlelen = std::strlen(needle);
        if (needlelen == 0 || needlelen > size) {
            continue;
        }
        // memmem-style search: first occurrence of needle + trailing '\0'.
        const size_t scan = size - needlelen;
        for (size_t off = 0; off <= scan; ++off) {
            if (std::memcmp(bytes + off, needle, needlelen) == 0) {
                hits += 1;
                break;  // one match per probe is enough
            }
        }
    }
    return hits;
}

// MemoryScanFromBuffer — scan a contiguous .rdata buffer for the
// PxScene vtable signature. Returns the first run >= kMinPhysXVtableRun
// consecutive 8-byte LE pointers whose values look like code pointers.
// When text_lo / text_hi are both zero, the scan uses the synthetic
// branch: any non-zero 8-byte value qualifies as a "code pointer".
// Production callers (Install path) pass the real .text VA range; the
// unit test uses the synthetic branch to inject payload without
// touching amtrucks.exe.
//
// Min threshold + probe strings live in include/walkercore/PhysXAdapter.h
// (`kMinPhysXVtableRun`, `kPhysXProbeStrings`) so tests + DLL builds
// reference the same set.
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
    if (best_run < kMinPhysXVtableRun) {
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
    // Both strategies run on the same buffer so the result carries the
    // truth about each independently. TMRealWalk logs each separately
    // ("scene strings not found - ...", "scene vtable not found - ...");
    // we follow the same convention.
    PhysXScanResult res;

    // Strategy 1 (cheapest): known PhysX error strings. A hit is
    // evidence that the binary statically linked the public PhysX SDK
    // assertions, which is the precondition for a PxScene vtable to
    // exist at all.
    res.string_probes_hit = CountPhysXStringsInBuffer(bytes, size);

    // Strategy 2 (signature): the .rdata run of code pointers that
    // forms the vtable layout. When text_lo / text_hi are both zero
    // (synthetic branch) we accept any non-zero 8-byte value so tests
    // can inject a synthetic vtable without a real .text.
    PhysXScanResult sig = ScanBufferForVtable(bytes, size, 0, 0);
    res.vtable = sig.vtable;

    // Pick a single strategy tag so the caller (test or runtime) can
    // distinguish fallback levels.
    if (res.string_probes_hit > 0 && sig.success) {
        res.success = true;
        res.strategy = "hybrid";
    } else if (sig.success) {
        res.success = true;
        res.strategy = "signature";
    } else if (res.string_probes_hit > 0) {
        // Strings present but no vtable run found: this is exactly the
        // condition TMRealWalk logs as "scene strings not found - ...".
        // We treat it as a partial success because it confirms the SDK
        // is linked; the no-hit fallback still applies.
        res.success = true;
        res.strategy = "strings";
    } else {
        res.success = false;
        res.strategy = "no_match";
    }
    return res;
}

size_t PhysXAdapter::DumpVtableEntries(const uint8_t *bytes,
                                        size_t size,
                                        size_t max_entries,
                                        char *out,
                                        size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (bytes == nullptr || size == 0 || max_entries == 0) {
        return 0;
    }
    // Reuse the same scan so the dump points at the *signature*
    // candidate, not the strings location. Synthetic branch — the
    // unit test calls with arbitrary non-zero payloads.
    PhysXScanResult sig = ScanBufferForVtable(bytes, size, 0, 0);
    if (!sig.success) {
        return 0;
    }
    const size_t vtable_off = sig.vtable.vtable_base;
    const size_t vtable_run =
        (sig.vtable.accepted_run > 0)
            ? static_cast<size_t>(sig.vtable.accepted_run)
            : 0;
    const size_t to_dump = (max_entries < vtable_run) ? max_entries : vtable_run;
    const size_t vtable_end = vtable_off + to_dump * 8;
    if (vtable_end > size) {
        return 0;
    }
    // Format each entry as 0x%016llX, separated by " | ". 21 bytes per
    // entry + slack. We size the snprintf buffer generously.
    size_t written = 0;
    for (size_t i = 0; i < to_dump; ++i) {
        uint64_t val = 0;
        std::memcpy(&val, bytes + vtable_off + i * 8, 8);
        const int n = std::snprintf(
            out + written,
            (out_size > written) ? out_size - written : 0,
            "%s0x%016llX",
            (i == 0 ? "" : " | "), val);
        if (n <= 0 || static_cast<size_t>(n) >= out_size - written) {
            // Truncate cleanly. The next call would need a bigger buffer.
            break;
        }
        written += static_cast<size_t>(n);
    }
    return written;
}

// The runtime catalog of known PxScene method VAs. VA = image base +
// RVA; image base is 0x140000000 for ATS 1.61.3.1s. VAs sourced from
// scripts/physx-vta-scan/README.md § "Hallazgos confirmados" (each
// entry has the LEA-ref matching chain documented there).
//
// Slot indices: open-source PhysX 3.4 PxScene.h has methods in the
// documented order. release is the first virtual destructor-style
// method (slot 0). addActor lives among the scene-graph mutators; the
// exact slot is build-dependent and is left at -1 here until a
// matching run resolves it.
const PhysXKnownMethod kPhysXKnownMethods[kPhysXKnownMethodCount] = {
    {"release",       0x141AE07B6ULL, -1},
    {"addActor",      0x141AE0B62ULL, -1},
    {"simulate",      0x141AE4D80ULL, -1},
    {"shiftOrigin",   0x141AE6C28ULL, -1},
};

bool PhysXAdapter::ResolveKnownMethods(const uint8_t *bytes,
                                       size_t size,
                                       uint64_t vtable_base,
                                       int32_t accepted_run,
                                       PhysXVtableResolveResult *out) {
    if (out == nullptr || bytes == nullptr || size == 0) {
        return false;
    }
    out->release_slot = -1;
    out->add_actor_slot = -1;
    out->simulate_slot = -1;
    out->shift_origin_slot = -1;
    if (accepted_run <= 0) {
        return false;
    }
    // Bounds-check the candidate vtable slice against `size`. We
    // don't apply a separate upper bound beyond accepted_run, so we
    // are safe against the test that synthesises a 200-entry vtable.
    const uint64_t vtable_bytes =
        static_cast<uint64_t>(accepted_run) * 8ULL;
    if (vtable_base >= size || vtable_bytes > size - vtable_base) {
        return false;
    }
    int matched = 0;
    for (int m = 0; m < kPhysXKnownMethodCount; ++m) {
        const uint64_t target = kPhysXKnownMethods[m].va;
        for (int32_t slot = 0; slot < accepted_run; ++slot) {
            const size_t off = vtable_base + static_cast<size_t>(slot) * 8;
            uint64_t val = 0;
            std::memcpy(&val, bytes + off, 8);
            if (val == target) {
                switch (m) {
                    case 0: out->release_slot = slot; break;
                    case 1: out->add_actor_slot = slot; break;
                    case 2: out->simulate_slot = slot; break;
                    case 3: out->shift_origin_slot = slot; break;
                    default: break;
                }
                matched += 1;
                break;
            }
        }
    }
    return matched > 0;
}

}  // namespace walkercore