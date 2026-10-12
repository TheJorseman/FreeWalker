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

// RVA of the `mov rbx, [rip + 0x2A95E7F]` instruction inside the
// dispatcher prologue. RIP-relative addressing resolves to
// image_base + 0xC22F4A + 7 + 0x2A95E7F = image_base + 0x1423A6D0.
constexpr std::uint32_t kDispatcherPrologueMovRbxRva = 0xC22F4Au;
constexpr std::int32_t kDispatcherPrologueMovRbxDisp = 0x2A95E7F;
constexpr std::size_t kDispatcherPrologueMovRbxLength = 7u;
// Convenience pre-computed RVA of the singleton global relative to
// image_base. Computed lazily by ComputeSingletonGlobalVa() below so
// the formula lives in one place.
constexpr std::uint64_t kSingletonGlobalRva =
    static_cast<std::uint64_t>(kDispatcherPrologueMovRbxRva) +
    static_cast<std::uint64_t>(kDispatcherPrologueMovRbxLength) +
    static_cast<std::uint64_t>(static_cast<std::uint32_t>(kDispatcherPrologueMovRbxDisp));

// vtable slot index * 8 bytes/entry = 0x80 on x64.
constexpr std::uint64_t kVtableSlot16Offset = 16ull * 8ull;

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
std::uint64_t CameraHijack::ComputeSingletonGlobalVa(std::uint64_t image_base) {
    return image_base + kSingletonGlobalRva;
}

// static
std::uint64_t CameraHijack::ComputeVtableSlot16Va(std::uint64_t vtable_base) {
    return vtable_base + kVtableSlot16Offset;
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
    // call the original and bump the counter; v2 folds in the
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
        // v2: lazy singleton walk + vtable[16] MinHook. Runs on the
        // first dispatch only — subsequent calls early-out because
        // vtable16_installed_ is true.
        if (!inst->vtable16_installed_) {
            if (!inst->LazyInstallVtable16Hook()) {
                // Lazy install failed (read returned null, MH_*
                // failed). Don't re-attempt every dispatch — log
                // once and bail. The next install attempt (DLL
                // reload, INI flip) creates a fresh CameraHijack.
                inst->vtable16_installed_ = true;  // sentinel: stop retrying
            }
        }
    }
#else
    (void)0;
#endif
}

bool CameraHijack::LazyInstallVtable16Hook() {
    // Defensive: refuse if image_base is unset, if vtable[16] was
    // already installed, or if MinHook isn't vendored.
    if (image_base_ == 0) {
        WALKERCORE_LOG_WARN("CameraHijack: LazyInstallVtable16Hook refused (image_base=0)");
        return false;
    }
#ifdef WALKERCORE_HAVE_MINHOOK
    // 1) Read the singleton global (the same RIP-relative target the
    //    original prologue reads at RVA 0xC22F4A).
    singleton_global_va_ = ComputeSingletonGlobalVa(image_base_);
    std::uint64_t singleton_value = 0;
    {
            const auto *p = reinterpret_cast<const std::uint8_t *>(
                static_cast<std::uintptr_t>(singleton_global_va_));
            (void)std::memcpy(&singleton_value, p, sizeof(singleton_value));
    }
    if (singleton_value == 0) {
        WALKERCORE_LOG_WARN("CameraHijack: singleton global at 0x%llX is NULL",
                            static_cast<unsigned long long>(singleton_global_va_));
        return false;
    }
    // 2) Read the camera state struct pointer at singleton + 0x2030.
    cam_state_struct_va_ = singleton_value + 0x2030u;
    std::uint64_t cam_state_struct = 0;
    {
            const auto *p = reinterpret_cast<const std::uint8_t *>(
                static_cast<std::uintptr_t>(cam_state_struct_va_));
            (void)std::memcpy(&cam_state_struct, p, sizeof(cam_state_struct));
    }
    if (cam_state_struct == 0) {
        WALKERCORE_LOG_WARN("CameraHijack: camera state struct at 0x%llX is NULL",
                            static_cast<unsigned long long>(cam_state_struct_va_));
        return false;
    }
    // 3) Read the vtable pointer at struct + 0.
    vtable_base_va_ = cam_state_struct;
    std::uint64_t vtable_base = 0;
    {
            const auto *p = reinterpret_cast<const std::uint8_t *>(
                static_cast<std::uintptr_t>(vtable_base_va_));
            (void)std::memcpy(&vtable_base, p, sizeof(vtable_base));
    }
    if (vtable_base == 0) {
        WALKERCORE_LOG_WARN("CameraHijack: vtable at 0x%llX is NULL",
                            static_cast<unsigned long long>(vtable_base_va_));
        return false;
    }
    // 4) Read vtable[16] = vtable_base + 0x80.
    vtable16_va_ = ComputeVtableSlot16Va(vtable_base);
    std::uint64_t vtable16_value = 0;
    {
            const auto *p = reinterpret_cast<const std::uint8_t *>(
                static_cast<std::uintptr_t>(vtable16_va_));
            (void)std::memcpy(&vtable16_value, p, sizeof(vtable16_value));
    }
    if (vtable16_value == 0) {
        WALKERCORE_LOG_WARN("CameraHijack: vtable[16] at 0x%llX is NULL",
                            static_cast<unsigned long long>(vtable16_va_));
        return false;
    }
    // 5) MinHook vtable[16] (lazy install — only runs once per
    //    CameraHijack lifetime).
    MH_STATUS mh_status = MH_CreateHook(
        reinterpret_cast<LPVOID>(vtable16_value),
        reinterpret_cast<LPVOID>(&Vtable16HookThunk),
        reinterpret_cast<LPVOID *>(&original_vtable16_));
    if (mh_status != MH_OK) {
        WALKERCORE_LOG_WARN("CameraHijack: MH_CreateHook(vtable[16]) failed (status=%d)",
                            static_cast<int>(mh_status));
        return false;
    }
    mh_status = MH_EnableHook(reinterpret_cast<LPVOID>(vtable16_value));
    if (mh_status != MH_OK) {
        WALKERCORE_LOG_WARN("CameraHijack: MH_EnableHook(vtable[16]) failed (status=%d)",
                            static_cast<int>(mh_status));
        MH_RemoveHook(reinterpret_cast<LPVOID>(vtable16_value));
        original_vtable16_ = nullptr;
        return false;
    }
    vtable16_installed_ = true;
    WALKERCORE_LOG_INFO(
        "CameraHijack: vtable[16] hooked at 0x%llX (vtable_base=0x%llX, "
        "cam_state=0x%llX, singleton_global=0x%llX)",
        static_cast<unsigned long long>(vtable16_value),
        static_cast<unsigned long long>(vtable_base),
        static_cast<unsigned long long>(cam_state_struct),
        static_cast<unsigned long long>(singleton_value));
    return true;
#else
    WALKERCORE_LOG_WARN("CameraHijack: LazyInstallVtable16Hook requires WALKERCORE_HAVE_MINHOOK=1");
    return false;
#endif
}

void WINAPI CameraHijack::Vtable16HookThunk() {
    // The per-frame camera update trampoline. Calls the original
    // (which writes SCS matrices to the GPU constant buffer slot 0),
    // bumps vtable16_calls_, and emits a one-line log every 256th
    // call so a deploy-side analyst can confirm vtable[16] is being
    // hit. v3 will replace the GPU constant buffer push from here.
#ifdef WALKERCORE_HAVE_MINHOOK
    using OriginalFn = void (*)();
    OriginalFn original = nullptr;
    CameraHijack *inst = g_instance_;
    if (inst != nullptr) {
        original = reinterpret_cast<OriginalFn>(inst->original_vtable16_);
    }
    if (original != nullptr) {
        original();
    }
    if (inst != nullptr) {
        ++inst->vtable16_calls_;
        if ((inst->vtable16_calls_ & 0xFF) == 1) {
            WALKERCORE_LOG_INFO("CameraHijack: vtable[16] called (n=%d)",
                                inst->vtable16_calls_);
        }
    }
#else
    (void)0;
#endif
}
#endif  // _WIN32

}  // namespace walkercore