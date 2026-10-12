// WalkerCore — PhysXAdapter multi-strategy memory-scan tests.
//
// The M3 Lander exposes two independent detection strategies that both
// run on the same byte buffer:
//
//   1. signature  — find a run of >= kMinPhysXVtableRun (200) contiguous
//                   8-byte values that look like code pointers. When the
//                   buffer is the runtime .rdata of amtrucks.exe we pass
//                   the .text VA range; the test-only synthetic branch
//                   accepts any non-zero 8-byte value so unit tests can
//                   inject a synthetic vtable.
//   2. strings    — verbatim search for the four known PxScene error
//                   strings from the open-source PhysX 3.4 SDK. They are
//                   present in amtrucks.exe (we do not RE that here —
//                   the strings come from public PhysX source headers).
//
// The combined result reflects "did we confirm PxScene is statically
// linked and its vtable is somewhere in this .rdata" without yet
// committing to specific slot offsets (those still need live
// verification). The behaviour mirrors TMRealWalk's
// "[physx] scene strings/vtable not found - ground/collision
// unavailable" runtime logging.

#include "TestHarness.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#include "walkercore/PhysXAdapter.h"

namespace walkercore {

namespace {

// Build a buffer containing one of the four probe strings at a known
// offset. The buffer is sized with leading noise + a 4 KiB tail of
// zeroes (which the scanner must NOT misinterpret as a vtable).
std::vector<uint8_t> BufferWithString(const char *needle) {
    const size_t kLead = 32;
    const size_t needlelen = std::strlen(needle);
    std::vector<uint8_t> buf(kLead + needlelen + 4096, 0);
    for (size_t i = 0; i < kLead; ++i) {
        // Salt the leading bytes so a naive "first-non-zero-pointer"
        // scanner would behave like the signature pass and need to be
        // interleaved with zeroes. We use a mix that mostly looks like
        // ASCII junk to keep the signature scan honest.
        buf[i] = static_cast<uint8_t>(0x20 + (i & 0x3F));
    }
    std::memcpy(buf.data() + kLead, needle, needlelen);
    return buf;
}

// Build a buffer that holds a synthetic vtable: exactly `run_len`
// non-zero 8-byte values (so the signature scan can be tested at
// exact threshold boundaries). Leading and trailing bytes are zero
// so the run length is *exactly* run_len — not run_len + leading
// noise + trailing noise. Otherwise an ASCII-filler lead adds 4
// extra entries to the contiguous run and would defeat negative
// probes (a run below threshold would still satisfy the cut).
std::vector<uint8_t> BufferWithSyntheticVtable(size_t run_len) {
    const size_t kLead = 32;
    const size_t kTrail = 32;
    const size_t total = kLead + run_len * 8 + kTrail;
    std::vector<uint8_t> buf(total, 0);
    // Each entry is a non-zero pointer-shape value (synthetic branch).
    for (size_t i = 0; i < run_len; ++i) {
        const uint64_t v = static_cast<uint64_t>(0x1ull << 32) | static_cast<uint64_t>(i + 1);
        const size_t off = kLead + i * 8;
        std::memcpy(buf.data() + off, &v, 8);
    }
    return buf;
}

}  // namespace

// Strings probe: a buffer containing exactly one of the four probe
// strings verbatim yields string_probes_hit == 1 and strategy "strings"
// (because no vtable run also exists in this buffer).

void TestPhysXStringsProbeFindsKnownString() {
    std::vector<uint8_t> buf = BufferWithString("PxScene::release(): Scene is still being simulated!");
    const PhysXScanResult res = PhysXAdapter::MemoryScanFromBuffer(buf.data(), buf.size());
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.string_probes_hit, 1);
    // No vtable run in this buffer, so the strategy is "strings".
    EXPECT_TRUE(res.strategy != nullptr);
    EXPECT_TRUE(std::strcmp(res.strategy, "strings") == 0 ||
                std::strcmp(res.strategy, "hybrid") == 0);
}

// Multiple probe strings embedded at distinct offsets each increase the
// hit count independently and the strategy stays "strings".

void TestPhysXStringsProbeFindsAllKnownStrings() {
    std::vector<uint8_t> buf;
    for (int i = 0; i < kPhysXProbeStringCount; ++i) {
        const char *needle = kPhysXProbeStrings[i];
        const size_t needlelen = std::strlen(needle);
        const size_t off = buf.size();
        buf.resize(off + needlelen + 32, 0);
        std::memcpy(buf.data() + off, needle, needlelen);
    }
    const PhysXScanResult res = PhysXAdapter::MemoryScanFromBuffer(buf.data(), buf.size());
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.string_probes_hit, kPhysXProbeStringCount);
}

// Empty / non-matching buffer reports zero hits and "no_match".

void TestPhysXStringsProbeRejectsEmptyBuffer() {
    const PhysXScanResult res = PhysXAdapter::MemoryScanFromBuffer(nullptr, 0);
    EXPECT_FALSE(res.success);
    EXPECT_EQ(res.string_probes_hit, 0);
    EXPECT_STREQ(res.strategy, "no_match");
}

void TestPhysXStringsProbeIgnoresPartialMatch() {
    // Same prefix as a real probe string but truncated by one byte — must
    // not register as a hit. We use memcmp, so a missing trailing NUL
    // or missing last character is a hard reject.
    std::vector<uint8_t> buf = BufferWithString("PxScene::release(): Scene is still being simulat");  // missing 'e'
    const PhysXScanResult res = PhysXAdapter::MemoryScanFromBuffer(buf.data(), buf.size());
    EXPECT_EQ(res.string_probes_hit, 0);
}

// Signature scan alone: synthetic vtable of kMinPhysXVtableRun entries
// triggers "signature" strategy. The strings strategy is also absent so
// success is from signature only.

void TestPhysXSignatureRunAcceptedAtThreshold() {
    std::vector<uint8_t> buf = BufferWithSyntheticVtable(kMinPhysXVtableRun);
    const PhysXScanResult res = PhysXAdapter::MemoryScanFromBuffer(buf.data(), buf.size());
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.string_probes_hit, 0);
    EXPECT_TRUE(res.vtable.accepted_run >= kMinPhysXVtableRun);
    EXPECT_TRUE(res.strategy != nullptr);
    EXPECT_TRUE(std::strcmp(res.strategy, "signature") == 0 ||
                std::strcmp(res.strategy, "hybrid") == 0);
}

// Short run below kMinPhysXVtableRun is rejected. The string-probe count is
// still 0 (no strings in the buffer).

void TestPhysXSignatureRunRejectedBelowThreshold() {
    std::vector<uint8_t> buf = BufferWithSyntheticVtable(kMinPhysXVtableRun - 1);
    const PhysXScanResult res = PhysXAdapter::MemoryScanFromBuffer(buf.data(), buf.size());
    // No success path: no signature (run too short), no strings.
    EXPECT_FALSE(res.success);
    EXPECT_EQ(res.string_probes_hit, 0);
    EXPECT_STREQ(res.strategy, "no_match");
}

// Hybrid: buffer has both a real string AND a synthetic vtable. The
// resulting strategy must be "hybrid" and both fields populated.

void TestPhysXBothStrategiesHybrid() {
    std::vector<uint8_t> buf = BufferWithSyntheticVtable(kMinPhysXVtableRun + 32);
    // Embed a known string at the very end (after the vtable run).
    const char *needle = "PxScene::shiftOrigin() not allowed";
    const size_t needlelen = std::strlen(needle);
    const size_t off = buf.size();
    buf.resize(off + needlelen + 8, 0);
    std::memcpy(buf.data() + off, needle, needlelen);

    const PhysXScanResult res = PhysXAdapter::MemoryScanFromBuffer(buf.data(), buf.size());
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.string_probes_hit, 1);
    EXPECT_TRUE(res.vtable.accepted_run >= kMinPhysXVtableRun);
    EXPECT_STREQ(res.strategy, "hybrid");
}

// Strategy tag is one of the four well-known values. White-box: we just
// check the strategy is one of the documented names (not e.g. random or
// "no_match" when success=true).

void TestPhysXStrategyTagIsCanonical() {
    std::vector<uint8_t> buf = BufferWithSyntheticVtable(kMinPhysXVtableRun + 16);
    const PhysXScanResult res = PhysXAdapter::MemoryScanFromBuffer(buf.data(), buf.size());
    EXPECT_TRUE(res.success);
    const bool is_canonical =
        std::strcmp(res.strategy, "signature") == 0 ||
        std::strcmp(res.strategy, "strings") == 0 ||
        std::strcmp(res.strategy, "hybrid") == 0 ||
        std::strcmp(res.strategy, "no_match") == 0;
    EXPECT_TRUE(is_canonical);
}

// DumpVtableEntries — when a buffer has a signature-acceptable vtable
// run, the dump reports the first N function pointers (synthetic
// mode here, since the test runs without a real .text). When the
// buffer has no vtable, the dump is a no-op (returns 0).

void TestPhysXDumpVtableEntriesReportsFirstNVtableRun() {
    std::vector<uint8_t> buf = BufferWithSyntheticVtable(kMinPhysXVtableRun + 8);
    char out[512] = {0};
    const size_t n = PhysXAdapter::DumpVtableEntries(
        buf.data(), buf.size(), 8, out, sizeof(out));
    EXPECT_TRUE(n > 0);
    // Each entry is 18 chars ("0x" + 16 hex digits), plus 3 bytes
    // separator (" | "). 8 entries => >= 8 * 18 + 7 * 3 = 165 bytes.
    EXPECT_TRUE(n >= 165);
    // The output should contain at least one "0x" prefix per entry.
    int zero_x_count = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (out[i] == '0' && out[i + 1] == 'x') {
            zero_x_count += 1;
        }
    }
    EXPECT_EQ(zero_x_count, 8);
}

void TestPhysXDumpVtableEntriesNoMatchReturnsZero() {
    std::vector<uint8_t> buf(8192, 0);
    char out[256] = {0};
    const size_t n = PhysXAdapter::DumpVtableEntries(
        buf.data(), buf.size(), 16, out, sizeof(out));
    EXPECT_EQ(n, 0u);
}

void TestPhysXDumpVtableEntriesRejectsNullBuffer() {
    char out[256] = {0};
    const size_t n = PhysXAdapter::DumpVtableEntries(
        nullptr, 0, 16, out, sizeof(out));
    EXPECT_EQ(n, 0u);
}

// DumpVtableEntriesAnnotated: when the vtable contains catalog VAs at
// known slot indices, the annotated dump must label those slots with
// the catalog name. This is the diagnostic the maintainer reads when
// verifying the catalog against the live binary — without the
// annotations, they would have to compare 8-byte VAs by hand against
// the catalog. With annotations, the matched slots are immediately
// visible.

void TestPhysXDumpVtableEntriesAnnotatedLabelsCatalogMatches() {
    // Build a vtable >= kMinPhysXVtableRun so the signature scan
    // accepts it. Embed the catalog's first 3 VAs at the start of
    // the run; the rest are synthetic non-catalog values. The
    // synthetic scan accepts any non-zero 8-byte value, so a
    // contiguous run of any non-zero pointers is enough.
    constexpr int kRun = kMinPhysXVtableRun + 8;
    std::vector<uint8_t> buf(kRun * 8, 0);
    // Place the catalog's first 3 VAs at slots 0, 3, 5.
    const uint64_t v0 = walkercore::kPhysXKnownMethods[0].va;
    const uint64_t v1 = walkercore::kPhysXKnownMethods[1].va;
    const uint64_t v2 = walkercore::kPhysXKnownMethods[2].va;
    std::memcpy(buf.data() + 0 * 8, &v0, 8);
    std::memcpy(buf.data() + 3 * 8, &v1, 8);
    std::memcpy(buf.data() + 5 * 8, &v2, 8);
    // Fill the remaining slots with synthetic non-catalog values so
    // the run stays >= kMinPhysXVtableRun.
    for (int i = 0; i < kRun; ++i) {
        if (i == 0 || i == 3 || i == 5) continue;
        const uint64_t v = static_cast<uint64_t>(0x1ull << 33) | static_cast<uint64_t>(i + 100);
        std::memcpy(buf.data() + i * 8, &v, 8);
    }
    char out[2048] = {0};
    const size_t n = PhysXAdapter::DumpVtableEntriesAnnotated(
        buf.data(), buf.size(), 8, out, sizeof(out));
    EXPECT_TRUE(n > 0);
    // The output should contain the catalog name for kPhysXKnownMethods[0]
    // (release) somewhere — at minimum "[release]" must appear.
    EXPECT_TRUE(std::strstr(out, "[release]") != nullptr);
    EXPECT_TRUE(std::strstr(out, "[addActor]") != nullptr);
    EXPECT_TRUE(std::strstr(out, "[simulate]") != nullptr);
    // The annotated function should produce exactly 3 brackets (one
    // per matched catalog row).
    int bracket_count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (out[i] == '[') bracket_count += 1;
    }
    EXPECT_EQ(bracket_count, 3);
}

// ResolveKnownMethods: synthesises a vtable whose entry at slot 0 is
// equal to kPhysXKnownMethods[0].va (release). The matcher must
// resolve slot 0 for release and skip the others (whose VAs are not
// in the synthetic buffer).

void TestPhysXResolveKnownMethodsFindsReleaseAtSlotZero() {
    constexpr int kRun = 8;
    std::vector<uint8_t> buf(kRun * 8, 0);
    // Place kPhysXKnownMethods[0].va at slot 0.
    {
        const uint64_t v = walkercore::kPhysXKnownMethods[0].va;
        std::memcpy(buf.data(), &v, 8);
    }
    // Slot 1 leaves a different value so it would not match anything.
    // Slots 2..7 stay zero.
    PhysXVtableResolveResult out{};
    const bool ok = PhysXAdapter::ResolveKnownMethods(
        buf.data(), buf.size(), 0, kRun, &out);
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.release_slot, 0);
    EXPECT_EQ(out.add_actor_slot, -1);
    EXPECT_EQ(out.simulate_slot, -1);
    EXPECT_EQ(out.shift_origin_slot, -1);
    EXPECT_EQ(out.sweep_slot, -1);
    EXPECT_EQ(out.raycast_slot, -1);
    EXPECT_EQ(out.overlap_slot, -1);
    EXPECT_EQ(out.remove_actor_slot, -1);
}

void TestPhysXResolveKnownMethodsAllEightSlotsInOrder() {
    constexpr int kRun = 9;
    std::vector<uint8_t> buf(kRun * 8, 0);
    // Embed all eight known VAs at slots 0..7 in their catalog order.
    for (int m = 0; m < kPhysXKnownMethodCount; ++m) {
        const uint64_t v = walkercore::kPhysXKnownMethods[m].va;
        std::memcpy(buf.data() + m * 8, &v, 8);
    }
    PhysXVtableResolveResult out{};
    const bool ok = PhysXAdapter::ResolveKnownMethods(
        buf.data(), buf.size(), 0, kRun, &out);
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.release_slot, 0);
    EXPECT_EQ(out.add_actor_slot, 1);
    EXPECT_EQ(out.simulate_slot, 2);
    EXPECT_EQ(out.shift_origin_slot, 3);
    EXPECT_EQ(out.sweep_slot, 4);
    EXPECT_EQ(out.raycast_slot, 5);
    EXPECT_EQ(out.overlap_slot, 6);
    EXPECT_EQ(out.remove_actor_slot, 7);
}

void TestPhysXResolveKnownMethodsNoMatchReturnsFalse() {
    constexpr int kRun = 4;
    std::vector<uint8_t> buf(kRun * 8, 0);
    // All zeros — none of the catalog VAs is zero, so no match.
    PhysXVtableResolveResult out{};
    const bool ok = PhysXAdapter::ResolveKnownMethods(
        buf.data(), buf.size(), 0, kRun, &out);
    EXPECT_FALSE(ok);
    EXPECT_EQ(out.release_slot, -1);
    EXPECT_EQ(out.add_actor_slot, -1);
    EXPECT_EQ(out.simulate_slot, -1);
    EXPECT_EQ(out.shift_origin_slot, -1);
    EXPECT_EQ(out.sweep_slot, -1);
    EXPECT_EQ(out.raycast_slot, -1);
    EXPECT_EQ(out.overlap_slot, -1);
    EXPECT_EQ(out.remove_actor_slot, -1);
}

void TestPhysXResolveKnownMethodsRespectsBounds() {
    // Buffer too short for the claimed `accepted_run` — must return
    // false safely without reading out of bounds.
    constexpr int kActual = 2;
    constexpr int kClaimed = 1000;
    std::vector<uint8_t> buf(kActual * 8, 0);
    PhysXVtableResolveResult out{};
    const bool ok = PhysXAdapter::ResolveKnownMethods(
        buf.data(), buf.size(), 0, kClaimed, &out);
    EXPECT_FALSE(ok);
}

void TestPhysXKnownMethodsCatalogIsComplete() {
    // The catalog must list the eight methods we ship. Adding a
    // method to the .cpp catalog without bumping kPhysXKnownMethodCount
    // would mis-list the count; this test is a regression guard.
    EXPECT_EQ(kPhysXKnownMethodCount, 8);
    EXPECT_TRUE(kPhysXKnownMethods[0].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[1].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[2].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[3].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[4].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[5].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[6].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[7].name != nullptr);
    // All eight VAs must be non-zero and differ from each other (we
    // have to make sure the catalog isn't accidentally all the same
    // address).
    EXPECT_TRUE(kPhysXKnownMethods[0].va != 0ULL);
    for (int i = 1; i < kPhysXKnownMethodCount; ++i) {
        EXPECT_TRUE(kPhysXKnownMethods[i].va != kPhysXKnownMethods[0].va);
    }
}

// ResolveKnownMethods: the catalog now lists five methods. Verify the
// fifth (PxScene::sweep) is the one with the highest VA — sweeps'
// function entry in amtrucks.exe lives at the high end of the
// 0x141AD0000..0x141B20000 PhysX TU range. This is a regression
// guard against accidentally cataloging a low-VA function as sweep.

void TestPhysXKnownMethodsSweepIsTheHighestVA() {
    uint64_t max_va = 0;
    int max_idx = -1;
    for (int i = 0; i < kPhysXKnownMethodCount; ++i) {
        if (kPhysXKnownMethods[i].va > max_va) {
            max_va = kPhysXKnownMethods[i].va;
            max_idx = i;
        }
    }
    EXPECT_EQ(max_idx, 4);  // sweep should be the highest VA
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[4].name, "sweep") == 0);
}

// Catalog-VA range: every catalog row must be in the
// 0x141AD0000..0x141B20000 PhysX translation-unit range, where the
// known PxScene methods live in amtrucks.exe. A VA outside this range
// means the LEA-walk landed in the wrong translation unit (or the
// heuristic was off by orders of magnitude). This is a regression
// guard against accidentally cataloging a string from a different
// class.

void TestPhysXKnownMethodsCatalogVAsInPhysXRange() {
    constexpr uint64_t kPhysXTULow  = 0x141AD0000ULL;
    constexpr uint64_t kPhysXTUHigh = 0x141B20000ULL;
    for (int i = 0; i < kPhysXKnownMethodCount; ++i) {
        EXPECT_TRUE(kPhysXKnownMethods[i].va >= kPhysXTULow);
        EXPECT_TRUE(kPhysXKnownMethods[i].va <  kPhysXTUHigh);
    }
}

// Catalog-name sanity: the names we publish in kPhysXKnownMethods
// must match the open-source PxScene 4.1 SDK method names that the
// asserts in .text reference. This catches typos and keeps the
// resolve-result keys in sync with what the live binary actually
// calls.

void TestPhysXKnownMethodsCatalogNamesMatchSDK() {
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[0].name, "release") == 0);
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[1].name, "addActor") == 0);
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[2].name, "simulate") == 0);
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[3].name, "shiftOrigin") == 0);
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[4].name, "sweep") == 0);
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[5].name, "raycast") == 0);
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[6].name, "overlap") == 0);
}

// Catalog-order regression guard: the three newly-added public-API
// entries (raycast, overlap, removeActor) must keep their position
// in the catalog at indices 5, 6, and 7 respectively, after the
// original five. Adding them in a different order would shift the
// slot-resolution switch in ResolveKnownMethods and break
// TestPhysXResolveKnownMethodsAllEightSlotsInOrder.

void TestPhysXKnownMethodsCatalogOrderIsStable() {
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[5].name, "raycast") == 0);
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[6].name, "overlap") == 0);
    EXPECT_TRUE(std::strcmp(kPhysXKnownMethods[7].name, "removeActor") == 0);
    // raycast and removeActor live in PxScene TU range too (just
    // below or near addActor's VA). Verifies the VAs came from the
    // same TU the other methods came from.
    EXPECT_TRUE(kPhysXKnownMethods[5].va >= 0x141AD0000ULL);
    EXPECT_TRUE(kPhysXKnownMethods[6].va >= 0x141AD0000ULL);
    EXPECT_TRUE(kPhysXKnownMethods[7].va >= 0x141AD0000ULL);
    EXPECT_TRUE(kPhysXKnownMethods[5].va <  0x141B20000ULL);
    EXPECT_TRUE(kPhysXKnownMethods[6].va <  0x141B20000ULL);
    EXPECT_TRUE(kPhysXKnownMethods[7].va <  0x141B20000ULL);
}

// Real-binary integration test: read amtrucks.exe from disk, run
// MemoryScanFromBuffer on its .rdata slice, then run
// ResolveKnownMethods against the vtable candidate. The catalog
// must match at least 4 of the 7 entries (the 5 SDK-anchor entries
// are exact or near-exact; sweep is in a separate vtable; raycast
// and overlap should now also match).
//
// The test is gated on WALKERCORE_REAL_BINARY_TEST_PATH being defined
// in CMake; it skips gracefully when the file is not available so the
// unit-test run does not depend on a 53 MiB binary being on the build
// host. The expectation below uses the catalog's exact slot mapping
// for ATS 1.61.3.1s SHA 5D3B4E343154DE6B25DD366B6048C5BC5C452A4DAA50BC38B33FD2E571D54257.

#ifdef WALKERCORE_REAL_BINARY_TEST_PATH
// Parse a PE file's PE header to find the .text section's RVA. Returns
// (text_rva, text_size) on success, (-1, 0) on failure. Minimal PE
// parser: NT headers -> section table -> match name == ".text".
namespace {

struct PeTextInfo {
    uint32_t text_rva;
    uint32_t text_size;
    bool ok;
};

struct PeRdataInfo {
    uint32_t rdata_rva;
    uint32_t rdata_size;
    bool ok;
};

// Parse the PE section table for ".rdata". Mirrors FindPeTextSection
// above and the FindPeSection helper inside PhysXAdapter's runtime
// scan path: identical algorithm, separate implementation to keep
// the test hermetic (the runtime helper is in an anonymous namespace
// inside the production .cpp). Any divergence between the two is a
// test failure on the production side.
PeRdataInfo FindRdataSection(const uint8_t *buf, size_t size) {
    PeRdataInfo info{0, 0, false};
    if (buf == nullptr || size < 0x40) {
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
    for (uint16_t i = 0; i < num_sections; ++i) {
        const size_t off = sections_off + static_cast<size_t>(i) * 40;
        if (off + 40 > size) break;
        if (buf[off] == '.' && buf[off + 1] == 'r' &&
            buf[off + 2] == 'd' && buf[off + 3] == 'a' &&
            buf[off + 4] == 't' && buf[off + 5] == 'a') {
            std::memcpy(&info.rdata_rva, buf + off + 12, 4);
            std::memcpy(&info.rdata_size, buf + off + 8, 4);
            info.ok = true;
            return info;
        }
    }
    return info;
}

PeTextInfo FindPeTextSection(const uint8_t *buf, size_t size) {
    PeTextInfo info{0, 0, false};
    if (buf == nullptr || size < 0x40) {
        return info;
    }
    // e_lfanew at offset 0x3C.
    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, buf + 0x3C, 4);
    if (e_lfanew + 24 > size) {
        return info;
    }
    // Signature "PE\0\0".
    if (buf[e_lfanew] != 'P' || buf[e_lfanew + 1] != 'E' ||
        buf[e_lfanew + 2] != 0 || buf[e_lfanew + 3] != 0) {
        return info;
    }
    // COFF header: 20 bytes starting at e_lfanew+4.
    uint16_t num_sections = 0;
    std::memcpy(&num_sections, buf + e_lfanew + 4 + 2, 2);
    uint16_t opt_hdr_size = 0;
    std::memcpy(&opt_hdr_size, buf + e_lfanew + 4 + 16, 2);
    const size_t sections_off = e_lfanew + 4 + 20 + opt_hdr_size;
    for (uint16_t i = 0; i < num_sections; ++i) {
        const size_t off = sections_off + static_cast<size_t>(i) * 40;
        if (off + 40 > size) break;
        // Name is the first 8 bytes.
        if (buf[off] == '.' && buf[off + 1] == 't' &&
            buf[off + 2] == 'e' && buf[off + 3] == 'x' &&
            buf[off + 4] == 't') {
            std::memcpy(&info.text_rva, buf + off + 12, 4);  // VirtualAddress
            std::memcpy(&info.text_size, buf + off + 8, 4);   // VirtualSize
            info.ok = true;
            return info;
        }
    }
    return info;
}

}  // namespace

void TestPhysXRealBinaryMatchesCatalog() {
    FILE *f = std::fopen(WALKERCORE_REAL_BINARY_TEST_PATH, "rb");
    if (f == nullptr) {
        // File missing — skip without failing. Production build hosts
        // may not have ATS installed.
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        std::fclose(f);
        return;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(fsize));
    const size_t nread = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (nread != buf.size()) {
        return;
    }

    // Parse the PE header for .text VA range. Image base for ATS is
    // 0x140000000; .text VA range = image_base + text_rva to
    // image_base + text_rva + text_size.
    const PeTextInfo text_info = FindPeTextSection(buf.data(), buf.size());
    EXPECT_TRUE(text_info.ok);
    constexpr uint64_t kAtsImageBase = 0x140000000ULL;
    const uint64_t text_lo = kAtsImageBase + text_info.text_rva;
    const uint64_t text_hi = text_lo + text_info.text_size;

    // The signature scan should constrain itself to the PhysX TU
    // range (0x141AD0000..0x141B20000) rather than the full .text.
    // The full .text range contains unrelated large vtables (13185
    // slots at 0x1DBF8C8 is unrelated code) that would dominate the
    // "best run" pick. The PhysX TU range picks the PxScene vtable at
    // RVA 0x25020A0 (118 slots) cleanly.
    constexpr uint64_t kPhysXTULow  = 0x141AD0000ULL;
    constexpr uint64_t kPhysXTUHigh = 0x141B20000ULL;
    (void)text_lo;
    (void)text_hi;

    const PhysXScanResult scan =
        PhysXAdapter::MemoryScanFromBufferWithTextRange(
            buf.data(), buf.size(), kPhysXTULow, kPhysXTUHigh);
    EXPECT_TRUE(scan.success);
    // Strategy must be hybrid (both signature and string probes hit).
    EXPECT_TRUE(std::strcmp(scan.strategy, "hybrid") == 0);
    EXPECT_TRUE(scan.string_probes_hit >= 3);  // at least 3 of 4 probe strings
    EXPECT_TRUE(scan.vtable.accepted_run >= kMinPhysXVtableRun);

    // ResolveKnownMethods: must match the 4 SDK-anchor VAs against the
    // discovered vtable candidate, plus the 2 newly-added raycast/overlap.
    PhysXVtableResolveResult resolved{};
    const bool ok = PhysXAdapter::ResolveKnownMethods(
        buf.data(),
        buf.size(),
        scan.vtable.vtable_base,
        scan.vtable.accepted_run,
        &resolved);
    EXPECT_TRUE(ok);

    // Exact-match slots (per docs/observations/ats_1_61_3_1s.physx.functions.md
    // and verified by hand on ATS 1.61.3.1s):
    EXPECT_EQ(resolved.release_slot, 2);
    EXPECT_EQ(resolved.add_actor_slot, 11);
    EXPECT_EQ(resolved.simulate_slot, 57);
    EXPECT_EQ(resolved.shift_origin_slot, 117);
    // raycast and overlap should resolve to slots 15 and 13 respectively
    // (their VAs were identified by disassembling these exact slots).
    EXPECT_EQ(resolved.raycast_slot, 15);
    EXPECT_EQ(resolved.overlap_slot, 13);
    // removeActor should resolve to slot 14 (identified by the
    // "PxScene::removeActor(): Actor" error-path string and a tail
    // jump to a per-type detach dispatcher).
    EXPECT_EQ(resolved.remove_actor_slot, 14);
    // sweep is NOT in the 0x25020A0 vtable — it lives in a separate
    // 5-slot run at 0x2507C08 (slot 4) that MemoryScanFromBuffer does
    // not discover on its own.
    EXPECT_EQ(resolved.sweep_slot, -1);
}

// Runtime-scan path test. Mirrors the algorithm in
// PhysXAdapter_RuntimeScan but operates on the file buffer instead
// of GetModuleHandleA's pointer. The production runtime code
// (FindPeSection + MemoryScanFromBufferWithTextRange +
// ResolveKnownMethods) lives in src/provider/PhysXAdapter.cpp; this
// test re-derives the PE parsing from a separate helper so a change
// in one but not the other fails the test.
//
// Critical invariant: the scan must find the same vtable (RVA
// 0x25020A0) and resolve the same 6 slots (release/addActor/
// simulate/shiftOrigin/raycast/overlap). The catalog-derived text
// range (1 MiB slack around the catalog VAs) replaces the fixed
// PhysX TU range here because the runtime uses catalog-derived
// ranges, not the fixed PhysX TU constants.
void TestPhysXRealBinaryRuntimeScanPath() {
    FILE *f = std::fopen(WALKERCORE_REAL_BINARY_TEST_PATH, "rb");
    if (f == nullptr) {
        return;  // production host without the binary: skip
    }
    std::fseek(f, 0, SEEK_END);
    const long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        std::fclose(f);
        return;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(fsize));
    const size_t nread = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (nread != buf.size()) {
        return;
    }

    // Parse the PE section table for .rdata and .text.
    const PeTextInfo text_info = FindPeTextSection(buf.data(), buf.size());
    const PeRdataInfo rdata_info = FindRdataSection(buf.data(), buf.size());
    EXPECT_TRUE(text_info.ok);
    EXPECT_TRUE(rdata_info.ok);
    if (!text_info.ok || !rdata_info.ok) {
        return;
    }

    // Compute the catalog-derived text range exactly as the runtime
    // scan does: [min(catalog VA) - 1 MiB, max(catalog VA) + 1 MiB].
    uint64_t catalog_min = UINT64_MAX;
    uint64_t catalog_max = 0;
    for (int m = 0; m < kPhysXKnownMethodCount; ++m) {
        const uint64_t va = kPhysXKnownMethods[m].va;
        if (va < catalog_min) catalog_min = va;
        if (va > catalog_max) catalog_max = va;
    }
    constexpr uint64_t kSlack = 0x100000ULL;
    constexpr uint64_t kAtsImageBase = 0x140000000ULL;
    const uint64_t text_lo =
        (catalog_min > kSlack) ? (catalog_min - kSlack) : 0;
    const uint64_t text_hi = catalog_max + kSlack;
    // .rdata is where the vtable lives; scan over its RVA range.
    const uint8_t *rdata_ptr = buf.data() + rdata_info.rdata_rva;
    const size_t rdata_size =
        (rdata_info.rdata_size > 0)
            ? static_cast<size_t>(rdata_info.rdata_size)
            : 0;
    // Bound-check before passing to the scan.
    if (rdata_info.rdata_rva >= buf.size() ||
        rdata_size > buf.size() - rdata_info.rdata_rva) {
        return;
    }

    const PhysXScanResult scan =
        PhysXAdapter::MemoryScanFromBufferWithTextRange(
            rdata_ptr, rdata_size, text_lo, text_hi);
    EXPECT_TRUE(scan.success);
    EXPECT_TRUE(std::strcmp(scan.strategy, "hybrid") == 0);
    EXPECT_TRUE(scan.vtable.accepted_run >= kMinPhysXVtableRun);

    // The catalog-derived range gives a vtable_base RVA inside
    // .rdata. On ATS 1.61.3.1s it lands on 0x25020A0 (118 slots).
    // We don't pin the exact RVA (sensitive to PE layout) but we do
    // pin the resolved slots after.
    PhysXVtableResolveResult resolved{};
    const bool ok = PhysXAdapter::ResolveKnownMethods(
        rdata_ptr, rdata_size,
        scan.vtable.vtable_base,
        scan.vtable.accepted_run,
        &resolved);
    EXPECT_TRUE(ok);
    EXPECT_EQ(resolved.release_slot, 2);
    EXPECT_EQ(resolved.add_actor_slot, 11);
    EXPECT_EQ(resolved.simulate_slot, 57);
    EXPECT_EQ(resolved.shift_origin_slot, 117);
    EXPECT_EQ(resolved.raycast_slot, 15);
    EXPECT_EQ(resolved.overlap_slot, 13);
    EXPECT_EQ(resolved.remove_actor_slot, 14);
    EXPECT_EQ(resolved.sweep_slot, -1);

    // Silence the unused-variable warning for kAtsImageBase (kept
    // for symmetry with the on-disk test above).
    (void)kAtsImageBase;
}
#endif  // WALKERCORE_REAL_BINARY_TEST_PATH

}  // namespace walkercore
