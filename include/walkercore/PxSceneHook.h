// WalkerCore — PxScene runtime hook (M3.5 lander).
//
// Why this exists:
//
//   The M3 lander (PhysXAdapter) was supposed to locate the game's
//   PxScene instance by reading the vtable out of `.rdata`/`.data`,
//   but the on-disk scan returns 0 candidates (see
//   `REA/scripts/physx-vta-scan/find_pxscene_instances.py`). The
//   instance is allocated at runtime on the heap by
//   `PxScene::create(...)` and held in a register / local variable
//   inside the game; we cannot statically find it.
//
//   This provider closes the gap with the smallest viable runtime
//   hook: install MinHook on the `PxScene::addActor` function body
//   at VA `0x141AE0B62` (the catalog slot-11 entry for ATS
//   1.61.3.1s). When the game calls `addActor`, the x64 calling
//   convention places `this` (the PxScene instance pointer) in
//   `RCX`; we capture RCX in the trampoline and save it to a
//   process-global. The first non-zero capture wins; subsequent
//   captures are logged only when the pointer changes.
//
//   With the captured pointer, downstream M3 work (collision
//   queries, knockout direction, the M3 long-term stub) can ask
//   "where is the live PxScene?" without doing further RE.
//
// ABI:
//   walker_pxscene_supported()
//   walker_pxscene_instance()
//   walker_dump_pxscene()
//
// The hook only ever reads `RCX`; it never mutates game state and
// never blocks. The original `addActor` is invoked via MinHook's
// trampoline exactly once per call.

#ifndef WALKERCORE_PX_SCENE_HOOK_H
#define WALKERCORE_PX_SCENE_HOOK_H

#include <cstdint>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace walkercore {

class PxSceneHook {
public:
    PxSceneHook();
    ~PxSceneHook();

    PxSceneHook(const PxSceneHook &) = delete;
    PxSceneHook &operator=(const PxSceneHook &) = delete;

    // Attempt to install the MinHook trampoline at `addActor` VA
    // `0x141AE0B62` of the live `amtrucks.exe`. Returns true when
    // the hook is attached AND `target_reachable()` is true.
    // Idempotent — second call returns the cached `installed_`
    // value without re-attaching. Safe to call before the game
    // loads; in that case `target_reachable_` stays false and
    // `installed_` stays true (we still want the ABI surface
    // available so `walker_dump_pxscene` reports "not yet
    // reachable" instead of crashing).
    bool Install();
    void Uninstall();
    bool isInstalled() const { return installed_; }

    // The catalog VA we hook. Documented publicly so the dump
    // prints the same value the RE pass identified.
    static constexpr uint64_t kAddActorVa = 0x141AE0B62ULL;
    static constexpr uint64_t kAmtrucksImageBase = 0x140000000ULL;

    // Live captured PxScene instance pointer (this on addActor
    // entry). 0 when the hook has not yet observed a call. Updates
    // atomically from the hooked function.
    uint64_t captured_instance() const;
    int add_actor_calls() const;

    // True when the hook is attached to a reachable target (i.e.
    // amtrucks.exe was loaded at Install time AND MinHook accepted
    // the trampoline at `kAddActorVa`). The captured_instance may
    // still be 0 even when supported_=true: it just means the game
    // has not yet called addActor (rare; the player world loads
    // actors during init).
    bool target_reachable() const { return target_reachable_; }

    // -- Test seams ---------------------------------------------------------
    // Pure helpers exposed for the unit-test suite. They are not part
    // of the runtime ABI and clients should never call them.
    //
    // `ObserveAddActorForTesting(this_ptr)` simulates the entry of
    // the hooked function — it stores `this_ptr` as the captured
    // instance and bumps the call counter, exactly as the real
    // trampoline would on a real addActor call. Used by tests to
    // exercise the capture / readback contract without MinHook.
    static void ObserveAddActorForTesting(uint64_t this_ptr);

    // `ResetForTesting()` zeroes the captured pointer, the call
    // counter, and the target-reachable flag. Used by tests to
    // start from a clean slate.
    static void ResetForTesting();

private:
    bool installed_ = false;
    bool target_reachable_ = false;
    std::string install_failure_;
};

}  // namespace walkercore

#endif  // WALKERCORE_PX_SCENE_HOOK_H