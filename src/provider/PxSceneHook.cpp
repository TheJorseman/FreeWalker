// WalkerCore — PxSceneHook provider implementation (M3.5 lander).
//
// The provider installs MinHook on `PxScene::addActor` at VA
// 0x141AE0B62 (the catalog slot-11 entry for ATS 1.61.3.1s, image
// base 0x140000000). On entry the hooked function captures RCX
// (the PxScene this_ptr) into a process-global atomic and forwards
// the call to the original via MinHook's trampoline.
//
// The hook is one-way (read-only): we never mutate game state and
// never call back into PxScene methods from the hook. The only
// side effect is the captured-pointer readback path used by
// downstream M3 work + the `walker_dump_pxscene` F12 dump.
//
// When MinHook is not vendored (`WALKERCORE_HAVE_MINHOK=0`), or
// when amtrucks.exe is not in this process (out-of-game debug
// runs, unit tests), the hook is a no-op: Install() still returns
// true so the ABI surface stays available, target_reachable()
// stays false, and the captured instance stays 0.

#include "walkercore/PxSceneHook.h"

#include <atomic>
#include <cstdio>
#include <string>

#include "walkercore/Log.h"

#if defined(WALKERCORE_HAVE_MINHOOK)
#include <MinHook.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#include "MinHookInit.h"

namespace walkercore {

namespace {

// Process-global state. The MinHook trampoline fires on the game
// thread; the capture path is a single 8-byte atomic store so we
// do not need a lock. The class API reads these through
// `PxSceneHook::captured_instance()` and friends.
std::atomic<uint64_t> g_captured_instance{0};
std::atomic<int> g_add_actor_calls{0};
std::atomic<bool> g_target_reachable{false};

// Trampoline pointer for the original addActor. Set once by
// MinHook_CreateHook and never reassigned. Only the trampoline
// dereferences it; nothing else in the codebase reads it.
#if defined(WALKERCORE_HAVE_MINHOOK)
using AddActorFn = void (*)(void *this_ptr, void *actor, void *aggregate);
AddActorFn g_original_add_actor = nullptr;
#endif

// Hooked function body. Receives RCX = this_ptr (PxScene*), RDX =
// actor, R8 = aggregate per the x64 calling convention. We do not
// touch the args — only save this_ptr into the process-global and
// forward the call to the original via MinHook's trampoline. Stack
// alignment + non-volatile register preservation are MinHook's
// responsibility; the trampoline compiles to the exact prologue the
// original had.
//
// The function is always defined (so the ABI surface compiles in
// every build configuration) but its body is empty when MinHook is
// not vendored — the test build + the WALKERCORE_HAVE_MINHOOK=0
// fallback path. Production builds always have MinHook wired.
extern "C" void HookedAddActor(void *this_ptr, void *actor,
                               void *aggregate) {
#if defined(WALKERCORE_HAVE_MINHOOK)
    const uint64_t ptr = reinterpret_cast<uint64_t>(this_ptr);
    // First-call capture: we only update the public global when
    // the pointer is non-zero AND we have not already captured a
    // pointer. The "already captured" check avoids polluting the
    // capture value with a transient zero (RCX is sometimes zero on
    // entry to virtual thunks), and the change-detection log helps
    // operators spot the (rare) case where the game's PxScene
    // pointer changes during a session.
    uint64_t expected = 0;
    if (ptr != 0 &&
        g_captured_instance.compare_exchange_strong(expected, ptr)) {
        WALKERCORE_LOG_INFO(
            "PxSceneHook: captured PxScene instance=0x%llX on first "
            "addActor call (target VA 0x%llX, RVA 0x%llX)",
            static_cast<unsigned long long>(ptr),
            static_cast<unsigned long long>(PxSceneHook::kAddActorVa),
            static_cast<unsigned long long>(PxSceneHook::kAddActorVa -
                                            PxSceneHook::kAmtrucksImageBase));
    } else if (ptr != 0 && g_captured_instance.load() != ptr) {
        // Pointer changed: rare but possible if the game creates a
        // second PxScene (e.g., editor / preview mode). Log it.
        WALKERCORE_LOG_WARN(
            "PxSceneHook: addActor this_ptr changed 0x%llX -> 0x%llX",
            static_cast<unsigned long long>(g_captured_instance.load()),
            static_cast<unsigned long long>(ptr));
        g_captured_instance.store(ptr);
    }
    g_add_actor_calls.fetch_add(1, std::memory_order_acq_rel);

    // Forward to the original addActor via MinHook's trampoline.
    if (g_original_add_actor != nullptr) {
        g_original_add_actor(this_ptr, actor, aggregate);
    }
#else
    (void)this_ptr;
    (void)actor;
    (void)aggregate;
#endif
}

}  // namespace

PxSceneHook::PxSceneHook() = default;
PxSceneHook::~PxSceneHook() {
    if (installed_) {
        Uninstall();
    }
}

uint64_t PxSceneHook::captured_instance() const {
    return g_captured_instance.load(std::memory_order_acquire);
}

int PxSceneHook::add_actor_calls() const {
    return g_add_actor_calls.load(std::memory_order_acquire);
}

void PxSceneHook::ObserveAddActorForTesting(uint64_t this_ptr) {
    if (this_ptr == 0) {
        return;
    }
    // First-call capture: the first non-zero pointer wins and is
    // never overwritten. Mirrors the live trampoline (compare-
    // exchange against 0). Subsequent calls only bump the counter.
    uint64_t expected = 0;
    (void)g_captured_instance.compare_exchange_strong(expected, this_ptr);
    g_add_actor_calls.fetch_add(1, std::memory_order_acq_rel);
}

void PxSceneHook::ResetForTesting() {
    g_captured_instance.store(0, std::memory_order_release);
    g_add_actor_calls.store(0, std::memory_order_release);
    g_target_reachable.store(false, std::memory_order_release);
}

bool PxSceneHook::Install() {
    if (installed_) {
        return true;
    }
    installed_ = true;

#if !defined(WALKERCORE_HAVE_MINHOOK)
    WALKERCORE_LOG_WARN(
        "PxSceneHook::Install: MinHook not vendored "
        "(WALKERCORE_HAVE_MINHOOK undefined). The hook is a no-op "
        "and target_reachable=false. Build with MinHook for live "
        "capture.");
    return false;
#else
#if !defined(_WIN32)
    (void)0;
    return false;
#else
    if (!MinHookInit()) {
        install_failure_ = "MinHookInit failed";
        WALKERCORE_LOG_ERROR(
            "PxSceneHook::Install: MinHookInit returned false; "
            "aborting before MH_CreateHook.");
        return false;
    }

    // Resolve the live address of amtrucks.exe + addActor VA. The
    // catalog VA is image_base + RVA; amtrucks.exe has a fixed
    // image base of 0x140000000 in the live process, so the
    // offset from the module base is
    //     kAddActorVa - kAmtrucksImageBase = 0x1AE0B62
    const HMODULE mod = ::GetModuleHandleA("amtrucks.exe");
    if (mod == nullptr) {
        install_failure_ = "amtrucks.exe not in-process";
        WALKERCORE_LOG_WARN(
            "PxSceneHook::Install: amtrucks.exe is not loaded in "
            "this process (GetModuleHandleA returned NULL). The "
            "ABI surface is installed but the hook will not fire "
            "until the game is running. This is expected for unit "
            "tests and debug runs without a game.");
        target_reachable_ = false;
        walkercore::MinHookShutdown();
        return false;
    }
    void *target =
        reinterpret_cast<void *>(reinterpret_cast<uint8_t *>(mod) +
                                 (kAddActorVa - kAmtrucksImageBase));
    char failure_buf[128];
    std::snprintf(failure_buf, sizeof(failure_buf),
                  "MH_CreateHook at 0x%llX failed",
                  static_cast<unsigned long long>(kAddActorVa));

    const MH_STATUS cs = MH_CreateHook(
        target,
        reinterpret_cast<void *>(&HookedAddActor),
        reinterpret_cast<void **>(&g_original_add_actor));
    if (cs != MH_OK) {
        std::snprintf(failure_buf, sizeof(failure_buf),
                      "MH_CreateHook at VA 0x%llX returned MH_STATUS=%d",
                      static_cast<unsigned long long>(kAddActorVa),
                      static_cast<int>(cs));
        install_failure_ = failure_buf;
        WALKERCORE_LOG_ERROR(
            "PxSceneHook::Install: %s. The catalog VA may not be "
            "callable in this build (ATS 1.61.3.1s only).",
            failure_buf);
        walkercore::MinHookShutdown();
        target_reachable_ = false;
        return false;
    }
    const MH_STATUS es = MH_EnableHook(target);
    if (es != MH_OK) {
        install_failure_ = "MH_EnableHook failed";
        WALKERCORE_LOG_ERROR(
            "PxSceneHook::Install: MH_EnableHook at VA 0x%llX "
            "returned MH_STATUS=%d",
            static_cast<unsigned long long>(kAddActorVa),
            static_cast<int>(es));
        MH_DisableHook(target);
        walkercore::MinHookShutdown();
        target_reachable_ = false;
        return false;
    }
    target_reachable_ = true;
    g_target_reachable.store(true, std::memory_order_release);
    WALKERCORE_LOG_INFO(
        "PxSceneHook::Install: hooked PxScene::addActor at "
        "VA 0x%llX (RVA 0x%llX). Walking Walking — first addActor "
        "call from the game will populate the captured instance "
        "pointer.",
        static_cast<unsigned long long>(kAddActorVa),
        static_cast<unsigned long long>(kAddActorVa - kAmtrucksImageBase));
    return true;
#endif  // _WIN32
#endif  // WALKERCORE_HAVE_MINHOOK
}

void PxSceneHook::Uninstall() {
    if (!installed_) {
        return;
    }
#if defined(WALKERCORE_HAVE_MINHOOK) && defined(_WIN32)
    if (target_reachable_) {
        const HMODULE mod = ::GetModuleHandleA("amtrucks.exe");
        if (mod != nullptr) {
            void *target =
                reinterpret_cast<void *>(reinterpret_cast<uint8_t *>(mod) +
                                         (kAddActorVa - kAmtrucksImageBase));
            MH_DisableHook(target);
        }
        target_reachable_ = false;
        g_target_reachable.store(false, std::memory_order_release);
        MinHookShutdown();
    }
#endif
    installed_ = false;
}

}  // namespace walkercore