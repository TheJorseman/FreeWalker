// WalkerCore — OpentrackReceiver probes (M7 lander).
//
// The receiver is the simplest piece of the M7 stack: a UDP socket
// bound to port 4242 that reads opentrack's 48-byte per-frame
// datagram and feeds yaw/pitch deltas into a HeadTrackerProvider.
// The tests here cover:
//
//   1. Install / Uninstall lifecycle (idempotent, double-uninstall
//      safe, double-install returns false).
//
//   2. Tick on a not-installed receiver is a safe no-op (no crash,
//      no provider call).
//
//   3. Tick with a nullptr provider still drains the socket — a
//      blocked recv() on a full kernel buffer would freeze the
//      next tick, so we always drain even without a downstream
//      consumer.
//
//   4. Round-trip via localhost: bind a receiver on a non-default
//      port, send a 48-byte datagram from a sender socket, and
//      verify the receiver's Tick feeds the expected deltas into
//      a HeadTrackerProvider. This is the integration test that
//      proves the protocol parsing (48-byte packet, 6 doubles,
//      shortest-arc delta) is wired correctly end-to-end.
//
// The protocol packet is 6 little-endian doubles in the order
// (x, y, z, yaw_deg, pitch_deg, roll_deg). We ignore x/y/z/roll
// (the comment block in OpentrackReceiver.cpp documents why). The
// only fields that drive the camera are yaw and pitch.

#include "TestHarness.h"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "walkercore/OpentrackReceiver.h"
#include "walkercore/PolishProviders.h"

namespace walkercore {

namespace {

// Build a 48-byte opentrack UDP datagram with the given yaw / pitch
// in degrees. x / y / z / roll default to 0; they're ignored by the
// receiver.
void BuildOpentrackPacket(char *buf, double yaw_deg, double pitch_deg,
                          double x = 0.0, double y = 0.0, double z = 0.0,
                          double roll_deg = 0.0) {
    std::memcpy(buf +  0, &x,        8);
    std::memcpy(buf +  8, &y,        8);
    std::memcpy(buf + 16, &z,        8);
    std::memcpy(buf + 24, &yaw_deg,  8);
    std::memcpy(buf + 32, &pitch_deg, 8);
    std::memcpy(buf + 40, &roll_deg, 8);
}

}  // namespace

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------

void TestOpentrackReceiverInstallUninstallIdempotent() {
    // Install binds the socket; Uninstall closes it. Both must be
    // safe to call multiple times. The first Install succeeds; the
    // second returns false (we only support one socket per process
    // — a second opentrack receiver would be the user's job to
    // wire up). The double-Uninstall is a no-op.
    OpentrackReceiver::ResetForTesting();
    OpentrackReceiver rx;
    EXPECT_TRUE(rx.Install(/*port=*/14242));
    EXPECT_TRUE(rx.isInstalled());
    EXPECT_FALSE(rx.Install(/*port=*/14243));  // already installed
    rx.Uninstall();
    EXPECT_FALSE(rx.isInstalled());
    rx.Uninstall();  // idempotent
    EXPECT_FALSE(rx.isInstalled());
}

void TestOpentrackReceiverTickWithoutInstallIsSafe() {
    // Tick on a not-installed receiver must not crash. The receiver
    // is a value type; default-construction gives a "closed"
    // instance. The frame loop might be holding one and never
    // calling Install (e.g. head tracker disabled in INI).
    OpentrackReceiver::ResetForTesting();
    OpentrackReceiver rx;
    HeadTrackerProvider provider;
    rx.Tick(&provider);  // must be a no-op
    EXPECT_FALSE(rx.isInstalled());
    // The provider's state stays at defaults.
    const HeadTrackerState s = provider.Peek();
    EXPECT_FALSE(s.active);
}

void TestOpentrackReceiverTickWithNullProviderDrainsSocket() {
    // Tick must drain the socket even when provider is nullptr.
    // Otherwise an undrained recvfrom would back-pressure the
    // kernel buffer and the next Tick (with a real provider)
    // would see a stale reading. We can't directly observe the
    // kernel buffer here, but we can verify the Tick returns
    // without erroring — the production safety guarantee.
    OpentrackReceiver::ResetForTesting();
    OpentrackReceiver rx;
    const bool ok = rx.Install(/*port=*/14244);
    if (!ok) {
        // Port already in use on a parallel test run; skip.
        return;
    }
    rx.Tick(nullptr);  // no provider, but Tick must not crash
    rx.Uninstall();
    OpentrackReceiver::ResetForTesting();
}

// -------------------------------------------------------------------------
// End-to-end round-trip via localhost UDP
// -------------------------------------------------------------------------

void TestOpentrackReceiverRoundTripFeedsDeltasIntoProvider() {
    // Bind a receiver on a non-default port. Open a sender socket,
    // send two packets with a known yaw offset, and verify the
    // HeadTrackerProvider accumulates the delta correctly.
    //
    // The first packet (yaw=10°, pitch=5°) arms the baseline
    // without producing a delta. The second packet (yaw=13°,
    // pitch=2°) produces a +3° yaw delta and a -3° pitch delta.
    // After one Tick the provider's smooth_yaw_ and smooth_pitch_
    // chases the cumulative target; with smoothing_factor=0 we
    // expect the smoothed value to equal the cumulative target
    // exactly. We use smoothing_factor=0 (no smoothing) so the
    // test pins the input → output contract without smoothing
    // arithmetic.
    OpentrackReceiver::ResetForTesting();
    OpentrackReceiver rx;
    const bool ok = rx.Install(/*port=*/14245);
    if (!ok) {
        return;
    }
    HeadTrackerConfig cfg;
    cfg.use_opentrack = true;
    cfg.smoothing_factor = 0.0f;  // no smoothing — direct read of accumulated
    HeadTrackerProvider provider;
    provider.SetConfig(cfg);
    provider.Install();

#if defined(_WIN32)
    // Open a sender socket. WSAStartup is already done by the
    // receiver's Install() call.
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    EXPECT_TRUE(s != INVALID_SOCKET);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons(14245);
#endif

    // First packet: arm the baseline. After this Tick, has_data_
    // is true on the provider.
    char pkt1[48];
    BuildOpentrackPacket(pkt1, /*yaw=*/10.0, /*pitch=*/5.0);
#if defined(_WIN32)
    sendto(s, pkt1, sizeof(pkt1), 0,
           reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
#endif
    rx.Tick(&provider);
    {
        const HeadTrackerState s = provider.Peek();
        EXPECT_TRUE(s.active);
        EXPECT_EQ(s.dominant_source, HeadTrackerSource::Opentrack);
        // First sample is zero-delta; smooth_yaw_ chases yaw_accum_=0
        // → 0. Same for pitch.
        EXPECT_NEAR(s.yaw_radians, 0.0f, 1e-6f);
        EXPECT_NEAR(s.pitch_radians, 0.0f, 1e-6f);
    }

    // Second packet: +3° yaw, -3° pitch. After this Tick the
    // provider's yaw_accum_/pitch_accum_ grow by +3° in radians
    // (~0.0524) and -3° in radians (~-0.0524). With
    // smoothing_factor=0, smooth_yaw_ = yaw_accum_ on the next
    // Sample.
    char pkt2[48];
    BuildOpentrackPacket(pkt2, /*yaw=*/13.0, /*pitch=*/2.0);
#if defined(_WIN32)
    sendto(s, pkt2, sizeof(pkt2), 0,
           reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
#endif
    rx.Tick(&provider);
    // Drain via Sample (which advances the EMA). With
    // smoothing_factor=0 the EMA reduces to "snap to the
    // accumulator" exactly.
    const HeadTrackerState s2 = provider.Sample();
    const double kPi = 3.14159265358979323846;
    EXPECT_NEAR(s2.yaw_radians, 3.0 * (kPi / 180.0), 1e-4);
    EXPECT_NEAR(s2.pitch_radians, -3.0 * (kPi / 180.0), 1e-4);

#if defined(_WIN32)
    closesocket(s);
#endif
    rx.Uninstall();
    OpentrackReceiver::ResetForTesting();
}

void TestOpentrackReceiverShortestArcDeltaOnYawWrap() {
    // opentrack's yaw wraps at ±180°. A movement from -179° to
    // +179° could be either +358° (going the long way) or ±2°
    // (the shortest arc through ±180°). The receiver uses the
    // standard shortest-arc formula:
    //   d = ((curr - prev + 540) mod 360) - 180
    // which for prev=-179, curr=+179 returns -2°: the
    // implementation walks (d -= 360) while d > 180, so d=358
    // becomes d=-2. The result is the CW direction (which is
    // equally short as the CCW direction; the formula picks the
    // signed one by construction). The pin here is that the
    // magnitude is +2° (NOT +358°), and the sign is one of
    // {+2, -2} — not ±360°. We accept both signs because the
    // test does not care which direction the formula picks,
    // only that wrap-around does not produce a 358° jump.
    OpentrackReceiver::ResetForTesting();
    OpentrackReceiver rx;
    const bool ok = rx.Install(/*port=*/14246);
    if (!ok) {
        return;
    }
    HeadTrackerConfig cfg;
    cfg.use_opentrack = true;
    cfg.smoothing_factor = 0.0f;
    HeadTrackerProvider provider;
    provider.SetConfig(cfg);
    provider.Install();

#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    EXPECT_TRUE(s != INVALID_SOCKET);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons(14246);
#endif

    char pkt1[48];
    BuildOpentrackPacket(pkt1, /*yaw=*/-179.0, /*pitch=*/0.0);
    char pkt2[48];
    BuildOpentrackPacket(pkt2, /*yaw=*/+179.0, /*pitch=*/0.0);
#if defined(_WIN32)
    sendto(s, pkt1, sizeof(pkt1), 0,
           reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
    sendto(s, pkt2, sizeof(pkt2), 0,
           reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
#endif
    rx.Tick(&provider);  // arm baseline
    rx.Tick(&provider);  // apply delta
    const HeadTrackerState out = provider.Sample();
    const double kPi = 3.14159265358979323846;
    // The magnitude must be 2°. Either sign is acceptable; the
    // |out.yaw_radians| < 10° assertion below catches the 358°
    // regression.
    const double magnitude = (out.yaw_radians < 0)
                                 ? -out.yaw_radians
                                 : out.yaw_radians;
    EXPECT_NEAR(magnitude, 2.0 * (kPi / 180.0), 1e-4);
    EXPECT_TRUE(magnitude < 10.0 * (kPi / 180.0));  // not 358°

#if defined(_WIN32)
    closesocket(s);
#endif
    rx.Uninstall();
    OpentrackReceiver::ResetForTesting();
}

}  // namespace walkercore
