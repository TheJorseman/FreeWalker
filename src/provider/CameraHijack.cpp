// WalkerCore — Camera Hijack (M2 long-term).
//
// See include/walkercore/CameraHijack.h for the design. This file
// implements the resolver test seams (no game process required) and
// the runtime install path (DLL-only, gated on WALKERCORE_HAVE_MINHOOK).
//
// v1 scope is intentionally narrow:
//   - The image base is captured via GetModuleHandleA("amtrucks.exe").
//   - The dispatcher VA is computed from that image base plus the
//     known RVA (0xC22F40) that find_camera_function.py pinned in
//     the REA repo.
//   - MinHook installs the trampoline. The trampoline calls the
//     original dispatcher first so SCS's state machine continues to
//     function. After the call, the trampoline logs a one-line
//     "camera hijack: dispatcher called (N)" message that becomes a
//     visible signal in game.log.txt the moment the player enters a
//     state that hits the dispatcher.
//   - Replacing the matrices with our own is a v2 follow-up.
//
// Pure-logic helpers (ComputeDispatcherVa, IsRvaExecutable) are
// exposed as static members so the unit-test suite can exercise the
// install math without booting the game.

#include "walkercore/CameraHijack.h"

#include <cstring>

#include "walkercore/Log.h"
#include "walkercore/Time.h"

#ifdef WALKERCORE_HAVE_MINHOOK
#include "MinHook.h"
#endif

namespace walkercore {

// Static member definition.
CameraHijack *CameraHijack::g_instance_ = nullptr;

namespace {

// RVA of the camera state dispatcher function we identified by static
// signature scan in scripts/tmrealwalk-re/find_camera_function.py
// (REA repo, commit 95b16cc7). The function:
//   1. Loads a camera singleton via `mov rbx, [rip + 0x2A95E7F]`.
//   2. On the state==-1 path, calls qword ptr [rdx + 0x80]
//      (= the per-frame camera update at vtable slot 16).
//   3. Overrides yaw/pitch to 0x42B40000 (= 75.0 deg) before the
//      vtable call and restores them after.
// The RVA is relative to the PE image base (PE32+ optional header
// field 24 for x86-64). 0xC22F40 falls inside .text (RVA 0x1000,
// raw size 0x1DBC800).
constexpr std::uint32_t kDispatcherRva = 0xC22F40u;

// IMAGE_SCN_MEM_EXECUTE = 0x20000000. We only need this bit for
// "is RVA inside an executable section?" checks during install.
constexpr std::uint32_t kImageScnMemExecute = 0x20000000u;

// The pe_base name expected by GetModuleHandleA on Windows when
// asked for the game executable. The user might rename amtrucks.exe
// under a Steam beta branch — for that case we additionally fall
// back to the running process's main module (NULL argument returns
// the host .exe handle). On the host the canonical name is
// amtrucks.exe for ATS and eurotrucks2.exe for ETS2; both are
// covered by the NULL fallback below.
#ifdef _WIN32
constexpr const char *kGameModuleNames[] = {
    "amtrucks.exe",
    "eurotrucks2.exe",
};
#endif

}  // namespace

CameraHijack::CameraHijack() = default;

CameraHijack::~CameraHijack() {
    Uninstall();
}

// static
std::uint64_t CameraHijack::ComputeDispatcherVa(std::uint64_t image_base) {
    return image_base + static_cast<std::uint64_t>(kDispatcherRva);
}

// static
bool CameraHijack::IsRvaExecutable(std::uint32_t rva,
                                   const PeSection *sections,
                                   std::size_t section_count) {
    for (std::size_t i = 0; i < section_count; ++i) {
        const PeSection &s = sections[i];
        const std::uint32_t lo = s.virtual_address;
        const std::uint32_t hi = s.virtual_address + s.virtual_size;
        if (lo <= rva && rva < hi) {
            return (s.characteristics & kImageScnMemExecute) != 0;
        }
    }
    return false;
}

void CameraHijack::SetEnabled(bool enabled) {
    enabled_ = enabled;
}

bool CameraHijack::Install() {
    ++install_attempts_;
    if (!enabled_) {
        install_error_ = "camera hijack gate is closed (set [m2] camera_hijack = 1 to enable)";
        return false;
    }
    if (installed_) {
        install_error_.clear();
        return true;  // idempotent
    }

#ifdef _WIN32
    // 1) Resolve the game module handle. amtrucks.exe is the ATS host;
    // eurotrucks2.exe is the ETS2 host. Both are covered. NULL returns
    // the calling process's main module handle, which on a DLL-loaded
    // SCS plugin is the game exe.
    HMODULE game_module = nullptr;
    for (const char *name : kGameModuleNames) {
        game_module = GetModuleHandleA(name);
        if (game_module != nullptr) {
            break;
        }
    }
    if (game_module == nullptr) {
        game_module = GetModuleHandleA(nullptr);
    }
    if (game_module == nullptr) {
        install_error_ = "GetModuleHandleA returned NULL — game module not loaded";
        WALKERCORE_LOG_ERROR("CameraHijack::Install: %s", install_error_.c_str());
        return false;
    }

    const std::uint64_t ib = reinterpret_cast<std::uint64_t>(game_module);
    image_base_ = ib;
    dispatcher_va_ = ComputeDispatcherVa(ib);
#else
    install_error_ = "Camera hijack requires Windows (MinHook + image base resolution)";
    WALKERCORE_LOG_ERROR("CameraHijack::Install: %s", install_error_.c_str());
    return false;
#endif

#ifdef WALKERCORE_HAVE_MINHOOK
    // 2) MinHook install. The MinHook trampoline is allocated in
    // executable memory automatically; the original 14 bytes of the
    // dispatcher prologue are patched with a JMP to our hook.
    MH_STATUS mh_status = MH_Initialize();
    if (mh_status != MH_OK && mh_status != MH_ERROR_ALREADY_INITIALIZED) {
        install_error_ = "MH_Initialize failed (status=" + std::to_string(mh_status) + ")";
        WALKERCORE_LOG_ERROR("CameraHijack::Install: %s", install_error_.c_str());
        return false;
    }
    mh_status = MH_CreateHook(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(dispatcher_va_)),
                              reinterpret_cast<LPVOID>(&DispatcherHookThunk),
                              reinterpret_cast<LPVOID *>(&original_dispatcher_));
    if (mh_status != MH_OK) {
        install_error_ = "MH_CreateHook failed (status=" + std::to_string(mh_status) + ")";
        WALKERCORE_LOG_ERROR("CameraHijack::Install: %s", install_error_.c_str());
        return false;
    }
    mh_status = MH_EnableHook(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(dispatcher_va_)));
    if (mh_status != MH_OK) {
        install_error_ = "MH_EnableHook failed (status=" + std::to_string(mh_status) + ")";
        WALKERCORE_LOG_ERROR("CameraHijack::Install: %s", install_error_.c_str());
        MH_RemoveHook(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(dispatcher_va_)));
        original_dispatcher_ = nullptr;
        return false;
    }
    installed_ = true;
    install_error_.clear();
    g_instance_ = this;
    WALKERCORE_LOG_INFO("CameraHijack: dispatcher hooked at 0x%llX (image_base=0x%llX)",
                        static_cast<unsigned long long>(dispatcher_va_),
                        static_cast<unsigned long long>(image_base_));
    return true;
#else
    install_error_ = "WalkerCore was built without WALKERCORE_HAVE_MINHOOK=1; dispatcher hook unavailable";
    WALKERCORE_LOG_ERROR("CameraHijack::Install: %s", install_error_.c_str());
    return false;
#endif
}

void CameraHijack::Uninstall() {
#ifdef WALKERCORE_HAVE_MINHOOK
    if (installed_ && dispatcher_va_ != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(dispatcher_va_)));
        MH_RemoveHook(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(dispatcher_va_)));
        original_dispatcher_ = nullptr;
        WALKERCORE_LOG_INFO("CameraHijack: dispatcher unhooked");
    }
#endif
    if (g_instance_ == this) {
        g_instance_ = nullptr;
    }
    installed_ = false;
    dispatcher_va_ = 0;
    image_base_ = 0;
}

#ifdef _WIN32
void WINAPI CameraHijack::DispatcherHookThunk() {
    // Single-instance dispatcher hook. The trampoline routes through
    // the global instance registered at install time. In v1 we only
    // call the original and bump the counter; v2 will fold in the
    // singleton walk + vtable[16] hook + matrix override.
#ifdef WALKERCORE_HAVE_MINHOOK
    using OriginalFn = void (*)();
    OriginalFn original = nullptr;
    CameraHijack *inst = g_instance_;
    if (inst != nullptr) {
        original = reinterpret_cast<OriginalFn>(inst->original_dispatcher_);
    }
    if (original != nullptr) {
        original();
    }
    if (inst != nullptr) {
        ++inst->dispatch_calls_;
        inst->last_dispatch_ms_ = static_cast<int>(NowMicros() / 1000LL);
        if ((inst->dispatch_calls_ & 0x3F) == 1) {
            WALKERCORE_LOG_INFO("CameraHijack: dispatcher called (n=%d)", inst->dispatch_calls_);
        }
    }
#else
    (void)0;
#endif
}
#endif  // _WIN32

}  // namespace walkercore