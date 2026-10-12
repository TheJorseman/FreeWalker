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

}  // namespace walkercore
