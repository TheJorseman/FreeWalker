// WalkerCore — background per-frame ticker.
//
// M2 fallback lander. The D3D11/D3D12 Present hooks were the primary
// mechanism for invoking walker_per_frame_tick, but those hooks only fire
// when the game calls CreateDXGIFactory1 / D3D11CreateDeviceAndSwapChain
// AFTER our DLL is loaded. SCS plugins load *after* the game's graphics
// subsystem has already created the DXGI factory + swap chain, so the
// trampoline never fires and walker_per_frame_tick never runs — leaving
// every input-driven subsystem (FSM transitions, fuel pump, flashlight,
// coupling, settings) frozen.
//
// This header declares the fallback:
//
//   * `PerFrameThread` runs a dedicated worker thread that calls
//     walker_per_frame_tick at ~60 Hz using std::chrono::steady_clock.
//   * The thread starts during scs_telemetry_init (after every other
//     subsystem has been wired) and stops during scs_telemetry_shutdown
//     (before the providers get torn down).
//   * A small CAS-based lock (`walker_try_enter_per_frame_tick` /
//     `walker_exit_per_frame_tick`) lets the Present hooks and the
//     background thread coexist: whichever one gets the lock first runs
//     the tick; the other skips this frame. Once a Present hook fires,
//     the background thread still wakes up every 16.67 ms, sees the lock
//     held, and goes back to sleep — no double-tick cost.
//
// Thread-safety:
//   - walker_per_frame_tick reads global state (FrameLoop, FSM, telemetry
//     snapshots) that the SCS callbacks also write to. SCS callbacks fire
//     on the game thread; both the background thread and the D3D Present
//     hooks run on whichever thread the OS hands the call to. Today every
//     reader/writer is single-threaded because (a) the SCS callbacks
//     arrive on the game thread (where the D3D Present hook also lives in
//     games that actually have the swap chain captured), and (b) the
//     background ticker is the only asynchronous reader. The lock above is
//     the seam that prevents both from running concurrently. Future
//     multi-reader code paths should keep using `WalkerCoreTickGuard`
//     helpers, not the raw CAS.
//
// Lifecycle (the caller owns the singleton):
//
//     walkercore::PerFrameThread g_ticker;
//     g_ticker.Start();   // scs_telemetry_init
//     g_ticker.Stop();    // scs_telemetry_shutdown

#ifndef WALKERCORE_PER_FRAME_THREAD_H
#define WALKERCORE_PER_FRAME_THREAD_H

#include <atomic>
#include <cstdint>
#include <thread>

#ifdef __cplusplus
extern "C" {
#endif

// CAS-based critical section around walker_per_frame_tick. Returns 1 if
// the caller acquired the lock and should run the tick; 0 if another
// thread is already inside the tick this frame.
//
// The two functions form a single critical region; do not call
// walker_per_frame_tick without first checking this and without releasing
// the lock on the way out.
int walker_try_enter_per_frame_tick(void);
void walker_exit_per_frame_tick(void);

#ifdef __cplusplus
}  // extern "C"
#endif

namespace walkercore {

class PerFrameThread {
public:
    PerFrameThread();
    ~PerFrameThread();

    PerFrameThread(const PerFrameThread &) = delete;
    PerFrameThread &operator=(const PerFrameThread &) = delete;

    // Spawn the worker thread. Idempotent: returns false (and does not
    // start a second thread) if the ticker is already running.
    bool Start();

    // Signal the worker to stop and join the thread. Idempotent: returns
    // immediately if the ticker was not running. Safe to call from a
    // destructor.
    void Stop();

    // True while the worker thread is alive (between successful Start
    // and completed Stop).
    bool IsRunning() const;

    // Number of times the worker thread has invoked walker_per_frame_tick.
    // Useful for tests and for the diagnostic dump. Monotonically
    // increasing; the value is approximate under contention.
    uint64_t Ticks() const;

private:
    // Worker thread body. Lives until the alive_ flag flips to false.
    void Run();

    std::atomic<bool> alive_{false};
    std::atomic<uint64_t> ticks_{0};
    std::thread thread_;
};

}  // namespace walkercore

#endif  // WALKERCORE_PER_FRAME_THREAD_H
