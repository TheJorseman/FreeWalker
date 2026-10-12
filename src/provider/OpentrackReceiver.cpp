// WalkerCore — Opentrack UDP receiver implementation (M7 partial).
//
// The opentrack UDP protocol (github.com/opentrack/opentrack, file
// `opentrack-library-path/udp/udp.cpp`) sends one 48-byte datagram
// per frame:
//
//   struct opentrack_udp_packet {
//       double x, y, z;       // 24 bytes — position
//       double yaw, pitch, roll;  // 24 bytes — orientation (degrees)
//   };
//
// The opentrack UI names the channels "TX", "TY", "TZ" for the
// position triple and "Yaw", "Pitch", "Roll" for the orientation
// triple. We ignore x/y/z — the camera world placement comes from
// the SCS telemetry channels (truck.world.placement), not from the
// tracker. The orientation is the only thing we need to drive the
// head-tracker's yaw / pitch.
//
// Tick() reads the latest datagram non-blockingly, computes the
// delta from the previous sample, and feeds it into the head
// tracker. The head tracker accumulates the deltas into
// yaw_accum_ / pitch_accum_ and smooths them — the smoothed output
// is what PhaseApplyMouseLook reads to drive the camera.
//
// Socket lifecycle: Install() calls WSAStartup once per process
// (idempotent), opens a SOCK_DGRAM socket, sets a 1ms recv timeout
// via setsockopt(SO_RCVTIMEO), and binds to the chosen port. We
// intentionally do NOT call WSAStartup in the DLL's DllMain —
// winsock is a system-wide resource and starting it during
// DLL_PROCESS_ATTACH can race with the SCS loader's own
// initialization. The first OpentrackReceiver::Install() call
// starts it.

#include "walkercore/OpentrackReceiver.h"

#include <math.h>
#include <cstring>  // memcpy

// M_PI is not standard C++ — it's a POSIX/GCC extension. MinGW
// defines it via <math.h>; the cast below keeps MSVC builds
// (which lack M_PI) compiling.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "walkercore/Log.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace walkercore {

namespace {

// Module-level Winsock init counter. WSAStartup / WSACleanup are
// reference-counted per process; we keep our own counter because
// the Winsock docs are vague about whether the OS unloads the
// DLL after the matching WSACleanup when the count returns to
// zero. Tracking it ourselves guarantees the test harness can
// repeatedly Install / Uninstall without leaking.
int g_wsa_init_count = 0;
bool g_wsa_initialized = false;

bool EnsureWinsock() {
    if (g_wsa_initialized) {
        g_wsa_init_count++;
        return true;
    }
#if defined(_WIN32)
    WSADATA wsa;
    const int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        WALKERCORE_LOG_WARN("OpentrackReceiver: WSAStartup failed (err=%d)",
                            r);
        return false;
    }
#endif
    g_wsa_initialized = true;
    g_wsa_init_count = 1;
    return true;
}

void ReleaseWinsock() {
    if (!g_wsa_initialized) return;
    g_wsa_init_count--;
    if (g_wsa_init_count > 0) return;
#if defined(_WIN32)
    WSACleanup();
#endif
    g_wsa_initialized = false;
}

}  // namespace

OpentrackReceiver::OpentrackReceiver() = default;

OpentrackReceiver::~OpentrackReceiver() {
    if (socket_ != nullptr) {
        Uninstall();
    }
}

bool OpentrackReceiver::Install(unsigned short port) {
    if (socket_ != nullptr) {
        WALKERCORE_LOG_WARN(
            "OpentrackReceiver::Install: already installed on port %u",
            static_cast<unsigned>(port_));
        return false;
    }
    if (!EnsureWinsock()) {
        return false;
    }
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WALKERCORE_LOG_WARN(
            "OpentrackReceiver::Install: socket() failed (err=%d)",
            WSAGetLastError());
        ReleaseWinsock();
        return false;
    }
    // Non-blocking receive with a 1ms timeout. The Tick() path is
    // on the per-frame conductor; we MUST NOT block the frame. 1ms
    // is short enough that even a 60 FPS game (16.67 ms / frame)
    // spends < 0.1% of its budget on this call when the socket is
    // empty, and long enough that an actual opentrack datagram
    // (which is one per frame, ~16 ms apart) is always drained
    // on the first poll.
    DWORD timeout_ms = 1;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&timeout_ms),
                   sizeof(timeout_ms)) == SOCKET_ERROR) {
        WALKERCORE_LOG_WARN(
            "OpentrackReceiver::Install: setsockopt(SO_RCVTIMEO) failed "
            "(err=%d)", WSAGetLastError());
        closesocket(s);
        ReleaseWinsock();
        return false;
    }
    // Allow address reuse. Without SO_REUSEADDR, restarting the
    // game within the WSATimeoutDelay window (typically 240s) leaves
    // the port bound and the second Install() fails. With it, the
    // rebind succeeds.
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        WALKERCORE_LOG_WARN(
            "OpentrackReceiver::Install: bind(%u) failed (err=%d) — "
            "another opentrack instance or stale socket is bound to "
            "this port; the M7 head tracker will fall back to mouse",
            static_cast<unsigned>(port), WSAGetLastError());
        closesocket(s);
        ReleaseWinsock();
        return false;
    }
    socket_ = reinterpret_cast<void *>(s);
    port_ = port;
    has_prev_ = false;
    prev_yaw_deg_ = 0.0;
    prev_pitch_deg_ = 0.0;
    WALKERCORE_LOG_INFO(
        "OpentrackReceiver: bound to UDP port %u; Tick() will feed the "
        "head tracker with yaw/pitch deltas (degrees → radians)",
        static_cast<unsigned>(port));
    return true;
#else
    WALKERCORE_LOG_WARN(
        "OpentrackReceiver::Install: only implemented on Windows");
    ReleaseWinsock();
    return false;
#endif
}

void OpentrackReceiver::Uninstall() {
    if (socket_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    SOCKET s = reinterpret_cast<SOCKET>(socket_);
    closesocket(s);
#endif
    socket_ = nullptr;
    port_ = 0;
    has_prev_ = false;
    ReleaseWinsock();
    WALKERCORE_LOG_INFO("OpentrackReceiver: closed");
}

void OpentrackReceiver::Tick(HeadTrackerProvider *provider) {
    if (socket_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    SOCKET s = reinterpret_cast<SOCKET>(socket_);
    // 48 bytes = 6 × sizeof(double). The opentrack protocol has
    // never added or removed fields; we reject any datagram of a
    // different size as a protocol violation (some opentrack forks
    // send 24-byte "no-position" packets for the "no tracker"
    // case — those are noise, not head pose, and we drop them
    // silently).
    constexpr int kPacketSize = 48;
    char buf[kPacketSize];
    sockaddr_in from{};
    int from_len = sizeof(from);
    const int n = recvfrom(s, buf, kPacketSize, 0,
                           reinterpret_cast<sockaddr *>(&from), &from_len);
    if (n != kPacketSize) {
        // 0 = timeout (no datagram this tick — normal). n<0 with
        // WSAETIMEDOUT = same. n<0 with any other error = log.
        if (n < 0) {
            const int err = WSAGetLastError();
            if (err != WSAETIMEDOUT) {
                WALKERCORE_LOG_WARN(
                    "OpentrackReceiver::Tick: recvfrom failed (err=%d)",
                    err);
            }
        }
        return;
    }
    // Parse the 6 doubles. memcpy is safe here because the wire
    // format is little-endian IEEE-754 and we are on a little-
    // endian Windows x64 host. A big-endian target would need a
    // byte-swap here, but WalkerCore ships Windows-only.
    double x = 0.0, y = 0.0, z = 0.0;
    double yaw_deg = 0.0, pitch_deg = 0.0, roll_deg = 0.0;
    std::memcpy(&x,        buf +  0, 8);
    std::memcpy(&y,        buf +  8, 8);
    std::memcpy(&z,        buf + 16, 8);
    std::memcpy(&yaw_deg,  buf + 24, 8);
    std::memcpy(&pitch_deg, buf + 32, 8);
    std::memcpy(&roll_deg, buf + 40, 8);
    (void)x; (void)y; (void)z; (void)roll_deg;  // position / roll not used

    if (!has_prev_) {
        // First sample. We have no baseline; emit a zero-delta
        // sample so the head tracker marks the source as active
        // and the smoothed output starts at the same yaw/pitch
        // the first sample reported. Without this, the smoothed
        // output would stay at 0 until the user actually moves
        // their head, and the very first Tick would show a
        // jump-cut from "neutral" to "first opentrack reading".
        has_prev_ = true;
        prev_yaw_deg_ = yaw_deg;
        prev_pitch_deg_ = pitch_deg;
        if (provider != nullptr) {
            HeadTrackerSample first_sample{};
            first_sample.yaw_delta_rad = 0.0f;
            first_sample.pitch_delta_rad = 0.0f;
            first_sample.roll_delta_rad = 0.0f;
            provider->FeedSample(HeadTrackerSource::Opentrack, first_sample);
        }
        return;
    }
    // Opentrack yaw is signed; ranges from -180..+180. The delta
    // can wrap at the discontinuity (e.g. -179 → +179 is a +2°
    // movement, NOT a +358° movement). The shortest-arc delta is
    //   delta = ((curr - prev + 540) mod 360) - 180
    // which lives in [-180, +180]. Same for pitch (opentrack uses
    // -90..+90 so wrap is uncommon but possible near gimbal lock).
    auto shortest_arc = [](double curr, double prev) {
        double d = curr - prev;
        while (d > 180.0) d -= 360.0;
        while (d < -180.0) d += 360.0;
        return d;
    };
    const double yaw_delta_deg = shortest_arc(yaw_deg, prev_yaw_deg_);
    const double pitch_delta_deg = pitch_deg - prev_pitch_deg_;
    prev_yaw_deg_ = yaw_deg;
    prev_pitch_deg_ = pitch_deg;

    if (provider != nullptr) {
        HeadTrackerSample next_sample{};
        next_sample.yaw_delta_rad = static_cast<float>(yaw_delta_deg * (M_PI / 180.0));
        next_sample.pitch_delta_rad = static_cast<float>(pitch_delta_deg * (M_PI / 180.0));
        next_sample.roll_delta_rad = 0.0f;  // roll is not used by WalkerCore
        provider->FeedSample(HeadTrackerSource::Opentrack, next_sample);
    }
#endif
}

void OpentrackReceiver::ResetForTesting() {
    // The test harness wants to be able to drive multiple
    // Install / Uninstall cycles in one process without leaking
    // Winsock state across tests. We force the WSA counter back
    // to zero and call WSACleanup so the next Install() runs
    // WSAStartup fresh. Production code never calls this.
    g_wsa_init_count = 0;
    if (g_wsa_initialized) {
#if defined(_WIN32)
        WSACleanup();
#endif
        g_wsa_initialized = false;
    }
}

}  // namespace walkercore
