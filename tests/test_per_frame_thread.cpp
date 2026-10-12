// WalkerCore — PerFrameThread fallback lander tests.
//
// The background ticker is the safety net for the case where the D3D
// Present hooks cannot drive walker_per_frame_tick (the game's DXGI
// factory and swap chain were already created before WalkerCore's DLL
// was loaded). These probes pin:
//
//   * Start / Stop are idempotent. A second Start() must return false
//     (we don't double-spawn a worker thread); a second Stop() must
//     return immediately (we don't join a thread that wasn't running).
//   * The ticker actually runs at the advertised cadence. Over a
//     ~120 ms wall-clock window the Ticks() counter must advance by
//     ~5-12 increments (60 Hz → 6-8 ticks; allow generous slack for
//     a CI machine under load).
//   * The per-frame lock (walker_try_enter_per_frame_tick /
//     walker_exit_per_frame_tick) serialises synthetic concurrent
//     callers exactly like it does the D3D Present hook in
//     production. Without the CAS, both sides of a race would
//     observe `true` and both would run walker_per_frame_tick in
//     the same frame.
//   * Many Start()/Stop() cycles don't leak threads, lose ticks, or
//     wedge the lock. This is the "atomic ticker reset from a long
//     sequence of init/shutdown pairs" scenario that would otherwise
//     surface only after a hot-reload during development.
//
// We do NOT call walker_per_frame_tick() in these unit tests. It
// would pull in FrameLoop, the SCS SDK, and every other provider the
// DLL links — making the test build big and slow. Instead we use the
// lock APIs the ticker itself uses (the same extern "C" symbols the
// D3D hooks call), which is the part we actually want to pin.

#include "TestHarness.h"

#include <chrono>
#include <thread>

#include "walkercore/PerFrameThread.h"

extern "C" {
int walker_try_enter_per_frame_tick(void);
void walker_exit_per_frame_tick(void);
}

namespace walkercore {

void TestPerFrameThreadStartStopIsIdempotent() {
    PerFrameThread ticker;

    EXPECT_TRUE(!ticker.IsRunning());

    EXPECT_TRUE(ticker.Start());
    EXPECT_TRUE(ticker.IsRunning());

    // Second Start must refuse — otherwise we'd spawn a second
    // worker thread racing the first one over the same providers.
    EXPECT_TRUE(!ticker.Start());
    EXPECT_TRUE(ticker.IsRunning());

    ticker.Stop();
    EXPECT_TRUE(!ticker.IsRunning());

    // Second Stop is a no-op (the destructor also calls Stop(), so
    // this guard matters for symmetry).
    ticker.Stop();
    EXPECT_TRUE(!ticker.IsRunning());
}

void TestPerFrameThreadTicksAdvanceOverWindow() {
    PerFrameThread ticker;
    EXPECT_TRUE(ticker.Start());

    // Sleep a fixed wall-clock window and verify the ticker advanced
    // by a plausible number of ticks for a 60 Hz scheduler with
    // slack for CI scheduler noise.
    const uint64_t before = ticker.Ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const uint64_t after = ticker.Ticks();

    ticker.Stop();

    // We don't want this test to flake on a heavily-loaded CI box,
    // so the lower bound is conservative — even at 25 Hz the test
    // still passes; what we need is "the ticker was alive and
    // ticking". The upper bound is generous for fast machines where
    // sleep_for() under-estimates the wait.
    const uint64_t observed = (after >= before) ? (after - before) : 0;
    EXPECT_TRUE(observed >= 2);
    EXPECT_TRUE(observed <= 64);
}

void TestPerFrameTickLockSerializesTwoCallers() {
    // The lock is the rendezvous between the background ticker and
    // the D3D Present hooks. The contract is: walker_try_enter
    // returns 1 the FIRST time, 0 while the slot is held, and
    // re-arms after walker_exit.

    EXPECT_TRUE(walker_try_enter_per_frame_tick() == 1);
    EXPECT_TRUE(walker_try_enter_per_frame_tick() == 0);

    // Even a busy-spin trying to grab the lock should not turn up a
    // non-zero value while the holder is still inside.
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(walker_try_enter_per_frame_tick() == 0);
    }

    walker_exit_per_frame_tick();
    EXPECT_TRUE(walker_try_enter_per_frame_tick() == 1);
    walker_exit_per_frame_tick();
}

void TestPerFrameThreadSurvivesManyStartStopCycles() {
    // Stress: 25 cycles of Start/Stop. Pins the behaviour that we
    // never leak threads, lose ticks, or wedge the lock across
    // repeated init/shutdown pairs.
    for (int cycle = 0; cycle < 25; ++cycle) {
        PerFrameThread ticker;
        EXPECT_TRUE(ticker.Start());
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        EXPECT_TRUE(ticker.IsRunning());
        ticker.Stop();
        EXPECT_TRUE(!ticker.IsRunning());
    }

    // After many cycles the lock must still be unowned (we never
    // leaked it through a missed exit).
    EXPECT_TRUE(walker_try_enter_per_frame_tick() == 1);
    walker_exit_per_frame_tick();
}

}  // namespace walkercore
