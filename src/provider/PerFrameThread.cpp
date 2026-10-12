// WalkerCore — background per-frame ticker (M2 fallback lander).
//
// Implementation notes:
//
//   1. The ticker drives walker_per_frame_tick at a steady ~60 Hz using
//      std::chrono::steady_clock + sleep_until. The first call passes
//      dt=0.0 (matches the existing "no baseline" contract on the D3D
//      Present path: last_us == 0 ⇒ dt = 0.0).
//   2. We catch up if the worker falls behind (>100 ms behind) by
//      resetting `next` to "now". Without that, a long stall (debug break,
//      system suspend) would deliver a tight burst of stale ticks on
//      resume.
//
//   3. The walker_try_enter_per_frame_tick / walker_exit_per_frame_tick
//      CAS lock is the rendezvous with the D3D Present hooks. When the
//      render path captures a swap chain and starts ticking from Present,
//      the CAS makes the background ticker a no-op for that frame; when
//      the render path is dormant (current ATS reality), every frame
//      belongs to the background ticker.
//
//   4. We keep the lock acquire/release symmetric across the extern "C"
//      entry points so a future caller (e.g. an external driver) can
//      guard a manually-driven tick without bypassing the existing
//      callers. The functions are 1-byte atomic CASes on x86-64, so the
//      per-frame cost is two locked instructions.

#include "walkercore/PerFrameThread.h"

#include <chrono>

#include "walkercore/Log.h"
#include "walkercore/Time.h"

#ifndef WALKERCORE_TEST_NO_PER_FRAME_TICK
extern "C" void walker_per_frame_tick(double dt_seconds);
#endif

namespace {

// File-local lock used by walker_try_enter_per_frame_tick /
// walker_exit_per_frame_tick. Single global so every tick site in the
// process agrees on whether the conductor is busy.
std::atomic<bool> g_tick_in_progress{false};

constexpr auto kTargetPeriod = std::chrono::microseconds(16667);  // ~60 Hz
constexpr auto kCatchUpThreshold = std::chrono::milliseconds(100);

}  // namespace

extern "C" int walker_try_enter_per_frame_tick(void) {
    bool expected = false;
    return g_tick_in_progress.compare_exchange_strong(
               expected, true,
               std::memory_order_acquire,
               std::memory_order_relaxed)
               ? 1
               : 0;
}

extern "C" void walker_exit_per_frame_tick(void) {
    g_tick_in_progress.store(false, std::memory_order_release);
}

namespace walkercore {

PerFrameThread::PerFrameThread() = default;

PerFrameThread::~PerFrameThread() {
    Stop();
}

bool PerFrameThread::Start() {
    bool expected = false;
    if (!alive_.compare_exchange_strong(
                expected, true,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
        return false;  // already running
    }
    thread_ = std::thread([this] { Run(); });
    return true;
}

void PerFrameThread::Stop() {
    bool expected = true;
    if (!alive_.compare_exchange_strong(
                expected, false,
                std::memory_order_release,
                std::memory_order_relaxed)) {
        return;  // already stopped
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool PerFrameThread::IsRunning() const {
    return alive_.load(std::memory_order_acquire);
}

uint64_t PerFrameThread::Ticks() const {
    return ticks_.load(std::memory_order_relaxed);
}

void PerFrameThread::Run() {
    WALKERCORE_LOG_INFO(
        "PerFrameThread: ticker started (~60 Hz; takes over from "
        "D3D Present hooks when the game's swap chain was already "
        "created before WalkerCore loaded)");

    using clock = std::chrono::steady_clock;
    auto next = clock::now() + kTargetPeriod;
    uint64_t last_us = walkercore::NowMicros();

    while (alive_.load(std::memory_order_acquire)) {
        if (walker_try_enter_per_frame_tick() != 0) {
            const uint64_t now_us = walkercore::NowMicros();
            const double dt =
                (last_us == 0)
                    ? 0.0
                    : static_cast<double>(now_us - last_us) / 1e6;
            last_us = now_us;
#ifndef WALKERCORE_TEST_NO_PER_FRAME_TICK
            walker_per_frame_tick(dt);
#endif
            ticks_.fetch_add(1, std::memory_order_relaxed);
            walker_exit_per_frame_tick();
        }

        std::this_thread::sleep_until(next);
        next += kTargetPeriod;

        // Catch-up: if we fell behind by more than 100 ms (debugger
        // break, OS suspend), don't try to deliver a tight burst of
        // stale ticks. Reset `next` to "now + period" so we resume
        // normal cadence from the present moment.
        if (next < clock::now() - kCatchUpThreshold) {
            next = clock::now() + kTargetPeriod;
        }
    }

    WALKERCORE_LOG_INFO("PerFrameThread: ticker stopped");
}

}  // namespace walkercore
