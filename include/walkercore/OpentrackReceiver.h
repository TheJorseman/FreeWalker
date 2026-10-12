// WalkerCore — Opentrack UDP receiver (M7 partial).
//
// Implements the opentrack "UDP" output protocol on port 4242. The
// opentrack project (github.com/opentrack/opentrack) publishes the
// per-frame head pose as a 48-byte UDP datagram: six little-endian
// IEEE-754 doubles in the order (x, y, z, yaw, pitch, roll). yaw /
// pitch / roll are in degrees. opentrack is the de-facto replacement
// for TrackIR on Linux + Mac and is increasingly common on Windows
// for users who want a non-TIR head tracker (IR tracker, webcam,
// phone accelerometer via the Android opentrack app, etc.).
//
// We deliberately do NOT implement the TrackIR FreeTrackClient.dll
// path: the FreeTrack protocol uses shared memory with a fixed
// layout that has shifted across releases; opentrack speaks both
// the legacy FreeTrack protocol AND a much simpler UDP one, and
// the UDP one is the one we land. Users who own a TrackIR can
// point opentrack at it and forward through the UDP link; users
// without TrackIR can use any of opentrack's input modules (face
// tracker, IR, etc.) and get them through this same socket.
//
// Lifecycle:
//   - Install() opens the socket, sets the previous-sample cache to
//     a "no previous sample" sentinel, and returns true on success.
//     On failure (e.g. another opentrack instance is already bound
//     to 4242), returns false and Tick() becomes a no-op.
//   - Tick() reads the most recent datagram (non-blocking) and
//     computes yaw_delta_rad / pitch_delta_rad from the previous
//     sample. The deltas are fed into a HeadTrackerProvider as
//     Opentrack-source samples. On the very first sample, the
//     deltas are zero — we don't have a baseline to subtract from.
//   - Uninstall() closes the socket.
//
// Platform note: we use the Winsock2 API (winsock2.h / WSADATA /
// SOCKET) on Windows. The DLL only loads inside the SCS loader on
// Windows, so Winsock2 is the right choice; a Linux port would
// swap to POSIX sockets but the protocol logic is identical.

#ifndef WALKERCORE_OPENTRACK_RECEIVER_H
#define WALKERCORE_OPENTRACK_RECEIVER_H

#include "walkercore/PolishProviders.h"

namespace walkercore {

class OpentrackReceiver {
public:
    // opentrack default port. Configurable per-user via the
    // M6 INI in a future lander; hard-coded for now because the
    // opentrack project has not changed this port in 10+ years
    // and any non-default would also be a non-default on the
    // opentrack side (no port discovery needed).
    static constexpr unsigned short kDefaultPort = 4242;

    OpentrackReceiver();
    ~OpentrackReceiver();

    OpentrackReceiver(const OpentrackReceiver &) = delete;
    OpentrackReceiver &operator=(const OpentrackReceiver &) = delete;

    // Open the UDP socket. Idempotent: returns false if already
    // installed or if socket / bind fails. Logs the failure reason
    // via WALKERCORE_LOG_WARN so the user can fix a port-conflict
    // (e.g. two WalkerCore plugins or a stale opentrack instance).
    bool Install(unsigned short port = kDefaultPort);

    // Close the socket. Idempotent.
    void Uninstall();

    bool isInstalled() const { return socket_ != 0; }

    // Drain the socket. Reads the most recent datagram (a single
    // recvfrom call with a 1ms timeout so we never block the
    // FrameLoop), converts yaw / pitch from degrees to radians,
    // and feeds the delta into the supplied HeadTrackerProvider as
    // a single Opentrack-source sample. The receiver is the
    // single owner of the previous-sample baseline; the head
    // tracker provider just sees a stream of deltas.
    //
    // `provider` may be nullptr (e.g. the M7 lander has not wired
    // it up yet); in that case Tick is a no-op apart from
    // draining the socket. The drain matters even without a
    // provider: an undrained socket buffers datagrams and the next
    // Tick reports a stale reading.
    void Tick(HeadTrackerProvider *provider);

    // Test-only reset hook so the unit test can drive multiple
    // installs in one process. Production code never calls this.
    static void ResetForTesting();

private:
    // Socket handle. 0 means "not installed". We use the raw
    // SOCKET type from <winsock2.h>; the header forward-declares
    // it as a `UINT_PTR` so we can avoid pulling winsock2.h into
    // this public header.
    void *socket_ = nullptr;
    unsigned short port_ = 0;
    bool has_prev_ = false;
    double prev_yaw_deg_ = 0.0;
    double prev_pitch_deg_ = 0.0;
};

}  // namespace walkercore

#endif  // WALKERCORE_OPENTRACK_RECEIVER_H
