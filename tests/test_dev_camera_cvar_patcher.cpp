// WalkerCore — DevCameraCvarPatcher unit tests.
//
// Coverage:
//   - SetEnabled / isEnabled round-trip.
//   - SetNameRva / name_rva round-trip (the patcher is build-
//     specific; the test seam lets us exercise the resolution
//     without going through the OS module loader).
//   - PatchOnce() with no module base set returns a skip with
//     `cvar_name_va = 0` (the no-module case mirrors what
//     scs_telemetry_init sees before amtrucks.exe is loaded).
//   - PatchOnce() with disabled gate increments skips_disabled
//     and returns `skipped=true`.
//   - PatchOnce() with a freshly-discovered `g_developer\0\0\0\0\0`
//     name VA inside a scratch block of memory, with the
//     heuristic scan locating a `g_developer\0` pointer that
//     points at a value buffer reading `0\0`, ends up with
//     `patched=true`, value reads `1\0` after, and patches_ == 1.
//   - PatchOnce() repeated: second call short-circuits with
//     `skipped=true, already_patched=true` (no second write).
//   - PatchOnce() with value already reading `1\0`: skips and
//     remembers already_patched, no write attempted.
//   - PatchOnce() with signature mismatch (wrong bytes at the
//     expected name VA): skips with the signature-mismatch
//     counter incremented and a non-zero cvar_name_va.
//
// The patcher's data path uses VirtualQuery / VirtualProtect to
// verify page protection. We allocate a scratch block on the
// heap with default (read-write) protection so VirtualQuery
// returns the same protection we expect; the read path succeeds
// and the write path succeeds. The test does NOT exercise the
// VirtualProtect-failed branch because that requires the
// operating system to grant us a read-only page that we then
// try to write; the unit test infrastructure can't synthesise
// that without OS-specific helpers.

#include "TestHarness.h"

#include "walkercore/DevCameraCvarPatcher.h"

#include <cstring>
#include <memory>
#include <vector>

// Forward declarations so test_main.cpp can use
// `walkercore::TestDevCameraCvarPatcherXxx` without including
// the full body. Matches the test_main.cpp convention of calling
// RUN_TEST on each test by qualified name.
namespace walkercore {
void TestDevCameraCvarPatcherDefaultStateIsEnabled();
void TestDevCameraCvarPatcherSetEnabledRoundTrip();
void TestDevCameraCvarPatcherSetNameRvaRoundTrip();
void TestDevCameraCvarPatcherPatchOnceSkipsWhenNoModule();
void TestDevCameraCvarPatcherPatchOnceSkipsWhenDisabled();
void TestDevCameraCvarPatcherPatchOncePatchesFromZeroToOne();
void TestDevCameraCvarPatcherPatchOnceIsIdempotent();
void TestDevCameraCvarPatcherPatchOnceSkipsWhenValueAlreadyOne();
void TestDevCameraCvarPatcherPatchOnceDetectsSignatureMismatch();
void TestDevCameraCvarPatcherCountersAfterSuccessfulPatch();
void TestDevCameraCvarPatcherDualCvarPathPatchesBoth();
void TestDevCameraCvarPatcherDualCvarPathIsIdempotent();
}  // namespace walkercore

#ifdef _WIN32
#include <windows.h>
#endif

namespace walkercore {

namespace {

// Hand-rolled scratch "game memory" — a malloc'd buffer we own
// and free explicitly. The patcher reads from this buffer via
// VirtualQuery + memcpy, and writes to it via VirtualProtect +
// raw pointer write. We arrange a tiny memory image that
// resembles the SCS cvar struct layout: a pointer to "g_developer"
// followed by a value buffer holding "0\0".
struct ScratchCvarImage {
    std::vector<char> bytes;
    uint64_t name_offset;   // offset within `bytes` where "g_developer\0" lives
    uint64_t value_offset;  // offset within `bytes` where the value struct lives
    uint64_t name_va;
    uint64_t value_va;

    // Reserve at least 64 bytes. The patcher uses VirtualQuery on
    // the page containing the target VA, so the whole buffer
    // must fit on one allocation (malloc returns contiguous bytes
    // that VirtualQuery sees as one region).
    static constexpr size_t kBytes = 4096;

    ScratchCvarImage() {
        bytes.assign(kBytes, 0);
        // Lay the name at offset 16 (8-byte aligned).
        name_offset = 16;
        const char *name = "g_developer";
        std::memcpy(&bytes[name_offset], name, std::strlen(name) + 1);
        // Pad with the expected null padding so the signature
        // match tolerates the exact "g_developer\0\0\0\0\0" pattern
        // the RE dump shows at file offset 0x223F6C0.
        for (size_t i = 0; i < 5; ++i) {
            bytes[name_offset + std::strlen(name) + 1 + i] = '\0';
        }
        // Lay the cvar control struct at offset 32 — the patcher's
        // scan starts at name_va - 8 KiB and looks for 8-byte
        // pointers == name_va. The struct we want is:
        //   offset 0: pointer to name (== name_va)
        //   offset 8: value field (a char[8] holding "0\0" or "1\0")
        value_offset = 32;
        // The pointer (uint64_t) is stored in little-endian host
        // order — same as what the patcher's SafeRead does.
        const uint64_t name_ptr = name_va = reinterpret_cast<uint64_t>(&bytes[name_offset]);
        std::memcpy(&bytes[value_offset], &name_ptr, sizeof(name_ptr));
        // Write the value '0' at value_offset + 8.
        bytes[value_offset + 8] = '0';
        bytes[value_offset + 9] = '\0';
        // Cache VAs the patcher will see (VirtualQuery's
        // allocation base + the offset).
        value_va = reinterpret_cast<uint64_t>(&bytes[value_offset + 8]);
    }
};

std::unique_ptr<DevCameraCvarPatcher> MakePatcher() {
    auto p = std::make_unique<DevCameraCvarPatcher>();
    p->SetEnabled(true);
    // Default name_rva_ is 0x223F6C0; tests override per-call.
    // Tests run inside a synthetic 4 KiB scratch buffer and have no
    // real amtrucks.exe to find the secondary `g_debug_camera` cvar
    // in — point its RVA at a deliberately unmapped address so
    // PatchOnce's second iteration cleanly fails the SafeRead and
    // reports a single `skips_value_unreadable`. This pins the
    // test surface for the original PatchOnce path without making
    // the new dual-cvar refactor look like a regression.
    p->SetDebugCameraNameRva(0x7FFE7FFFFFFFULL);
    return p;
}

// Tests use the patcher's SetModuleBaseForTesting hook to point
// the patcher at our ScratchCvarImage. The patcher's scan is
// +/- 8 KiB around the cvar name VA, so the scratch buffer must
// hold both the name and the struct. Our buffer is 4096 bytes
// which is well within the 8 KiB window.

}  // namespace

void TestDevCameraCvarPatcherDefaultStateIsEnabled() {
    auto p = MakePatcher();
    EXPECT_TRUE(p->isEnabled());
    EXPECT_STREQ("0x22408C0", "0x22408C0");  // sentinel
    // Default RVA is 0x22408C0 in ATS 1.61.3.1s (.rdata-resident
    // section, file offset 0x223F6C0). The original commit (2cf1867)
    // used 0x223F6C0 — the file offset, not the RVA. The PE has
    // SectionAlignment=0x1000 but FileAlignment=0x200, so .rdata has
    // a +0x200 offset between file and RVA. The new RVA was derived
    // by walking the .rdata section header.
    EXPECT_EQ(p->name_rva(), 0x22408C0ULL);
    EXPECT_EQ(p->runs(), 0);
    EXPECT_EQ(p->patches(), 0);
}

void TestDevCameraCvarPatcherSetEnabledRoundTrip() {
    auto p = MakePatcher();
    p->SetEnabled(false);
    EXPECT_FALSE(p->isEnabled());
    p->SetEnabled(true);
    EXPECT_TRUE(p->isEnabled());
}

void TestDevCameraCvarPatcherSetNameRvaRoundTrip() {
    auto p = MakePatcher();
    p->SetNameRva(0xDEADBEEFULL);
    EXPECT_EQ(p->name_rva(), 0xDEADBEEFULL);
}

void TestDevCameraCvarPatcherPatchOnceSkipsWhenNoModule() {
    // No module base set + no real amtrucks.exe loaded in the
    // test environment. PatchOnce should skip with the no-module
    // counter incremented.
    auto p = MakePatcher();
    const DevCameraCvarPatchResult r = p->PatchOnce();
    EXPECT_TRUE(r.skipped);
    EXPECT_FALSE(r.patched);
    EXPECT_EQ(r.cvar_name_va, 0ULL);
    EXPECT_EQ(p->skips_no_module(), 1);
    EXPECT_EQ(p->runs(), 1);
}

void TestDevCameraCvarPatcherPatchOnceSkipsWhenDisabled() {
    auto p = MakePatcher();
    p->SetEnabled(false);
    const DevCameraCvarPatchResult r = p->PatchOnce();
    EXPECT_TRUE(r.skipped);
    EXPECT_FALSE(r.patched);
    EXPECT_EQ(p->skips_disabled(), 1);
}

void TestDevCameraCvarPatcherPatchOncePatchesFromZeroToOne() {
    ScratchCvarImage img;
    auto p = MakePatcher();
    // The patcher uses the module base as the search base, not
    // the absolute VA. We pass the buffer's address directly and
    // override the name RVA to 0 (so name_va = base + 0 = buffer
    // start). The struct (offset 32) must be inside the +/- 8
    // KiB scan window around name_va (== buffer start), which
    // 32 bytes certainly is.
    p->SetNameRva(0);
    p->SetModuleBaseForTesting(img.name_va);
    const DevCameraCvarPatchResult r = p->PatchOnce();
    EXPECT_TRUE(r.patched);
    EXPECT_FALSE(r.skipped);
    EXPECT_EQ(r.cvar_name_va, img.name_va);
    EXPECT_EQ(r.cvar_value_va, img.value_va);
    EXPECT_STREQ(r.cvar_value_before, "0");
    EXPECT_STREQ(r.cvar_value_after, "1");
    EXPECT_EQ(p->patches(), 1);
    // The scratch buffer's value byte must now be '1'.
    EXPECT_EQ(img.bytes[img.value_offset + 8], '1');
    EXPECT_EQ(img.bytes[img.value_offset + 9], '\0');
}

void TestDevCameraCvarPatcherPatchOnceIsIdempotent() {
    ScratchCvarImage img;
    auto p = MakePatcher();
    p->SetNameRva(0);
    p->SetModuleBaseForTesting(img.name_va);
    const DevCameraCvarPatchResult r1 = p->PatchOnce();
    EXPECT_TRUE(r1.patched);
    // Reset the scratch value back to '0' so we can prove the
    // second call does NOT rewrite it.
    img.bytes[img.value_offset + 8] = '0';
    img.bytes[img.value_offset + 9] = '\0';
    const DevCameraCvarPatchResult r2 = p->PatchOnce();
    // Second call: the patcher short-circuits because
    // already_patched_ was set on the first call.
    EXPECT_TRUE(r2.skipped);
    EXPECT_FALSE(r2.patched);
    EXPECT_EQ(p->skips_already_patched(), 1);
    // The scratch buffer must still be '0' — the second call did
    // not write.
    EXPECT_EQ(img.bytes[img.value_offset + 8], '0');
}

void TestDevCameraCvarPatcherPatchOnceSkipsWhenValueAlreadyOne() {
    ScratchCvarImage img;
    img.bytes[img.value_offset + 8] = '1';
    img.bytes[img.value_offset + 9] = '\0';
    auto p = MakePatcher();
    p->SetNameRva(0);
    p->SetModuleBaseForTesting(img.name_va);
    const DevCameraCvarPatchResult r = p->PatchOnce();
    EXPECT_TRUE(r.skipped);
    EXPECT_FALSE(r.patched);
    EXPECT_EQ(p->skips_already_patched(), 1);
    EXPECT_EQ(p->patches(), 0);
    // Buffer must still read '1'.
    EXPECT_EQ(img.bytes[img.value_offset + 8], '1');
}

void TestDevCameraCvarPatcherPatchOnceDetectsSignatureMismatch() {
    ScratchCvarImage img;
    // Corrupt the name so the signature check fails.
    img.bytes[img.name_offset + 2] = 'X';
    auto p = MakePatcher();
    p->SetNameRva(0);
    p->SetModuleBaseForTesting(img.name_va);
    const DevCameraCvarPatchResult r = p->PatchOnce();
    EXPECT_TRUE(r.skipped);
    EXPECT_FALSE(r.patched);
    EXPECT_EQ(p->skips_signature_mismatch(), 1);
    EXPECT_EQ(r.cvar_name_va, img.name_va);
    EXPECT_EQ(r.cvar_value_va, 0ULL);
    EXPECT_EQ(img.bytes[img.value_offset + 8], '0');
}

void TestDevCameraCvarPatcherCountersAfterSuccessfulPatch() {
    ScratchCvarImage img;
    auto p = MakePatcher();
    p->SetNameRva(0);
    p->SetModuleBaseForTesting(img.name_va);
    (void)p->PatchOnce();
    EXPECT_EQ(p->runs(), 1);
    EXPECT_EQ(p->patches(), 1);
    EXPECT_EQ(p->skips_already_patched(), 0);
    EXPECT_EQ(p->skips_disabled(), 0);
    EXPECT_EQ(p->skips_no_module(), 0);
    EXPECT_EQ(p->skips_signature_mismatch(), 0);
    // g_developer patched, g_debug_camera SAFELY-FAILED (MakePatcher
    // pins its RVA at an unmapped address so the secondary
    // PatchOneCvar reports skips_value_unreadable once).
    EXPECT_EQ(p->skips_value_unreadable(), 1);
    EXPECT_EQ(p->skips_protect_failed(), 0);
    EXPECT_EQ(p->last_result().cvar_name_va, img.name_va);
    EXPECT_EQ(p->last_result().cvar_value_va, img.value_va);
    // Secondary cvar's outcome is in its own result struct —
    // the patched field is false (we deliberately re-pointed it
    // off-image) and the cvar_name_va VA matches the unmapped RVA
    // so the analyst can tell "the secondary cvar failed, here's
    // the address I tried to read".
    EXPECT_FALSE(p->last_debug_camera_result().patched);
    EXPECT_TRUE(p->last_debug_camera_result().skipped);
    EXPECT_EQ(p->last_debug_camera_result().cvar_name_va,
              img.name_va + 0x7FFE7FFFFFFFULL);
}

// ===========================================================================
// Synthetic layouts for the extended probe.
//
// The pre-M7.2 patcher (HEAD pre-b99ba24's dev branch) only checked
// offset 8 from the matched pointer for the value field. ATS
// 1.61.3.1s's actual SCS CVar struct puts the value field at
// offset 24 (with the hash-chain pointer at +8 and the type tag
// at +16). The probe now walks offsets {8, 16, 24, 32, 40}.
// These tests pin each.
//
// We also exercise the wider scan window (32 KiB vs the historical
// 8 KiB) by placing the struct further from the name pointer than
// the old window allowed.
// ===========================================================================

// SCS CVar struct layout variant A: value at offset 24 (the real ATS
// 1.61.3.1s layout per RE). The struct is laid as:
//   offset 0:  pointer to name (matched)
//   offset 8:  pointer to next in hash chain (8 bytes)
//   offset 16: type tag (uint32 + 4 padding)
//   offset 24: value buffer "0\0"
//   (offset 32+ would be min/max bounds, docstring ptr.)
struct ScratchCvarImageValueAt24 {
    std::vector<char> bytes;
    uint64_t name_offset;
    uint64_t struct_offset;
    uint64_t name_va;
    uint64_t value_va;

    static constexpr size_t kBytes = 4096;

    ScratchCvarImageValueAt24() {
        bytes.assign(kBytes, 0);
        name_offset = 16;
        const char *name = "g_developer";
        std::memcpy(&bytes[name_offset], name, std::strlen(name) + 1);
        for (size_t i = 0; i < 5; ++i) {
            bytes[name_offset + std::strlen(name) + 1 + i] = '\0';
        }
        struct_offset = 48;
        name_va = reinterpret_cast<uint64_t>(&bytes[name_offset]);
        std::memcpy(&bytes[struct_offset], &name_va, sizeof(name_va));
        // Skip the hash-chain ptr at +8 and the type tag at +16
        // (we leave them zero, which the patcher treats as "not a
        // value field" — the heuristic correctly skips those).
        bytes[struct_offset + 24] = '0';
        bytes[struct_offset + 25] = '\0';
        value_va = reinterpret_cast<uint64_t>(&bytes[struct_offset + 24]);
    }
};

void TestDevCameraCvarPatcherProbesValueAtStructOffset24() {
    ScratchCvarImageValueAt24 img;
    auto p = MakePatcher();
    p->SetNameRva(0);
    p->SetModuleBaseForTesting(img.name_va);
    const DevCameraCvarPatchResult r = p->PatchOnce();
    EXPECT_TRUE(r.patched);
    EXPECT_FALSE(r.skipped);
    EXPECT_EQ(r.cvar_name_va, img.name_va);
    EXPECT_EQ(r.cvar_value_va, img.value_va);
    EXPECT_STREQ(r.cvar_value_before, "0");
    EXPECT_STREQ(r.cvar_value_after, "1");
    EXPECT_EQ(p->patches(), 1);
    EXPECT_EQ(img.bytes[img.struct_offset + 24], '1');
    EXPECT_EQ(img.bytes[img.struct_offset + 25], '\0');
}

// Probe the wider scan window. The struct sits at +16 KiB from the
// name pointer — outside the historical 8 KiB window but inside the
// new 32 KiB window. The patcher must still find it.
struct ScratchCvarImageValueFar {
    std::vector<char> bytes;
    uint64_t name_offset;
    uint64_t struct_offset;
    uint64_t name_va;
    uint64_t value_va;

    // 40 KiB. The struct lives 16 KiB after the name pointer —
    // well past the historical 8 KiB window, well inside the new
    // 32 KiB.
    static constexpr size_t kBytes = 40 * 1024;

    ScratchCvarImageValueFar() {
        bytes.assign(kBytes, 0);
        name_offset = 16;
        const char *name = "g_developer";
        std::memcpy(&bytes[name_offset], name, std::strlen(name) + 1);
        // Pad name with zeros up to 16 bytes so the signature
        // matches.
        for (size_t i = 0; i < 5; ++i) {
            bytes[name_offset + std::strlen(name) + 1 + i] = '\0';
        }
        // Struct 16 KiB later: offset 0 = pointer to name,
        // offset 8 = "0\0".
        struct_offset = 16 * 1024;
        name_va = reinterpret_cast<uint64_t>(&bytes[name_offset]);
        std::memcpy(&bytes[struct_offset], &name_va, sizeof(name_va));
        bytes[struct_offset + 8] = '0';
        bytes[struct_offset + 9] = '\0';
        value_va = reinterpret_cast<uint64_t>(&bytes[struct_offset + 8]);
    }
};

void TestDevCameraCvarPatcherWiderScanWindowFindsFarStruct() {
    ScratchCvarImageValueFar img;
    auto p = MakePatcher();
    p->SetNameRva(0);
    p->SetModuleBaseForTesting(img.name_va);
    const DevCameraCvarPatchResult r = p->PatchOnce();
    EXPECT_TRUE(r.patched);
    EXPECT_FALSE(r.skipped);
    EXPECT_EQ(r.cvar_value_va, img.value_va);
    EXPECT_STREQ(r.cvar_value_before, "0");
    EXPECT_STREQ(r.cvar_value_after, "1");
    EXPECT_EQ(p->patches(), 1);
}

// Same as the +24 case but value lives at +32 (the heuristic's
// fifth probe offset). Pins the loop's later offsets work too.
struct ScratchCvarImageValueAt32 {
    std::vector<char> bytes;
    uint64_t name_offset;
    uint64_t struct_offset;
    uint64_t name_va;
    uint64_t value_va;

    static constexpr size_t kBytes = 4096;

    ScratchCvarImageValueAt32() {
        bytes.assign(kBytes, 0);
        name_offset = 16;
        const char *name = "g_developer";
        std::memcpy(&bytes[name_offset], name, std::strlen(name) + 1);
        for (size_t i = 0; i < 5; ++i) {
            bytes[name_offset + std::strlen(name) + 1 + i] = '\0';
        }
        struct_offset = 48;
        name_va = reinterpret_cast<uint64_t>(&bytes[name_offset]);
        std::memcpy(&bytes[struct_offset], &name_va, sizeof(name_va));
        // Value lives at offset 32 instead of 8 (e.g., the SCS
        // struct has four pointer slots before the value).
        bytes[struct_offset + 32] = '0';
        bytes[struct_offset + 33] = '\0';
        value_va = reinterpret_cast<uint64_t>(&bytes[struct_offset + 32]);
    }
};

void TestDevCameraCvarPatcherProbesValueAtStructOffset32() {
    ScratchCvarImageValueAt32 img;
    auto p = MakePatcher();
    p->SetNameRva(0);
    p->SetModuleBaseForTesting(img.name_va);
    const DevCameraCvarPatchResult r = p->PatchOnce();
    EXPECT_TRUE(r.patched);
    EXPECT_EQ(r.cvar_value_va, img.value_va);
    EXPECT_EQ(img.bytes[img.struct_offset + 32], '1');
}

// ===========================================================================
// Dual-cvar path (G5.4, Oct 2026 cycle).
//
// The patcher is responsible for setting both `g_developer` AND
// `g_debug_camera` to "1" because the SCS dev free-camera path
// reads both cvars (TMRealWalk only needed g_developer because
// they used the dev console directly). Lay a single scratch buffer
// holding two cvar structs — one for each name — and confirm
// PatchOnce patches both.
// ===========================================================================
struct ScratchDualCvarImage {
    std::vector<char> bytes;
    uint64_t g_developer_name_offset;
    uint64_t g_developer_struct_offset;
    uint64_t g_developer_name_va;
    uint64_t g_developer_value_va;
    uint64_t g_debug_camera_name_offset;
    uint64_t g_debug_camera_struct_offset;
    uint64_t g_debug_camera_name_va;
    uint64_t g_debug_camera_value_va;

    static constexpr size_t kBytes = 8192;

    ScratchDualCvarImage() {
        bytes.assign(kBytes, 0);

        // g_developer @ offset 0
        g_developer_name_offset = 0;
        const char *dev = "g_developer";
        std::memcpy(&bytes[g_developer_name_offset], dev,
                    std::strlen(dev) + 1);
        for (size_t i = 0; i < 5; ++i) {
            bytes[g_developer_name_offset + std::strlen(dev) + 1 + i] = '\0';
        }
        g_developer_struct_offset = 32;
        g_developer_name_va = reinterpret_cast<uint64_t>(
            &bytes[g_developer_name_offset]);
        std::memcpy(&bytes[g_developer_struct_offset],
                    &g_developer_name_va, sizeof(g_developer_name_va));
        // Layout variant: value at offset 8 from struct (the simple
        // form the smallest SCS CVar struct uses).
        bytes[g_developer_struct_offset + 8] = '0';
        bytes[g_developer_struct_offset + 9] = '\0';
        g_developer_value_va = reinterpret_cast<uint64_t>(
            &bytes[g_developer_struct_offset + 8]);

        // g_debug_camera @ a higher offset so the two scans don't
        // alias. Lay the second cvar struct at offset 256 from the
        // second name.
        g_debug_camera_name_offset = 128;
        const char *dbg = "g_debug_camera";
        std::memcpy(&bytes[g_debug_camera_name_offset], dbg,
                    std::strlen(dbg) + 1);
        for (size_t i = 0; i < 5; ++i) {
            bytes[g_debug_camera_name_offset + std::strlen(dbg) + 1 + i] =
                '\0';
        }
        g_debug_camera_struct_offset = 256;
        g_debug_camera_name_va = reinterpret_cast<uint64_t>(
            &bytes[g_debug_camera_name_offset]);
        std::memcpy(&bytes[g_debug_camera_struct_offset],
                    &g_debug_camera_name_va,
                    sizeof(g_debug_camera_name_va));
        bytes[g_debug_camera_struct_offset + 8] = '0';
        bytes[g_debug_camera_struct_offset + 9] = '\0';
        g_debug_camera_value_va = reinterpret_cast<uint64_t>(
            &bytes[g_debug_camera_struct_offset + 8]);
    }
};

void TestDevCameraCvarPatcherDualCvarPathPatchesBoth() {
    ScratchDualCvarImage img;
    auto p = std::make_unique<DevCameraCvarPatcher>();
    p->SetEnabled(true);
    // Both cvars live at the buffer's start, so name_rva_=0 maps
    // `g_developer` onto img.g_developer_name_va. For
    // g_debug_camera we set debug_camera_name_rva_ to the offset
    // from the buffer's start to img.g_debug_camera_name_va.
    p->SetNameRva(0);
    p->SetDebugCameraNameRva(
        img.g_debug_camera_name_va - img.g_developer_name_va);
    p->SetModuleBaseForTesting(img.g_developer_name_va);

    const DevCameraCvarPatchResult r = p->PatchOnce();

    // g_developer patched.
    EXPECT_TRUE(r.patched);
    EXPECT_EQ(r.cvar_name_va, img.g_developer_name_va);
    EXPECT_EQ(r.cvar_value_va, img.g_developer_value_va);
    EXPECT_STREQ(r.cvar_value_before, "0");
    EXPECT_STREQ(r.cvar_value_after, "1");
    EXPECT_EQ(img.bytes[img.g_developer_struct_offset + 8], '1');
    EXPECT_EQ(img.bytes[img.g_developer_struct_offset + 9], '\0');

    // g_debug_camera ALSO patched.
    const DevCameraCvarPatchResult &rd = p->last_debug_camera_result();
    EXPECT_TRUE(rd.patched);
    EXPECT_EQ(rd.cvar_name_va, img.g_debug_camera_name_va);
    EXPECT_EQ(rd.cvar_value_va, img.g_debug_camera_value_va);
    EXPECT_STREQ(rd.cvar_value_before, "0");
    EXPECT_STREQ(rd.cvar_value_after, "1");
    EXPECT_EQ(img.bytes[img.g_debug_camera_struct_offset + 8], '1');
    EXPECT_EQ(img.bytes[img.g_debug_camera_struct_offset + 9], '\0');

    // Counters: 1 patch per cvar, 0 skips on the primary run.
    EXPECT_EQ(p->runs(), 1);
    EXPECT_EQ(p->patches(), 2);
    EXPECT_EQ(p->skips_already_patched(), 0);
    EXPECT_EQ(p->skips_signature_mismatch(), 0);
    EXPECT_EQ(p->skips_value_unreadable(), 0);
    EXPECT_EQ(p->skips_protect_failed(), 0);
}

// Idempotency: a second PatchOnce() on the dual-cvar layout must
// short-circuit and NOT re-write either cvar (each result reports
// skipped + patched=false, no further memory changes).
void TestDevCameraCvarPatcherDualCvarPathIsIdempotent() {
    ScratchDualCvarImage img;
    auto p = std::make_unique<DevCameraCvarPatcher>();
    p->SetEnabled(true);
    p->SetNameRva(0);
    p->SetDebugCameraNameRva(
        img.g_debug_camera_name_va - img.g_developer_name_va);
    p->SetModuleBaseForTesting(img.g_developer_name_va);

    (void)p->PatchOnce();
    const int runs_after_first = p->runs();
    const int patches_after_first = p->patches();

    // Reset the scratch value bytes back to '0' so we can prove the
    // second call does NOT re-write.
    img.bytes[img.g_developer_struct_offset + 8] = '0';
    img.bytes[img.g_developer_struct_offset + 9] = '\0';
    img.bytes[img.g_debug_camera_struct_offset + 8] = '0';
    img.bytes[img.g_debug_camera_struct_offset + 9] = '\0';

    const DevCameraCvarPatchResult r2 = p->PatchOnce();
    EXPECT_TRUE(r2.skipped);
    EXPECT_FALSE(r2.patched);
    EXPECT_EQ(p->runs(), runs_after_first + 1);
    // Patches counter is NOT incremented on the idempotent path.
    EXPECT_EQ(p->patches(), patches_after_first);

    // Buffer values stayed '0' — the patcher did not write.
    EXPECT_EQ(img.bytes[img.g_developer_struct_offset + 8], '0');
    EXPECT_EQ(img.bytes[img.g_debug_camera_struct_offset + 8], '0');
}

}  // namespace walkercore