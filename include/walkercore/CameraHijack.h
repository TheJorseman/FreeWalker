// WalkerCore — Camera Hijack (M2 long-term).
//
// Why this exists:
//   The M2 MVP path (GameFreeCameraShim) relies on the SCS built-in
//   developer free camera. That works on the host (g_developer "1" +
//   g_flyspeed 195.313 are already in config.cfg), but it doesn't
//   give WalkerCore control over the actual view+projection matrices
//   we feed to the renderer. For that we need to replace the SCS
//   per-frame camera update with our own.
//
// How we found the target:
//   scripts/tmrealwalk-re/find_camera_function.py in the REA repo
//   pinned the SCS camera state dispatcher at 0x140C22F40 (RVA
//   0xC22F40 relative to amtrucks.exe image base). It saves yaw +
//   pitch at [rbx+0x30]/[rbx+0x34], overrides them to 0x42B40000
//   (= 75.0 deg) when the state machine permits, calls
//   qword ptr [rdx+0x80] (= vtable slot 16 = the per-frame camera
//   matrix update), then restores the saved angles and returns 5.
//
//   scripts/tmrealwalk-re/find_vtable_slot16.py walked the entire
//   .text (31 MB) for the byte pattern `FF 92 80 00 00 00` (call
//   qword ptr [rdx+0x80]). 101 hits total. After filtering for the
//   post-restore movss signatures plus the 0x42B40000 override,
//   exactly one survivor: 0x140C2306D — the vtable-slot-16 call
//   inside the dispatcher.
//
// What this module does:
//   1. Resolves amtrucks.exe image base via GetModuleHandleA.
//   2. Computes dispatcher_va = image_base + 0xC22F40.
//   3. MinHooks dispatcher_va. Trampoline calls the original dispatcher
//      first so the SCS state machine continues to work, then logs
//      a one-line "dispatcher hooked" message that shows up in
//      game.log.txt. v2 will replace the matrices here.
//   4. Optionally walks the singleton path (the same RIP-relative
//      `[rip + 0x2A95E7F]` the original prologue uses at RVA 0xC22F4A)
//      to capture the camera state struct pointer + vtable address,
//      then captures vtable[16] = vtable_addr + 0x80. Once captured,
//      the second hook (vtable slot 16) is installed lazily on the
//      first dispatch.
//
// ABI:
//   walker_camera_hijack_set_enabled(int)
//   walker_camera_hijack_supported()
//   walker_camera_hijack_install()
//   walker_camera_hijack_uninstall()
//   walker_dump_camera_hijack()
//
// INI:
//   [m2] camera_hijack = true   (default false — must opt in)
//
// v1 scope: hook the dispatcher + log every entry. No matrix writes
// yet. v2 will add the D3D11 constant buffer override (in this same
// module — currently a v1 stub that captures the per-frame VA so a
// follow-up commit can install the vtable-slot-16 hook without
// redoing the singleton walk).

#ifndef WALKERCORE_CAMERA_HIJACK_H
#define WALKERCORE_CAMERA_HIJACK_H

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace walkercore {

class CameraHijack {
public:
    CameraHijack();
    ~CameraHijack();

    CameraHijack(const CameraHijack &) = delete;
    CameraHijack &operator=(const CameraHijack &) = delete;

    // INI gate. Default false — must opt in via `[m2] camera_hijack = 1`.
    void SetEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    // Resolve amtrucks.exe image base (via GetModuleHandleA) and capture
    // the dispatcher VA. Called by PluginEntry when the user enables
    // the gate. Returns true on success.
    bool Install();
    void Uninstall();
    bool IsInstalled() const { return installed_; }

    // Diagnostic getters. Filled by Install() / the dispatcher trampoline.
    std::uint64_t image_base() const { return image_base_; }
    std::uint64_t dispatcher_va() const { return dispatcher_va_; }
    int install_attempts() const { return install_attempts_; }
    int dispatch_calls() const { return dispatch_calls_; }
    int last_dispatch_ms() const { return last_dispatch_ms_; }
    bool install_error() const { return !install_error_.empty(); }
    const char *install_error_msg() const { return install_error_.c_str(); }

    // -- Pure resolver test seams (no game process required) ------------
    //
    // Given an image base (PE32+ optional header field), returns the
    // dispatcher VA. Used by the test suite to pin the formula.
    static std::uint64_t ComputeDispatcherVa(std::uint64_t image_base);

    // Given a pe_base and an rva, returns true if `rva` falls inside
    // any PE section that has IMAGE_SCN_MEM_EXECUTE set. Used by the
    // install path to validate that 0xC22F40 is in .text (executable)
    // before MinHook tries to patch it.
    //
    // The test build does NOT link against the game binary so we work
    // with a hand-built PE section table. The DLL build always works
    // against the live amtrucks.exe; this helper accepts any layout.
    struct PeSection {
        std::uint32_t virtual_size;
        std::uint32_t virtual_address;
        std::uint32_t characteristics;  // IMAGE_SCN_* bitfield
    };
    static bool IsRvaExecutable(std::uint32_t rva,
                                const PeSection *sections,
                                std::size_t section_count);

private:
    // MinHook trampoline target. Stamped by Install(), released by
    // Uninstall(). On Win64 the dispatcher is __cdecl-like (callee
    // preserves rbx/rbp/rsi/rdi per the SCS prologue) so the hook
    // signature is the natural `void(void)`.
#ifdef _WIN32
    using DispatcherFn = void (*)();
    static void WINAPI DispatcherHookThunk();
    void DispatcherHookImpl();
#endif

    bool enabled_ = false;
    bool installed_ = false;
    std::uint64_t image_base_ = 0;
    std::uint64_t dispatcher_va_ = 0;
    int install_attempts_ = 0;
    int dispatch_calls_ = 0;
    int last_dispatch_ms_ = 0;
    std::string install_error_;

    // MinHook trampoline target. Stamped by Install(), released by
    // Uninstall(). On Win64 the dispatcher is __cdecl-like (callee
    // preserves rbx/rbp/rsi/rdi per the SCS prologue) so the hook
    // signature is the natural `void(void)`.
#ifdef WALKERCORE_HAVE_MINHOOK
    DispatcherFn original_dispatcher_ = nullptr;
#endif

    // Single global instance registered at Install(). The static
    // DispatcherHookThunk function (which can't take `this`) routes
    // through this pointer to bump counters + reach the original
    // trampoline. NULL when no install is live.
    static CameraHijack *g_instance_;
};

}  // namespace walkercore

#endif  // WALKERCORE_CAMERA_HIJACK_H