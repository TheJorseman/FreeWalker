// WalkerCore — process-wide MinHook lifecycle.
//
// MinHook's MH_Initialize / MH_Uninitialize is one-shot per process:
// calling MH_Initialize() twice returns MH_ERROR_ALREADY_INITIALIZED,
// and calling MH_Uninitialize() while another consumer still owns hooks
// disables those hooks too. WalkerCore has two hook providers (D3D11
// and D3D12); both can be installed by the same SCS plugin instance,
// so we need a shared refcount instead of each provider calling MH
// itself.
//
// Contract:
//   - MinHookInit() returns true once and every subsequent time it is
//     called (idempotent). First caller pays the MH_Initialize cost;
//     later callers just bump the refcount.
//   - MinHookShutdown() decrements the refcount; the LAST caller is
//     the one that actually disables all hooks and calls
//     MH_Uninitialize. Safe to call even if MinHookInit() was never
//     called (no-op).
//
// Header-only on purpose: there is no state that needs to live outside
// of this translation unit and no constructor/destructor order to
// reason about. Tests can link this in the same way they link the
// provider sources.
//
// When MinHook is not vendored (WALKERCORE_HAVE_MINHOOK is undefined)
// we still expose the same refcount API but both functions are no-ops
// returning true / decrementing to zero. That keeps the call sites in
// D3D11Hook / D3D12Hook identical regardless of how the build is
// configured.

#ifndef WALKERCORE_PROVIDER_MIN_HOOK_INIT_H
#define WALKERCORE_PROVIDER_MIN_HOOK_INIT_H

#include <atomic>
#include <mutex>

#if defined(WALKERCORE_HAVE_MINHOOK)
#include <MinHook.h>
#endif

namespace walkercore {

// Initialize the process-wide MinHook state. Idempotent. Returns true
// on the first call AND on every subsequent call; returns false only
// if the underlying MH_Initialize fails on a fresh process.
inline bool MinHookInit() {
    static std::atomic<int> refcount{0};
    static std::once_flag once;
    bool ok = true;
    std::call_once(once, [&ok]() {
#if defined(WALKERCORE_HAVE_MINHOOK)
        const MH_STATUS s = MH_Initialize();
        ok = (s == MH_OK);
        if (ok) {
            refcount.store(1, std::memory_order_release);
        }
#else
        // No MinHook in this build. Treat as always initialised so the
        // call sites are uniform.
        refcount.store(1, std::memory_order_release);
#endif
    });
    if (ok) {
        // Refcount goes 0 -> 1 on the first call_once fire; subsequent
        // calls just bump it.
        const int prev = refcount.fetch_add(1, std::memory_order_acq_rel);
        if (prev == 0) {
            // Race: another thread decremented between our call_once
            // fire and our fetch_add. We've already initialised; bump
            // is correct. Nothing else to do.
        }
    }
    return ok;
}

// Release one ref on the process-wide MinHook state. The last call
// disables all hooks and calls MH_Uninitialize. Safe to call when
// MinHookInit() was never called (no-op).
inline void MinHookShutdown() {
    static std::atomic<int> refcount{0};
#if defined(WALKERCORE_HAVE_MINHOOK)
    const int prev = refcount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 1) {
        // We're the last owner — tear MinHook down so re-Init works
        // cleanly later (uncommon during a single game session, but
        // tests re-init for Install/Uninstall roundtrips).
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        refcount.store(0, std::memory_order_release);
    }
#else
    // No MinHook in this build. Decrement and reset at zero so the
    // next Init starts clean.
    const int prev = refcount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 1) {
        refcount.store(0, std::memory_order_release);
    }
#endif
}

// Test-only hook: peek at the live refcount so unit tests can assert
// Idempotency / refcount-drop semantics without reaching into the
// internals. Not used by production code.
inline int MinHookRefCountForTesting() {
    static std::atomic<int> refcount{0};
    return refcount.load(std::memory_order_acquire);
}

}  // namespace walkercore

#endif  // WALKERCORE_PROVIDER_MIN_HOOK_INIT_H
