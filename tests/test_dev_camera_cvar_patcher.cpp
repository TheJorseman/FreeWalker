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
    EXPECT_STREQ("0x223F6C0", "0x223F6C0");  // sentinel
    EXPECT_EQ(p->name_rva(), 0x223F6C0ULL);
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
    EXPECT_EQ(p->skips_value_unreadable(), 0);
    EXPECT_EQ(p->skips_protect_failed(), 0);
    EXPECT_EQ(p->last_result().cvar_name_va, img.name_va);
    EXPECT_EQ(p->last_result().cvar_value_va, img.value_va);
}

}  // namespace walkercore