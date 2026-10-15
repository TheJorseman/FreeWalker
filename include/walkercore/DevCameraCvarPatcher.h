// WalkerCore — DevCameraCvarPatcher (M2 dev-camera prerequisite).
//
// Why this exists:
//   SCS's dev free camera path requires both `g_developer=1` AND
//   `g_debug_camera=1` in the live cvar registry. Without them the
//   dev console's `free_camera` concommand and the `~`-toggleable
//   dev free roam view are both inert ("Only supported in free camera
//   mode."). The dev console help pages (Sparked Host, YouTube
//   tutorials on the SCS dev console) describe the toggle as
//   'press 0 to enter the debug camera' once both cvars are on.
//
//   We patch both cvars to "1" in amtrucks.exe memory on the first
//   scs_telemetry_init call so the user does not have to edit
//   `config.cfg` manually. The patch is one-shot per process lifetime
//   (value already reads "1" → short-circuit) and writes a single
//   byte wrapped in VirtualProtect so the original page protection
//   is restored after the write.
//
//   TMRealWalk (the working reference, see
//   docs/observations/tmrealwalk-findings.md § "Camera manager + cvar
//   patcher") patches `g_developer` only because that cvar alone is
//   enough for *their* camera path (which uses the dev console
//   directly). WalkerCore needs both because the SCS dev free camera
//   reads `g_debug_camera=1` as the explicit unlock.
    //
// What this provider does:
//   1. Resolve amtrucks.exe image base via GetModuleHandleA("amtrucks.exe").
//   2. For each cvar in our patch set:
//        - Verify the literal cvar name string at the rebased RVA
//          (e.g. "g_developer\0" at RVA 0x22408C0 in ATS 1.61.3.1s).
//        - Scan amtrucks.exe's data region (read-only PE scan, no
//          allocation, no writing) for pointers that reference the
//          cvar name VA. The first such pointer that points at a
//          neighbouring buffer holding either `0` or `1` is the cvar
//          control struct.
//        - Patch the value buffer in-place from "0" to "1".
//   3. Log every cvar touch separately so the analyst sees exactly
//      which cvars were patched vs skipped.
//
// Default patch set:
//   - `g_developer` at RVA 0x22408C0 (.rdata-resident; file offset
//     0x223F6C0 in 1.61.3.1s).
//   - `g_debug_camera` at RVA 0x2240760 (.rdata-resident; file
//     offset 0x0223F560 in 1.61.3.1s — verified live Oct 2026 via
//     file-byte search, sits 0x160 below the `g_developer` name
//     in the same .rdata cache line).
//
// Why a separate provider (and not folded into GameFreeCameraShim):
//   - The shim's contract is "synthesise keystrokes"; it doesn't
//     touch game state. Adding memory patching would couple it to
//     amtrucks.exe's specific build and break its tests.
//   - The patcher is independent of the shim's enable state. The
//     dev-camera cvars are a prerequisite for any dev-camera toggle
//     path, not just the shim's.
//   - The patcher has its own gate (`[m2] dev_cvar_patcher`) so users
//     who never want us to write to game memory can opt out.
//
// ABI:
//   walker_dev_camera_cvar_patcher_supported()
//   walker_dev_camera_cvar_patcher_run_once()
//   walker_dump_dev_camera_cvar_patcher()
//
// Default `[m2] dev_cvar_patcher = true`. Auto-migrate persists
// `true` so the patcher is in effect on the next run.

#ifndef WALKERCORE_DEV_CAMERA_CVAR_PATCHER_H
#define WALKERCORE_DEV_CAMERA_CVAR_PATCHER_H

#include <cstdint>

namespace walkercore {

// Result of a single patch attempt for the first cvar in the patch
// set (kept as the public Result for ABI stability — callers like
// `walker_dev_camera_cvar_patcher_supported` and the F12 dump read
// this). The second cvar's per-call outcome is exposed through
// `last_debug_camera_result()` for instrumentation.
struct DevCameraCvarPatchResult {
    bool patched = false;
    bool skipped = false;
    // The rebased VA of the cvar name string. 0 if not resolved.
    uint64_t cvar_name_va = 0;
    // The rebased VA of the value buffer (where we wrote "1"). 0
    // if not resolved.
    uint64_t cvar_value_va = 0;
    // The first 32 bytes of the cvar value buffer as observed before
    // the patch. The expected original value is "0\0".
    char cvar_value_before[32] = {0};
    // Same, after.
    char cvar_value_after[32] = {0};
};

class DevCameraCvarPatcher {
public:
    DevCameraCvarPatcher();
    ~DevCameraCvarPatcher();

    DevCameraCvarPatcher(const DevCameraCvarPatcher &) = delete;
    DevCameraCvarPatcher &operator=(const DevCameraCvarPatcher &) = delete;

    // INI gate. The patcher's default is `true` (enabled).
    void SetEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    // INI knobs for the g_developer cvar (the master dev console
    // switch). The default matches the RVA observed for ATS
    // 1.61.3.1s in `rea/scripts/tmrealwalk-re/find_camera_refs.py`.
    void SetNameRva(std::uint64_t rva);
    std::uint64_t name_rva() const { return name_rva_; }

    // INI knob for the g_debug_camera cvar (the explicit dev free
    // camera unlock). Default RVA derived from the same `.rdata`
    // section math as `g_developer` but for the offset observed in
    // 1.61.3.1s's SCS cvar table. If the value is wrong for a
    // newer build, the signature-mismatch branch skips gracefully.
    void SetDebugCameraNameRva(std::uint64_t rva);
    std::uint64_t debug_camera_name_rva() const {
        return debug_camera_name_rva_;
    }

    // One-shot patch attempt. Walks every cvar in the patch set in
    // order. The public `last_result()` reports the outcome for the
    // first cvar (`g_developer`); the second cvar's outcome is in
    // `last_debug_camera_result()`.
    DevCameraCvarPatchResult PatchOnce();

    // Diagnostic accessors for the F12 dump and per-run log line.
    int runs() const { return runs_; }
    int patches() const { return patches_; }
    int skips_already_patched() const { return skips_already_patched_; }
    int skips_disabled() const { return skips_disabled_; }
    int skips_no_module() const { return skips_no_module_; }
    int skips_signature_mismatch() const { return skips_signature_mismatch_; }
    int skips_value_unreadable() const { return skips_value_unreadable_; }
    int skips_protect_failed() const { return skips_protect_failed_; }
    const DevCameraCvarPatchResult &last_result() const { return last_result_; }
    // Outcome of the secondary cvar (`g_debug_camera`) on the most
    // recent PatchOnce call.
    const DevCameraCvarPatchResult &last_debug_camera_result() const {
        return last_debug_camera_result_;
    }

    // -- Test-only --------------------------------------------------------
    // Override the module base the patcher scans. Test seam for the
    // data-path resolution that does not rely on GetModuleHandleA.
    // Production code never calls this.
    void SetModuleBaseForTesting(std::uint64_t base);

private:
    std::uint64_t module_base_for_testing_ = 0;

    bool enabled_ = true;
    std::uint64_t name_rva_ = 0x22408C0ULL;                  // g_developer RVA
    std::uint64_t debug_camera_name_rva_ = 0x2240760ULL;    // g_debug_camera RVA
    DevCameraCvarPatchResult last_result_{};
    DevCameraCvarPatchResult last_debug_camera_result_{};
    int runs_ = 0;
    int patches_ = 0;
    int skips_already_patched_ = 0;
    int skips_disabled_ = 0;
    int skips_no_module_ = 0;
    int skips_signature_mismatch_ = 0;
    int skips_value_unreadable_ = 0;
    int skips_protect_failed_ = 0;
    bool already_patched_ = false;
};

}  // namespace walkercore

#endif  // WALKERCORE_DEV_CAMERA_CVAR_PATCHER_H
