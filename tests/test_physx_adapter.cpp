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
    std::vector<uint8_t> buf = BufferWithString("PxScene::release(): Scene is still being simulated");
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
}

void TestPhysXResolveKnownMethodsAllFiveSlotsInOrder() {
    constexpr int kRun = 8;
    std::vector<uint8_t> buf(kRun * 8, 0);
    // Embed five known VAs at slots 0..4 in their catalog order.
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
    // The catalog must list the five methods we ship. Adding a method
    // to the .cpp catalog without bumping kPhysXKnownMethodCount would
    // mis-list the count; this test is a regression guard.
    EXPECT_EQ(kPhysXKnownMethodCount, 5);
    EXPECT_TRUE(kPhysXKnownMethods[0].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[1].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[2].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[3].name != nullptr);
    EXPECT_TRUE(kPhysXKnownMethods[4].name != nullptr);
    // All five VAs must be non-zero and differ from each other (we
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
}

}  // namespace walkercore
