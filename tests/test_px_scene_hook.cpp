// WalkerCore — PxSceneHook unit tests.
//
// The hook only fires when the live game's addActor is called, so
// the tests cover the capture / readback contract via the test
// seam `ObserveAddActorForTesting` instead of trying to drive a
// real MinHook trampoline. The test seam mimics the exact path
// the trampoline would take on a real addActor entry: a single
// RCX (this_ptr) -> atomic compare-exchange -> call counter bump.
//
// We also exercise the catalog VA + RVA constants so the wire-up
// to amtrucks.exe stays correct: kAddActorVa - kAmtrucksImageBase
// is the offset that the Install path uses to locate the target
// inside the loaded module. A wrong offset would still install
// the hook, but the trampoline would land in the middle of some
// unrelated function and crash on the first call.
//
// Each test starts from a clean slate via `ResetForTesting` so
// running tests in sequence does not leak state.

#include "TestHarness.h"

#include "walkercore/PxSceneHook.h"

#include <memory>

namespace walkercore {

namespace {

void Reset() { PxSceneHook::ResetForTesting(); }

}  // namespace

void TestPxSceneHookCatalogConstantsMatchAmtrucksLayout() {
    // Image base 0x140000000; PxScene::addActor at VA 0x141AE0B62.
    // The offset is what the Install path uses to locate the
    // target inside the loaded module.
    EXPECT_EQ(static_cast<uint64_t>(PxSceneHook::kAmtrucksImageBase),
              0x140000000ULL);
    EXPECT_EQ(static_cast<uint64_t>(PxSceneHook::kAddActorVa),
              0x141AE0B62ULL);
    EXPECT_EQ(
        static_cast<uint64_t>(PxSceneHook::kAddActorVa -
                              PxSceneHook::kAmtrucksImageBase),
        0x1AE0B62ULL);
}

void TestPxSceneHookFreshInstanceIsZero() {
    Reset();
    PxSceneHook h;
    EXPECT_EQ(h.captured_instance(), 0ULL);
    EXPECT_EQ(h.add_actor_calls(), 0);
    EXPECT_FALSE(h.target_reachable());
}

void TestPxSceneHookObserveStoresFirstCapture() {
    Reset();
    PxSceneHook h;
    PxSceneHook::ObserveAddActorForTesting(0x140000000ULL);
    EXPECT_EQ(h.captured_instance(), 0x140000000ULL);
    EXPECT_EQ(h.add_actor_calls(), 1);
}

void TestPxSceneHookObserveKeepsFirstCaptureOnLaterNonZeroCalls() {
    // First non-zero capture wins; subsequent non-zero calls do
    // not overwrite the captured value (matches the live
    // trampoline's compare_exchange_strong gate).
    Reset();
    PxSceneHook h;
    PxSceneHook::ObserveAddActorForTesting(0x140000000ULL);
    PxSceneHook::ObserveAddActorForTesting(0x150000000ULL);
    EXPECT_EQ(h.captured_instance(), 0x140000000ULL);
    EXPECT_EQ(h.add_actor_calls(), 2);
}

void TestPxSceneHookObserveIgnoresZero() {
    // The trampoline never overwrites a captured pointer with
    // zero — but it does ignore zero-calls entirely. The test
    // seam mirrors that contract.
    Reset();
    PxSceneHook h;
    PxSceneHook::ObserveAddActorForTesting(0);
    EXPECT_EQ(h.captured_instance(), 0ULL);
    EXPECT_EQ(h.add_actor_calls(), 0);
}

void TestPxSceneHookResetForTestingClearsState() {
    PxSceneHook h;
    PxSceneHook::ObserveAddActorForTesting(0x140000000ULL);
    EXPECT_EQ(h.add_actor_calls(), 1);
    Reset();
    EXPECT_EQ(h.captured_instance(), 0ULL);
    EXPECT_EQ(h.add_actor_calls(), 0);
}

void TestPxSceneHookInstallWithoutAmtrucksLeavesTargetUnreachable() {
    // When amtrucks.exe is not in this process (every unit test
    // environment, every debug run without the game), the Install
    // path returns true (ABI surface installed) but the target
    // is not reachable. The hook never fires; the captured value
    // stays at 0.
    Reset();
    PxSceneHook h;
    const bool ok = h.Install();
    EXPECT_FALSE(ok);
    EXPECT_FALSE(h.target_reachable());
    EXPECT_EQ(h.captured_instance(), 0ULL);
    h.Uninstall();
}

void TestPxSceneHookInstallUninstallRoundtripIsIdempotent() {
    Reset();
    PxSceneHook h;
    EXPECT_FALSE(h.isInstalled());
    h.Install();
    EXPECT_TRUE(h.isInstalled());
    h.Install();  // idempotent — second call must not crash and
                  // must leave state unchanged.
    EXPECT_TRUE(h.isInstalled());
    h.Uninstall();
    EXPECT_FALSE(h.isInstalled());
    h.Uninstall();  // second uninstall is a safe no-op.
    EXPECT_FALSE(h.isInstalled());
}

}  // namespace walkercore