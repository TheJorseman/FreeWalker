// WalkerCore — Tiny time utility.
//
// Domain-level clocks. We don't want FrameLoop to call QueryPerformanceCounter
// directly because (a) it makes the math harder to unit-test and (b) the
// D3D11 hook already has its own per-frame time. This header gives the
// domain a single source of truth: "microseconds since session start" and
// "delta seconds between two ticks".
//
// Resolution: QueryPerformanceCounter on Windows gives us < 100 ns ticks,
// which is fine for the per-frame integration. We use 64-bit integer
// microseconds as the storage type; double seconds is what consumers see.

#ifndef WALKERCORE_TIME_H
#define WALKERCORE_TIME_H

#include <stdint.h>

namespace walkercore {

// Returns microseconds since some unspecified monotonic origin (typically
// system boot). Resolution is high enough for per-frame integration.
// Tests can stub the clock by replacing this function in a single .cpp.
uint64_t NowMicros();

// Returns seconds between `now_us` and `then_us`. Negative if `now_us`
// is older than `then_us` (which would be a clock anomaly; callers are
// expected to clamp to 0).
inline double DeltaSeconds(uint64_t now_us, uint64_t then_us) {
    if (now_us <= then_us) return 0.0;
    return static_cast<double>(now_us - then_us) / 1e6;
}

}  // namespace walkercore

#endif  // WALKERCORE_TIME_H