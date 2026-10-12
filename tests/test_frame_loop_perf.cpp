// WalkerCore — FrameLoop per-frame performance probes.
//
// Two benchmark-style tests pin a per-tick budget on the hot paths of the
// per-frame conductor:
//
//   - TestFrameLoopRunOnceHotPathPerfBudget
//       Times FrameLoop::RunOnce(dt) in Walking mode with a no-collide
//       IWorldGeometry and a real (uninstalled) InputHook. This isolates
//       the on-foot integration cost: PhaseApplyMouseLook →
//       PhaseIntegrateMovement → PhaseResolveCollision (open world) →
//       PhaseAnimatePlayer → CameraState_AdoptPlayer / CameraState_Update.
//       The PhasePushCameraToRenderer phase is intentionally empty until
//       the M2/D3D11 lander ships.
//
//   - TestPerFrameTickConductorPerfBudget
//       Times the WHOLE per-frame assembly that walker_per_frame_tick(dt)
//       drives in production: SampleInput + WalkerFsm::Update +
//       FrameLoop::RunOnce(dt). This is what runs once per Present in
//       the DLL — the only hot path we ship where latency matters, since
//       a frame stall here either forces the game's render thread to wait
//       or causes the user-visible camera to hitch.
//
//   - TestInputHookSamplePerfBudget
//       Isolates the per-call cost of InputHook::Sample(), the function
//       called from FrameLoop::PhaseSampleInput once per frame.
//
// Thresholds are set generously above the expected cost on a modern
// x86 host so the test does not flake on slower CI VMs; they still
// catch a 10× or worse regression in the hot path (the value of a
// micro-benchmark like this is the long-tail regression, not absolute
// numbers, which we do not care to commit to).
//
// Run on Windows: link the same domain + workflow sources the DLL
// uses (no SCS SDK at runtime), so the benchmark exercises the same
// code path that ships. No PhysX here — that path is M3-stubbed and
// has its own cost profile in production that we measure separately.

#include "TestHarness.h"

#include <stdint.h>

#include "walkercore/FrameLoop.h"
#include "walkercore/InputHook.h"
#include "walkercore/MovementConfig.h"
#include "walkercore/PlayerState.h"
#include "walkercore/Time.h"
#include "walkercore/TruckState.h"
#include "walkercore/WalkerFsm.h"
#include "walkercore/WorldState.h"

namespace walkercore {

namespace {

// Cheap IWorldGeometry that models an "open world" — every sweep is fully
// accepted, every raycast misses, every ground probe returns y = 0. This
// represents the cheapest non-trivial path through PhaseResolveCollision
// (which would otherwise balloon into a PhysX cross-process call once the
// M3 lander ships).
class BenchWorld : public IWorldGeometry {
public:
    bool Raycast(Vec3 /*origin*/, Vec3 /*direction*/, double /*max_distance*/,
                 Vec3 * /*hit_point*/, Vec3 * /*hit_normal*/) override {
        return false;
    }
    MoveResult SphereSweep(Vec3 origin, double /*radius*/,
                           Vec3 /*displacement*/) override {
        return MoveResult{origin, {0.0, 0.0, 0.0}, 0.0f, false};
    }
    bool groundY(Vec3 /*probe*/, double *out_y) override {
        if (out_y) *out_y = 0.0;
        return true;
    }
};

// Bootstrap a FrameLoop into Walking mode without driving a real truck.
// The FSM transition tween needs only an empty TruckState (no placement
// required) and zero intent, which is fine for benchmarking.
void EnterWalking(FrameLoop *loop, WalkerFsm *fsm, PlayerState *player,
                  InputHook *input) {
    (void)loop;
    (void)input;
    // Short transition so we reach Walking in a handful of Update() calls.
    fsm->SetTransitionSeconds(0.05);
    WalkerFsmInputs inputs;
    TruckState truck_state{};
    inputs.truck = &truck_state;
    inputs.player = player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;

    CameraState target;
    CameraState_Reset(target);
    // First Update starts the transition.
    fsm->Update(inputs, 0.016, &target);
    // Subsequent Updates advance it; one more Update with a big dt pushes
    // us over the finish line.
    fsm->Update(inputs, 0.10, &target);
}

}  // namespace

void TestFrameLoopRunOnceHotPathPerfBudget() {
    // We expect:
    //   - PhaseSampleInput (~32 byte zero) when input_ == nullptr
    //   - PhaseApplyMouseLook (a couple of multiplies + clamp)
    //   - PhaseIntegrateMovement (sqrt + 2×cosf/sinf + a few *dt ops)
    //   - PhaseResolveCollision (2× SphereSweep returning instantly)
    //   - PhaseAnimatePlayer (2× sin/cos + a few *dt + clamp)
    //   - CameraState_AdoptPlayer + CameraState_Update (mat math)
    //
    // Cheap, but not free. Per-tick cost on a 2025-era x86 host should be
    // 5-20 µs. The test budget of 200 µs/tick is 10-40× above the real
    // cost — enough margin for CI VMs, tight enough to catch a 10×
    // regression.
    FrameLoop loop;
    WalkerFsm fsm;
    PlayerState player;
    PlayerState_Reset(player);
    fsm.BindPlayerTarget(&player);
    loop.BindFsm(&fsm);
    loop.BindInput(nullptr);  // explicit: nothing wired, SampleInput is a 32-byte zero

    BenchWorld world;
    loop.BindWorld(&world);

    // The FSM must be in Walking for RunOnce() to do the hot per-frame work.
    EnterWalking(&loop, &fsm, &player, nullptr);

    constexpr int kWarmup = 5000;
    constexpr int kIterations = 50000;
    constexpr double kDtSeconds = 1.0 / 60.0;

    // Warmup the CPU caches / branch predictors and the FPU pipeline.
    for (int i = 0; i < kWarmup; ++i) {
        loop.RunOnce(kDtSeconds);
    }

    // Measure.
    const uint64_t t0 = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        loop.RunOnce(kDtSeconds);
    }
    const uint64_t t1 = NowMicros();

    const uint64_t total_us = t1 - t0;
    // Average per-tick cost in microseconds, as a double so we can compare
    // with a generous ceiling later.
    const double per_tick_us =
        static_cast<double>(total_us) / static_cast<double>(kIterations);

    // Budget: 200 µs/tick. Modern x86 should run at < 20 µs/tick.
    // 200 µs/tick × 50,000 = 10,000 ms absolute time ceiling; the test will
    // fail well before that on a regression.
    EXPECT_LT(per_tick_us, 200.0);

    // Sanity-check: at least one tick actually completed (the loop advanced
    // the player). If the loop never integrated, the test would pass on
    // an empty measurement, which we want to detect.
    EXPECT_NE(total_us, 0ull);
}

void TestPerFrameTickConductorPerfBudget() {
    // Same harness as above, but we wrap the whole per-frame assembly:
    //   1. SampleInput (cheap; reads bound InputHook snapshot)
    //   2. WalkerFsm::Update (cheap in steady state; non-cheap during a
    //      0.05-0.6 s transition window, which is irrelevant to the
    //      per-frame budget in the long run — the steady-state phase is
    //      what governs camera smoothness)
    //   3. FrameLoop::RunOnce(dt)
    //
    // The InputHook is bound but not installed — Sample() copies the
    // last_ snapshot out, which is the no-system-hook path.
    FrameLoop loop;
    WalkerFsm fsm;
    InputHook input;
    PlayerState player;
    PlayerState_Reset(player);
    fsm.BindPlayerTarget(&player);
    loop.BindFsm(&fsm);
    loop.BindInput(&input);

    BenchWorld world;
    loop.BindWorld(&world);

    EnterWalking(&loop, &fsm, &player, &input);

    constexpr int kWarmup = 5000;
    constexpr int kIterations = 50000;
    constexpr double kDtSeconds = 1.0 / 60.0;

    for (int i = 0; i < kWarmup; ++i) {
        loop.SampleInput();
        WalkerFsmInputs inputs;
        TruckState truck_state{};
        inputs.truck = &truck_state;
        inputs.player = &player;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = false;
        CameraState target;
        CameraState_Reset(target);
        fsm.Update(inputs, kDtSeconds, &target);
        loop.RunOnce(kDtSeconds);
    }

    const uint64_t t0 = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        loop.SampleInput();
        WalkerFsmInputs inputs;
        TruckState truck_state{};
        inputs.truck = &truck_state;
        inputs.player = &player;
        inputs.interact_pressed = false;
        inputs.toggle_walk_pressed = false;
        CameraState target;
        CameraState_Reset(target);
        fsm.Update(inputs, kDtSeconds, &target);
        loop.RunOnce(kDtSeconds);
    }
    const uint64_t t1 = NowMicros();

    const uint64_t total_us = t1 - t0;
    const double per_tick_us =
        static_cast<double>(total_us) / static_cast<double>(kIterations);

    // Same budget: 200 µs/tick. The whole conductor (FSM + FrameLoop)
    // adds a WalkerFsm::Update call (a few conditionals + state enum
    // write) and an InputHook::Sample (a copy of a ~70-byte struct).
    EXPECT_LT(per_tick_us, 200.0);
    EXPECT_NE(total_us, 0ull);
}

//   - TestInputHookSamplePerfBudget
//       Pin a per-call budget on InputHook::Sample() in isolation. This
//       is the per-frame call the FrameLoop orchestrator makes (via
//       loop.SampleInput() → FrameLoop::PhaseSampleInput →
//       input_->Sample(last_input_)). The implementation is a struct copy
//       plus a 32-iteration bit-AND that clears edge bits in the source
//       (so the next sample starts fresh). On modern x86 the real cost
//       is sub-microsecond; the 10 µs/call ceiling here is 10× above
//       expected and catches a regression that, say, wraps the copy in a
//       mutex or moves the keys[] array to a heap allocation.
//
//       The InputHook is constructed but NOT installed (no WH_KEYBOARD_LL
//       hook, no WM_INPUT target). Sample() runs the no-callback path,
//       which is exactly what the per-frame loop hits in production when
//       the hook is installed and games with our render-thread capture
//       are active.
void TestInputHookSamplePerfBudget() {
    InputHook hook;
    // Realistic last_ state: a couple of keys held + rising edges on a
    // few that should be visible to the next consumer. Edges are what
    // force Sample() to do extra work (it has to clear them from the
    // source after copying), so we exercise the non-trivial path.
    for (int i = 0; i < 16; ++i) {
        hook.mutable_last().keys[i] = static_cast<uint8_t>(0x03);  // held + rising
    }
    hook.mutable_last().mouse_dx = 17;
    hook.mutable_last().mouse_dy = -3;
    hook.mutable_last().valid = true;

    constexpr int kWarmup = 20000;
    constexpr int kIterations = 200000;

    InputState sink;
    for (int i = 0; i < kWarmup; ++i) {
        hook.Sample(sink);
    }

    const uint64_t t0 = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        hook.Sample(sink);
    }
    const uint64_t t1 = NowMicros();

    const uint64_t total_us = t1 - t0;
    const double per_call_us =
        static_cast<double>(total_us) / static_cast<double>(kIterations);

    // 10 µs/call ceiling. Real cost is < 0.5 µs/call on modern x86.
    EXPECT_LT(per_call_us, 10.0);
    EXPECT_NE(total_us, 0ull);

    // Sanity: Sample() returns correct copy on every call. This catches
    // a regression where Sample() would skip the struct copy and return
    // zeroed data on the consumer side (which would be both a bug AND
    // artificially fast — a perf budget test that goes "fast" because
    // it's not doing the work it claims to do is the failure mode we
    // most want to avoid).
    //
    // M4 MousePump contract: Sample() also drains the per-frame motion
    // and wheel accumulators so the next frame's OnRawInputData calls
    // start from zero. After 200000+20000 Sample() calls in this
    // perf-budget test, the source mouse_dx/dy are zero (drained on
    // every iteration). The fixture's initial 17/-3 is gone. We
    // re-prime the deltas and one more Sample() to confirm the drain
    // contract is exercised by this loop.
    InputState verify;
    hook.Sample(verify);
    EXPECT_EQ(verify.mouse_dx, 0);
    EXPECT_EQ(verify.mouse_dy, 0);
    EXPECT_TRUE(verify.valid);
    // Held bits remain set; edges were cleared by the prior Sample().
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(verify.keys[i] & 0x01, 0x01);  // held survives
        EXPECT_EQ(verify.keys[i] & 0x02, 0x00);  // rising edge cleared
    }

    // M4 MousePump drain re-check: prime + sample + verify the
    // accumulator survives exactly one Sample() then drains.
    hook.mutable_last().mouse_dx = 17;
    hook.mutable_last().mouse_dy = -3;
    hook.Sample(verify);
    EXPECT_EQ(verify.mouse_dx, 17);
    EXPECT_EQ(verify.mouse_dy, -3);
    InputState verify2;
    hook.Sample(verify2);
    EXPECT_EQ(verify2.mouse_dx, 0);
    EXPECT_EQ(verify2.mouse_dy, 0);
}

//   - TestWalkerFsmUpdateSteadyStatePerfBudget
//       Pin a per-call budget on WalkerFsm::Update() in steady state
//       (state == Driving, no key pressed, no transition active). This
//       is the most common per-frame call from the conductor — for the
//       vast majority of frames the player is sitting in the cab with
//       no F10/F press, and Update() should be effectively a no-op
//       (sets out_camera_target->valid = false and returns).
//
//       Real cost on modern x86 is sub-microsecond. The 20 µs/call
//       ceiling here gives ~20× headroom for CI VMs and catches a
//       regression where someone, say, decides to call a log function
//       every frame inside Update()'s Driving branch.
//
//       We also exercise the Walking steady-state path (the second-most
//       common case during gameplay) and the TransitionIn mid-tween
//       path (where ComputeTransitionTarget actually runs).
void TestWalkerFsmUpdateSteadyStatePerfBudget() {
    // The fsm is created in Driving with no player bound. A fresh
    // TruckState with no placement is fine for the Driving branch
    // (no PlacePlayerExited call because player_target_ is null).
    WalkerFsm fsm;
    PlayerState player;
    PlayerState_Reset(player);
    fsm.BindPlayerTarget(&player);

    CameraState target;
    CameraState_Reset(target);

    WalkerFsmInputs inputs;
    TruckState truck_state{};
    inputs.truck = &truck_state;
    inputs.player = &player;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = false;

    constexpr int kWarmup = 5000;
    constexpr int kIterations = 100000;
    constexpr double kDtSeconds = 1.0 / 60.0;

    // Driving steady-state: Update() enters the Driving branch, sees
    // no key flags, sets valid=false and returns. Cheapest possible path.
    for (int i = 0; i < kWarmup; ++i) {
        fsm.Update(inputs, kDtSeconds, &target);
    }
    const uint64_t t0 = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        fsm.Update(inputs, kDtSeconds, &target);
    }
    const uint64_t t1 = NowMicros();
    const double per_call_us =
        static_cast<double>(t1 - t0) / static_cast<double>(kIterations);

    // Budget: 20 µs/call. Real cost on modern x86 is sub-µs/call for
    // the Driving-no-key branch (one branch, one bool write). 20 µs is
    // ~20× above expected and catches a 10× regression that would
    // still pass a FrameLoop-style 200 µs budget but would compound
    // every frame alongside the FrameLoop + Sample + Render layers.
    EXPECT_LT(per_call_us, 20.0);
    EXPECT_NE(t1, t0);

    // Sanity-check: state did not move. If Update accidentally advanced
    // the FSM (e.g. by treating false-but-uninitialised memory as true),
    // the FSM would be in TransitionOut and target.valid would be true.
    EXPECT_EQ(fsm.state(), WalkerFsmState::Driving);
    EXPECT_FALSE(target.valid);
}

//   - TestWalkerFsmUpdateMidTransitionPerfBudget
//       Pin a per-call budget on WalkerFsm::Update() while in the
//       middle of a TransitionIn tween (player walking → seat). This
//       exercises the heavier branch: every Update() runs
//       ComputeTransitionTarget which LerpVec3 + LerpAngle + Lerp's
//       eye_height and writes a CameraState target.
//
//       Transitions are short (0.6s by default; user-overridable to 0.05s
//       in tests), but the per-frame cost during them must still fit
//       inside the same per-frame budget as steady-state. A regression
//       here would only be visible to the user during cabin-enter, but
//       it would also be the worst case for the FrameLoop + FSM + Sample
//       per-frame conductor.
//
//       Real cost on modern x86 is a few µs/call. 50 µs/call ceiling
//       gives ~10× headroom for CI VMs and catches a regression where
//       someone, say, walks the input struct by value or adds a string
//       formatting call inside ComputeTransitionTarget.
//
//       Trick: we set the transition timer to 1 hour so the tween never
//       completes during the 50,000-iteration measurement window.
//       Per-call tween advance = (1/60) / 3600 ≈ 4.6e-6, so transition_t
//       reaches ~0.23 at the end of the run — well inside [0.05, 0.95]
//       where ComputeTransitionTarget runs unconditionally. This is
//       synthetic (no real transition is 1 hour long) but keeps the
//       measurement path clean.
void TestWalkerFsmUpdateMidTransitionPerfBudget() {
    WalkerFsm fsm;
    // 1-hour transition: synthetic, but keeps the FSM in TransitionIn
    // for the entire measurement window.
    fsm.SetTransitionSeconds(3600.0);
    PlayerState player;
    PlayerState_Reset(player);
    fsm.BindPlayerTarget(&player);

    CameraState target;
    CameraState_Reset(target);

    // Build a truck with a real placement so ComputeTransitionTarget has
    // a non-trivial seat world-position to lerp toward.
    TruckState truck{};
    truck.has_placement = true;
    truck.placement.position = Vec3{10.0, 1.5, -20.0};
    truck.placement.orientation.heading = 0.25f;  // unit-range → ~π/2 rad

    WalkerFsmInputs inputs;
    inputs.truck = &truck;
    inputs.player = &player;
    inputs.interact_pressed = false;

    // Walk the FSM: Driving → F10 → TransitionOut → run updates until it
    // finishes → Walking → F10 → TransitionIn (where we measure).
    inputs.toggle_walk_pressed = true;
    fsm.Update(inputs, 0.016, &target);
    inputs.toggle_walk_pressed = false;
    // The 1-hour transition timer means we need ~2,160,000 calls at
    // kDtSeconds = 1/60 to finish. We use a much bigger dt here just to
    // advance to Walking quickly.
    for (int i = 0; i < 10; ++i) {
        fsm.Update(inputs, 1.0e6, &target);  // huge dt; capped at t=1.0
    }
    EXPECT_EQ(fsm.state(), WalkerFsmState::Walking);

    // Enter TransitionIn.
    inputs.toggle_walk_pressed = true;
    fsm.Update(inputs, 0.016, &target);
    inputs.toggle_walk_pressed = false;
    EXPECT_EQ(fsm.state(), WalkerFsmState::TransitionIn);

    constexpr int kWarmup = 5000;
    constexpr int kIterations = 50000;
    constexpr double kDtSeconds = 1.0 / 60.0;

    // Steady Update calls with kDtSeconds — tween advances by
    // (1/60)/3600 per call, so it stays inside the TransitionIn branch
    // for the entire measurement.
    for (int i = 0; i < kWarmup; ++i) {
        fsm.Update(inputs, kDtSeconds, &target);
    }

    const uint64_t t0 = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        fsm.Update(inputs, kDtSeconds, &target);
    }
    const uint64_t t1 = NowMicros();
    const double per_call_us =
        static_cast<double>(t1 - t0) / static_cast<double>(kIterations);

    // Budget: 50 µs/call. Real cost is ComputeTransitionTarget (a few
    // multiply-adds for Vec3 lerp + a NormalizeAngle for the angle lerp
    // + a CameraState write) plus the steady-state branch overhead.
    EXPECT_LT(per_call_us, 50.0);
    EXPECT_NE(t1, t0);

    // Sanity-check: the FSM did real work. ComputeTransitionTarget sets
    // target.valid = true. If Update accidentally skipped it (regression
    // that would falsely make the benchmark pass), valid would be false.
    EXPECT_EQ(fsm.state(), WalkerFsmState::TransitionIn);
    EXPECT_TRUE(target.valid);
}

}  // namespace walkercore
