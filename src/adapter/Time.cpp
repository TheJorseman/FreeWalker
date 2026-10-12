// WalkerCore — Time implementation.

#include "walkercore/Time.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace walkercore {

uint64_t NowMicros() {
#if defined(_WIN32)
    // QueryPerformanceCounter is the standard monotonic clock on Windows.
    // Frequency is per-machine but constant per boot; we read it once and
    // cache the value in a function-local static (C++11 magic statics are
    // thread-safe by construction).
    static const double ticks_to_us = []() -> double {
        LARGE_INTEGER freq{};
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
            // Unreachable on any modern Windows, but degrade safely to 1 µs
            // per tick so division never crashes. The user will see slow
            // motion, but the DLL will still load.
            return 1.0;
        }
        // ticks / ticks_per_second = seconds; * 1e6 = microseconds.
        return 1e6 / static_cast<double>(freq.QuadPart);
    }();

    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(static_cast<double>(t.QuadPart) * ticks_to_us);
#else
    // Non-Windows placeholder. We don't build anywhere else; this keeps
    // the symbol resolvable for cross-compile experiments.
    return 0;
#endif
}

}  // namespace walkercore