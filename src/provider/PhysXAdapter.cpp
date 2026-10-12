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
#include <cstdint>  // UINT64_MAX
#include <cstdio>  // std::snprintf
#include <cstring>  // memmem, memcpy

#include "walkercore/Log.h"

#if defined(_WIN32)
#include <windows.h>
#endif

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

#if defined(_WIN32)
namespace {

// Minimal in-memory PE parser used by the runtime scan. Returns the
// (rva, size) pair for the requested section, or {0, 0, false} when
// not found / malformed. Pure byte reader — no Win32 header needed
// beyond the section name + the standard offsets. The implementation
// mirrors FindPeTextSection in tests/test_physx_adapter.cpp: the test
// reads from a FILE* into a vector, this reads from a const uint8_t*
// base address.
struct PeSectionRange {
    uint32_t rva;
    uint32_t size;
    bool ok;
};

PeSectionRange FindPeSection(const uint8_t *buf,
                              size_t size,
                              const char *section_name) {
    PeSectionRange info{0, 0, false};
    if (buf == nullptr || size < 0x40 || section_name == nullptr) {
        return info;
    }
    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, buf + 0x3C, 4);
    if (e_lfanew + 24 > size) {
        return info;
    }
    if (buf[e_lfanew] != 'P' || buf[e_lfanew + 1] != 'E' ||
        buf[e_lfanew + 2] != 0 || buf[e_lfanew + 3] != 0) {
        return info;
    }
    uint16_t num_sections = 0;
    std::memcpy(&num_sections, buf + e_lfanew + 4 + 2, 2);
    uint16_t opt_hdr_size = 0;
    std::memcpy(&opt_hdr_size, buf + e_lfanew + 4 + 16, 2);
    const size_t sections_off = e_lfanew + 4 + 20 + opt_hdr_size;
    const size_t name_len = std::strlen(section_name);
    if (name_len > 8) {
        // Standard PE section names are <= 8 chars (including the
        // trailing NUL if shorter). Anything longer is a malformed
        // request, not a real PE section.
        return info;
    }
    for (uint16_t i = 0; i < num_sections; ++i) {
        const size_t off = sections_off + static_cast<size_t>(i) * 40;
        if (off + 40 > size) break;
        if (std::memcmp(buf + off, section_name, name_len) == 0 &&
            (name_len == 8 || buf[off + name_len] == 0)) {
            std::memcpy(&info.rva, buf + off + 12, 4);
            std::memcpy(&info.size, buf + off + 8, 4);
            info.ok = true;
            return info;
        }
    }
    return info;
}

}  // namespace
#endif  // _WIN32

// Run the real-binary scan + ResolveKnownMethods against the live
// amtrucks.exe process (Windows hosts only). Returns true when at
// least one catalog slot was resolved against the in-memory PxScene
// vtable. On non-Windows hosts or when amtrucks.exe is not in the
// process (unit tests, debug runs without a game), returns false and
// leaves the cache empty — walker_dump_physx reports "(no live
// process)" for the slot column.
static bool PhysXAdapter_RuntimeScan(PhysXAdapter::RuntimeCache *cache) {
#if !defined(_WIN32)
    (void)cache;
    return false;
#else
    if (cache == nullptr) return false;
    const HMODULE mod = ::GetModuleHandleA("amtrucks.exe");
    if (mod == nullptr) {
        return false;
    }
    const auto *base = reinterpret_cast<const uint8_t *>(mod);
    // We don't know the mapped size without parsing optional header
    // fields we don't currently consume. The signature scan reads up
    // to its buffer size, so a 4 MiB ceiling is enough to cover the
    // PxScene vtable on ATS 1.61.3.1s (the vtable lives at RVA
    // 0x25020A0 well within the first ~2.5 MiB of .text mapping).
    // Reading past the actual mapping is unsafe; cap at 8 MiB which
    // comfortably covers the .rdata + early .text range the scan
    // needs and never reads past the .text section end + slack.
    constexpr size_t kMaxScanBytes = 8u * 1024u * 1024u;
    const PeSectionRange text_sec = FindPeSection(base, kMaxScanBytes, ".text");
    const PeSectionRange rdata_sec = FindPeSection(base, kMaxScanBytes, ".rdata");
    if (!text_sec.ok || !rdata_sec.ok) {
        return false;
    }
    const uint8_t *rdata_ptr = base + rdata_sec.rva;
    const size_t rdata_size = static_cast<size_t>(rdata_sec.size);
    if (rdata_size == 0 || rdata_size > kMaxScanBytes) {
        return false;
    }
    // Narrow the .text VA range to [min(catalog VA) - 1 MiB,
    // max(catalog VA) + 1 MiB]. Passing the full .text section lets
    // unrelated giant vtables (e.g. 13185 slots at RVA 0x1DBF8C8 on
    // ATS 1.61.3.1s) dominate the "best run" pick. The catalog is
    // already build-specific, so deriving the range from it makes the
    // scan automatically build-correct: a matching PxScene vtable
    // will have all 7 of its slot pointers inside this window, while
    // unrelated vtables will have at least one pointer outside it and
    // get rejected by the signature scan.
    uint64_t catalog_min = UINT64_MAX;
    uint64_t catalog_max = 0;
    for (int m = 0; m < kPhysXKnownMethodCount; ++m) {
        const uint64_t va = kPhysXKnownMethods[m].va;
        if (va < catalog_min) catalog_min = va;
        if (va > catalog_max) catalog_max = va;
    }
    constexpr uint64_t kCatalogSlackBytes = 0x100000ULL;  // 1 MiB
    const uint64_t text_lo = (catalog_min > kCatalogSlackBytes)
                                 ? (catalog_min - kCatalogSlackBytes)
                                 : 0;
    const uint64_t text_hi = catalog_max + kCatalogSlackBytes;

    cache->scan = PhysXAdapter::MemoryScanFromBufferWithTextRange(
        rdata_ptr, rdata_size, text_lo, text_hi);
    cache->have_scan = cache->scan.success;

    if (!cache->scan.success) {
        return false;
    }
    // The vtable_base returned by the scan is an OFFSET INTO THE
    // BUFFER WE PASSED (i.e., offset into .rdata). ResolveKnownMethods
    // takes the same buffer + the same base offset + accepted_run,
    // so the math is the same as in the on-disk test.
    const bool ok = PhysXAdapter::ResolveKnownMethods(
        rdata_ptr,
        rdata_size,
        cache->scan.vtable.vtable_base,
        cache->scan.vtable.accepted_run,
        &cache->resolve);
    cache->have_resolve = ok;
    return ok;
#endif  // _WIN32
}

void PhysXAdapter::SetConfig(const PhysXAdapterConfig &cfg) {
    cfg_ = cfg;
}

bool PhysXAdapter::Install() {
    if (installed_) {
        return false;
    }
    installed_ = true;

    // Run the real-binary scan + ResolveKnownMethods against the
    // live amtrucks.exe process when present. The result feeds
    // walker_dump_physx() so the F12 dump reports actual slot
    // numbers, not -1 placeholders. When amtrucks.exe is not loaded
    // (unit tests, debug runs without a game), the cache stays
    // empty and the dump says "(no live process)".
    const bool scanned = PhysXAdapter_RuntimeScan(&cache_);
    if (scanned && cache_.have_resolve) {
        WALKERCORE_LOG_INFO(
            "PhysXAdapter::Install: runtime scan resolved %d slots: "
            "release=%d addActor=%d simulate=%d shiftOrigin=%d "
            "sweep=%d raycast=%d overlap=%d [TMRealWalk-style probe]",
            cache_.resolve.release_slot >= 0 ? 1 : 0,
            cache_.resolve.add_actor_slot,
            cache_.resolve.simulate_slot,
            cache_.resolve.shift_origin_slot,
            cache_.resolve.sweep_slot,
            cache_.resolve.raycast_slot,
            cache_.resolve.overlap_slot);
    } else {
        WALKERCORE_LOG_WARN(
            "PhysXAdapter::Install: amtrucks.exe not in-process or "
            "scan failed; walker_dump_physx will report "
            "(no live process) for slot columns. Queries fall back "
            "to no-hit until then.");
    }

    // Log the catalog so the game.log.txt carries a snapshot of which
    // VAs the live ResolveKnownMethods will look for. The trailing
    // [TMRealWalk-style probe] tag matches the M10 collision log so
    // diagnostic parsers can group M3 + M10 events together.
    char catalog_buf[512];
    int written = std::snprintf(
        catalog_buf, sizeof(catalog_buf),
        "PhysXAdapter::Install: catalog has %d methods:",
        kPhysXKnownMethodCount);
    if (written <= 0) {
        return true;
    }
    size_t off = static_cast<size_t>(written);
    for (int m = 0; m < kPhysXKnownMethodCount && off < sizeof(catalog_buf); ++m) {
        const int n = std::snprintf(
            catalog_buf + off, sizeof(catalog_buf) - off,
            "%s %s=0x%llX",
            (m == 0 ? "" : ","),
            kPhysXKnownMethods[m].name,
            static_cast<unsigned long long>(kPhysXKnownMethods[m].va));
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(catalog_buf) - off) {
            break;
        }
        off += static_cast<size_t>(n);
    }
    WALKERCORE_LOG_INFO("%s [TMRealWalk-style probe]", catalog_buf);
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

PhysXScanResult PhysXAdapter::MemoryScanFromBufferWithTextRange(const uint8_t *bytes,
                                                                  size_t size,
                                                                  uint64_t text_lo,
                                                                  uint64_t text_hi) {
    // Same strategy selection as MemoryScanFromBuffer but with the
    // .text VA range passed through to the signature scan. The
    // synthetic branch (text_lo == text_hi == 0) is intentionally NOT
    // supported here; this overload exists for the real-binary
    // integration test where mis-identifying a vtable in an unrelated
    // data structure would silently break the slot resolution.
    PhysXScanResult res;

    res.string_probes_hit = CountPhysXStringsInBuffer(bytes, size);

    PhysXScanResult sig = ScanBufferForVtable(bytes, size, text_lo, text_hi);
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

size_t PhysXAdapter::DumpVtableEntriesAnnotated(const uint8_t *bytes,
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
    // Per-entry max size: "0x" + 16 hex digits + " [name]" where
    // name is up to 12 chars (PxScene::shiftOrigin is the longest
    // at 16 chars but the longest catalog name is "shiftOrigin" at
    // 11 chars) + " | " separator. We size the snprintf buffer
    // generously.
    size_t written = 0;
    for (size_t i = 0; i < to_dump; ++i) {
        uint64_t val = 0;
        std::memcpy(&val, bytes + vtable_off + i * 8, 8);
        const char *matched = nullptr;
        for (int m = 0; m < kPhysXKnownMethodCount; ++m) {
            if (kPhysXKnownMethods[m].va == val) {
                matched = kPhysXKnownMethods[m].name;
                break;
            }
        }
        const int n = std::snprintf(
            out + written,
            (out_size > written) ? out_size - written : 0,
            "%s0x%016llX%s%s%s",
            (i == 0 ? "" : " | "),
            static_cast<unsigned long long>(val),
            matched ? " [" : "",
            matched ? matched  : "",
            matched ? "]"       : "");
        if (n <= 0 || static_cast<size_t>(n) >= out_size - written) {
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
//
// sweep: located by walking back from the LEA ref to the assertion
// string "PxScene::sweep(): first geometry object parameter must be
// specified" (PhysX 3.4 SDK). The LEA sits 0x83B bytes into a
// 0x2A0-byte-stack function whose prologue is the typical
// "mov [rsp+0x10], rdx; push rbp; push rbx; push rsi; push rdi;
// push r13; push r14; push r15; lea rbp, [rsp-0x1A0]; sub rsp, 0x2A0"
// (sweep allocates a stack array for batched hits). The function
// entry's preceding int3 padding (15 bytes of 0xCC) gives a
// clean MSVC function boundary.
const PhysXKnownMethod kPhysXKnownMethods[kPhysXKnownMethodCount] = {
    {"release",       0x141AE07B6ULL, -1},
    {"addActor",      0x141AE0B62ULL, -1},
    {"simulate",      0x141AE4D80ULL, -1},
    {"shiftOrigin",   0x141AE6C28ULL, -1},
    {"sweep",         0x141B06F10ULL, -1},
    // raycast: full implementation with 0x490-byte stack frame and
    // 64-slot hit buffer init at VA 0x141AE1610. Located as slot
    // 15 in the PxScene vtable @ RVA 0x25020A0.
    {"raycast",       0x141AE1610ULL, -1},
    // overlap: thin dispatcher at 0x141AE0F70 that sets r9=0 and
    // tail-jumps to 0x141AE0FD0, where the actual function has a
    // 0x2c0-byte stack frame and 8-slot hit buffer init.
    // Located as slot 13 in the PxScene vtable @ RVA 0x25020A0.
    {"overlap",       0x141AE0F70ULL, -1},
    // removeActor: thin dispatcher at 0x141AE1C60 whose error-path
    // strings uniquely identify "PxScene::removeActor(): Actor"
    // (followed by source-file
    // "D:\\manual\\third\\physx\\version_patched\\PhysX_3.4\\Source\\PhysX\\src\\NpScene.cpp"
    // and message "%s not assigned to scene or assigned to another
    // scene. Call will be ignored!"). The tail target 0x141AE1CF0
    // dispatches over the actor kind and calls the per-type detach
    // helper. Located as slot 14 in the PxScene vtable @ RVA
    // 0x25020A0. Catalog VA is exact (no LEA-walk-back drift).
    {"removeActor",   0x141AE1C60ULL, -1},
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
    out->sweep_slot = -1;
    out->raycast_slot = -1;
    out->overlap_slot = -1;
    out->remove_actor_slot = -1;
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
            // Match within kPhysXResolveSlotTolerance bytes of the
            // catalog VA. The catalog VAs are LEA-walk-back
            // approximations; the slot pointers are the actual runtime
            // entries. The drift is bounded by 8 bytes on ATS
            // 1.61.3.1s (release Δ=-6, addActor Δ=-2, shiftOrigin Δ=-8);
            // a 16-byte tolerance gives headroom for future builds.
            const uint64_t delta =
                (val > target) ? (val - target) : (target - val);
            if (delta <= kPhysXResolveSlotTolerance) {
                switch (m) {
                    case 0: out->release_slot = slot; break;
                    case 1: out->add_actor_slot = slot; break;
                    case 2: out->simulate_slot = slot; break;
                    case 3: out->shift_origin_slot = slot; break;
                    case 4: out->sweep_slot = slot; break;
                    case 5: out->raycast_slot = slot; break;
                    case 6: out->overlap_slot = slot; break;
                    case 7: out->remove_actor_slot = slot; break;
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