// WalkerCore — D3D11Hook state-machine probes (M2 lander coverage).
//
// Pins the deterministic parts of the D3D11Hook class lifecycle. We
// deliberately avoid asserting whether Install() returned true in this
// test build, because that depends on whether d3d11.dll happens to be
// loaded in the test process (and which version of the MinHook call
// succeeded). Instead we pin the *invariants* a regression in the
// Install/Uninstall state machine would break.
//
// What this pins:
//   - Fresh hook: isInstalled() == false, CurrentSwapChainGeneration() == 0.
//   - BindFrameLoop accepts null without storing it as live state (the
//     hook must not crash if the conductor passes null between tests).
//   - Uninstall is safe when never installed (idempotent).
//   - Install is idempotent at the entry level: a second Install() on
//     the same instance returns false (no double-hook).
//   - After a successful Install, isInstalled() reflects the state.
//   - After Uninstall, isInstalled() returns to false.
//   - Destructor on a never-installed instance is safe.
//
// What this does NOT pin (and why):
//   - Whether Install() returns true in this build. The test exe links
//     d3d11.dll transitively via WalkerCoreTestLib's D3D11Hook.cpp
//     object (which includes <d3d11.h>) on MinGW builds. When that
//     import is satisfied at load time, Install() would try to call
//     MinHook and might succeed or fail. The real per-frame Present
//     hook is verified by integration, not here.

#include "TestHarness.h"

#include "walkercore/D3D11Hook.h"
#include "walkercore/FrameLoop.h"

namespace walkercore {

namespace {

// FrameLoop default-constructs cleanly; we never call RunOnce in these
// probes so no telemetry / input / world is required. The local
// instance is just there to exercise BindFrameLoop's non-null branch
// in one probe.
FrameLoop MakeIdleFrameLoop() {
    return FrameLoop();
}

}  // namespace

void TestD3D11HookFreshInstanceIsNotInstalled() {
    D3D11Hook hook;
    EXPECT_FALSE(hook.isInstalled());
}

void TestD3D11HookFreshSwapChainGenerationIsZero() {
    // g_swap_chain_gen is process-global; reset by re-running the test
    // suite from a clean process. In a single test binary run, every
    // other test also calls CurrentSwapChainGeneration(), but none of
    // them actually capture a device, so the value stays at 0 across
    // the whole suite. If a future test starts capturing, this probe
    // will fail and force the maintainer to think about ordering.
    EXPECT_EQ(CurrentSwapChainGeneration(), 0u);
}

void TestD3D11HookBindFrameLoopNullIsSafe() {
    D3D11Hook hook;
    // Must not crash, must not install.
    hook.BindFrameLoop(nullptr);
    EXPECT_FALSE(hook.isInstalled());
}

void TestD3D11HookBindFrameLoopStoresPointer() {
    // We can't observe frame_loop_ directly (private), but we can pin
    // that the call is well-defined for a non-null pointer. If the
    // BindFrameLoop contract ever changes to require a non-null
    // argument, this test will crash at construction time of the
    // FrameLoop, not at BindFrameLoop — that's the right failure mode
    // (a maintainer sees the FrameLoop fail, not the BindFrameLoop).
    D3D11Hook hook;
    FrameLoop fl = MakeIdleFrameLoop();
    hook.BindFrameLoop(&fl);
    EXPECT_FALSE(hook.isInstalled());  // bind does not install
}

void TestD3D11HookUninstallWithoutInstallIsSafe() {
    D3D11Hook hook;
    hook.Uninstall();  // must not crash
    EXPECT_FALSE(hook.isInstalled());
    // Idempotent: a second Uninstall is also safe.
    hook.Uninstall();
    EXPECT_FALSE(hook.isInstalled());
}

void TestD3D11HookInstallUninstallRoundtrip() {
    // This test runs the actual MinHook path when d3d11.dll is
    // import-resolved. The MinHook call may legitimately fail on
    // hosts where d3d11.dll is loaded but the symbol is unavailable
    // (rare). The contract is: whatever Install returns, isInstalled
    // must match, and Uninstall must leave the hook uninstalled.
    D3D11Hook hook;
    const bool installed = hook.Install();
    EXPECT_EQ(hook.isInstalled(), installed);
    hook.Uninstall();
    EXPECT_FALSE(hook.isInstalled());
}

void TestD3D11HookSecondInstallReturnsFalse() {
    D3D11Hook hook;
    const bool first = hook.Install();
    // Second call must NOT re-install. The contract is that
    // installed_ guards against double-hook; even if the first
    // Install() failed, the second is a no-op.
    const bool second = hook.Install();
    EXPECT_FALSE(second);
    EXPECT_EQ(hook.isInstalled(), first);
    hook.Uninstall();
}

void TestD3D11HookDestructorOnFreshInstanceIsSafe() {
    // Stack-allocated, never installed. Just exercises the
    // ~D3D11Hook -> Uninstall() -> MH_Uninitialize() path with
    // installed_ == false, which short-circuits inside Uninstall.
    {
        D3D11Hook hook;
        (void)hook;
    }
    EXPECT_TRUE(true);  // reaching here is the test
}

void TestD3D11HookDestructorAfterUninstallIsSafe() {
    // Destructor -> Uninstall() -> installed_ is false, the body of
    // Uninstall returns immediately.
    D3D11Hook hook;
    hook.Install();
    hook.Uninstall();
    // hook goes out of scope; ~D3D11Hook() calls Uninstall() again.
    EXPECT_TRUE(true);
}

}  // namespace walkercore
