// WalkerCore — CameraHijack resolver test seams.
//
// The CameraHijack module exposes two pure helpers (ComputeDispatcherVa +
// IsRvaExecutable) that don't need a live game process. This file
// exercises them with hand-built inputs and pins the install math so a
// future edit to kDispatcherRva or the section-walk loop is caught
// immediately by the CI gate.
//
// The install path that actually calls MinHook is integration-tested
// separately (it needs a real game process) and is out of scope for
// the unit-test suite.

#include "TestHarness.h"

#include <cstdint>

#include "walkercore/CameraHijack.h"

namespace walkercore {

namespace {

// Small section table for boundary tests. .text starts at RVA 0x1000
// and runs to 0x1FFF; .rdata starts at RVA 0x2000 and runs to 0x3FFF.
constexpr CameraHijack::PeSection kSmallSections[] = {
    {0x1000u, 0x1000u, 0x60000020u},   // .text at RVA 0x1000 (4 KiB)
    {0x2000u, 0x2000u, 0x40000040u},   // .rdata at RVA 0x2000 (8 KiB)
};

// Realistic amtrucks.exe (ATS 1.61.3.1s) section table. .text is
// huge (RVA 0x1000, vsize 0x1DBC800 = 31 MiB) and ends at RVA
// 0x1DBD7FF. .rdata starts at RVA 0x1DBE000 (vsize 0x91EA00 =
// ~9 MiB). kDispatcherRva = 0xC22F40 must fall inside .text on this
// layout. If a future edit changes kDispatcherRva, this table will
// catch the regression only if the new RVA still lies within .text;
// if not, IsRvaExecutable will return false and the test fails.
constexpr CameraHijack::PeSection kAts161Sections[] = {
    {0x1DBC800u, 0x1000u,    0x60000020u},   // .text
    {0x91EA00u,  0x1DBE000u, 0x40000040u},   // .rdata
};

}  // namespace

void TestCameraHijackComputeDispatcherVaZero() {
    EXPECT_EQ(CameraHijack::ComputeDispatcherVa(0), 0xC22F40u);
}

void TestCameraHijackComputeDispatcherVaAtsKbExe() {
    // ATS image base is 0x140000000 (PE32+ optional header field 24).
    EXPECT_EQ(CameraHijack::ComputeDispatcherVa(0x140000000ULL),
              0x140C22F40ULL);
}

void TestCameraHijackComputeDispatcherVaEts2() {
    // ETS2 image base is 0x140000000 for the 64-bit build too.
    EXPECT_EQ(CameraHijack::ComputeDispatcherVa(0x140000000ULL),
              0x140C22F40ULL);
}

void TestCameraHijackIsRvaExecutableInText() {
    // RVA 0x1500 falls inside .text (RVA 0x1000..0x1FFF).
    EXPECT_TRUE(CameraHijack::IsRvaExecutable(
        0x1500u, kSmallSections,
        sizeof(kSmallSections) / sizeof(kSmallSections[0])));
}

void TestCameraHijackIsRvaExecutableAtBoundary() {
    // RVA 0x1000 is the start of .text — inclusive boundary.
    EXPECT_TRUE(CameraHijack::IsRvaExecutable(
        0x1000u, kSmallSections,
        sizeof(kSmallSections) / sizeof(kSmallSections[0])));
}

void TestCameraHijackIsRvaExecutableAtBoundaryJustAbove() {
    // RVA 0x3000 falls outside .text (RVA 0x1000..0x1FFF) and inside
    // .rdata (RVA 0x2000..0x3FFF) — which is not executable.
    EXPECT_FALSE(CameraHijack::IsRvaExecutable(
        0x3000u, kSmallSections,
        sizeof(kSmallSections) / sizeof(kSmallSections[0])));
}

void TestCameraHijackIsRvaExecutableInRdata() {
    // RVA 0x2500 is in .rdata, which is not executable.
    EXPECT_FALSE(CameraHijack::IsRvaExecutable(
        0x2500u, kSmallSections,
        sizeof(kSmallSections) / sizeof(kSmallSections[0])));
}

void TestCameraHijackIsRvaExecutableOutOfAllSections() {
    // RVA 0x5000 is in no section.
    EXPECT_FALSE(CameraHijack::IsRvaExecutable(
        0x5000u, kSmallSections,
        sizeof(kSmallSections) / sizeof(kSmallSections[0])));
}

void TestCameraHijackIsRvaExecutableEmptyTable() {
    // No sections: any RVA is rejected.
    EXPECT_FALSE(CameraHijack::IsRvaExecutable(0x1500u, nullptr, 0));
    EXPECT_FALSE(CameraHijack::IsRvaExecutable(0x1500u, kSmallSections, 0));
}

void TestCameraHijackDispatcherRvaInText() {
    // kDispatcherRva = 0xC22F40 must be inside the live binary's
    // .text section (RVA 0x1000..0x1DBD7FF). If a future edit moves
    // kDispatcherRva or the binary's section table shifts, this
    // test fails immediately.
    EXPECT_TRUE(CameraHijack::IsRvaExecutable(
        0xC22F40u, kAts161Sections,
        sizeof(kAts161Sections) / sizeof(kAts161Sections[0])));
}

}  // namespace walkercore