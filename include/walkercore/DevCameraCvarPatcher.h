// WalkerCore — DevCameraCvarPatcher (M2 dev-camera prerequisite).
//
// Why this exists:
//   TMRealWalk (the working reference, see
//   docs/observations/tmrealwalk-findings.md § "Camera manager + cvar
//   patcher") ships its own debug camera path that requires the in-memory
//   cvar `g_developer` to read `1`. Their README explicitly mentions
//   the failure mode: "F10 fades to black and then just puts you back
//   in the cab — the game's own free camera needs g_developer turned
//   on". TMRealWalk works around this by patching the cvar to `1`
//   in memory at runtime: their log line `[camera] g_developer was 0:
//   set to 1 in memory for the free camera (config.cfg unchanged)` is
//   emitted from their camera manager finder path. The README notes
//   that the user can also add `uset g_developer "1"` to their
//   config.cfg, but the in-memory patch is the no-config-edit path.
//
//   Our WalkerCore path is structurally similar — the F10 Walking
//   transition will eventually drive the SCS dev free camera
//   (whatever the source of the toggle is — concommand invocation,
//   camera manager API, etc.). All of those paths require
//   `g_developer = 1` at the moment we invoke them, otherwise the
//   game silently no-ops ("Only supported in free camera mode." is
//   the runtime error we RE-confirmed at RVA 0x2333FB8 / file
//   offset 0x23329FB8).
//
// What this provider does:
//   1. Resolve amtrucks.exe image base via GetModuleHandleA("amtrucks.exe").
//   2. Compute the rebased location of the cvar name string at
//      RVA 0x22408C0 in ATS 1.61.3.1s. The original commit
//      (2cf1867) used 0x223F6C0 — the file offset, NOT the RVA
//      (the PE has SectionAlignment=0x1000 but FileAlignment=0x200,
//      so .rdata has a +0x200 offset between file and RVA; the
//      literal 0x223F6C0 only happens to match because the
//      difference happens to be the section's rawaddr, not zero).
//      The 0x22408C0 RVA was derived from the file offset
//      0x223F6C0 by walking the .rdata section header (VAddr
//      0x1DBE000, RawAddr 0x1DBCE00, so the .rdata-resident
//      offset +0x4828C0 above the section start gives
//      RVA 0x22408C0). Verify the string at that VA is exactly
//      `g_developer\0`.
//   3. Scan amtrucks.exe's data region (read-only PE scan, no
//      allocation, no writing) for pointers that reference the cvar
//      name VA. The first such pointer that points at a neighbouring
//      string buffer holding either `0` or `1` (and is preceded by
//      another pointer into the same allocation) is the cvar control
//      struct. This is heuristic but reliable for the SCS CVar
//      layout — verified by reading the cvar registration block at
//      RVA 0x330xx (which loads `g_developer` via
//      `lea rax, [0x1422408C0]` per find_camera_refs.py).
//   4. Patch the value buffer in-place from "0" to "1" (a single byte
//      overwrite, wrapped in VirtualProtect to keep the page writable
//      during the patch then restored to PAGE_READONLY).
//   5. Log the entire traceback so they can find it in game.log.txt:
//        - the rebased cvar name VA
//        - the value buffer VA
//        - the original and patched bytes (with hex)
//        - the patch success / failure status
//
// Why a separate provider (and not folded into GameFreeCameraShim):
//   - The shim's contract is "synthesise keystrokes"; it doesn't
//     touch game state. Adding memory patching would couple it to
//     amtrucks.exe's specific build and break its tests.
//   - The patcher is independent of the shim's enable state. The
//     dev-camera cvar is a prerequisite for any dev-camera toggle
//     path, not just the shim's.
//   - The patcher has its own gate (`[m2] dev_cvar_patcher`) so users
//     who never want us to write to game memory can opt out (the
//     shim's `uset` config.cfg edit is the user-controlled
//     alternative).
//
// ABI:
//   walker_dev_camera_cvar_patcher_supported()
//   walker_dev_camera_cvar_patcher_run_once()
//   walker_dump_dev_camera_cvar_patcher()
//
// Default `[m2] dev_cvar_patcher = true`. Auto-migrate persists
// `true` so the patcher is in effect on the next run. The gate is
// independent of `m2_game_free_camera_shim` — users who disable the
// shim still want the cvar patched so that any other dev-camera
// path (the `~` console command `free_camera_u`, a future camera
// manager resolver, etc.) can take effect.

#ifndef WALKERCORE_DEV_CAMERA_CVAR_PATCHER_H
#define WALKERCORE_DEV_CAMERA_CVAR_PATCHER_H

#include <cstdint>

namespace walkercore {

// Result of a single patch attempt. Surfaced both in the F12 dump
// and in the per-run log line so the next iteration can fold the
// diagnosis into a one-liner.
struct DevCameraCvarPatchResult {
    // True if we successfully overwrote "0" → "1" (or wrote "1" to
    // a freshly-discovered empty cvar). False if any precondition
    // was not met (module not loaded, signature mismatch, value
    // buffer not discoverable, value was not "0", or the page
    // could not be protected for write).
    bool patched = false;
    // False if we declined to patch because the user disabled the
    // gate, the module was not loaded yet (probe is best-effort and
    // may be re-attempted on a later tick), or the signature was
    // not the expected cvar.
    bool skipped = false;
    // The rebased VA of the cvar name string (the literal
    // "g_developer\0" at RVA 0x22408C0 in ATS 1.61.3.1s). 0 if not resolved.
    uint64_t cvar_name_va = 0;
    // The rebased VA of the value buffer (where we wrote "1"). 0
    // if not resolved.
    uint64_t cvar_value_va = 0;
    // The first 32 bytes of the cvar value buffer as observed
    // before the patch (truncated if the buffer is shorter). The
    // expected original value is "0\0" — anything else is logged
    // for the analyst.
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

    // INI gate. The patcher's default is `true` (enabled) because
    // the patch is required for any future dev-camera toggle path
    // to work, and the patch is one-shot (PatchOnce() only writes if
    // the value is still "0").
    void SetEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    // INI knobs. The defaults match the offsets confirmed for ATS
    // 1.61.3.1s in `rea/scripts/tmrealwalk-re/find_camera_refs.py`
    // and `find_g_developer.py`. Future builds may move the cvar
    // string (SCS occasionally relocates the cvar table for
    // localization reasons); the test seam below lets the test
    // suite exercise the lookup without going through the OS module
    // loader.
    void SetNameRva(std::uint64_t rva);
    std::uint64_t name_rva() const { return name_rva_; }

    // One-shot patch attempt. Returns the outcome. Idempotent: if
    // the cvar value already reads "1" the patcher reports
    // `skipped=true, patched=false` and does not touch memory.
    // Calling PatchOnce() multiple times is safe; subsequent calls
    // see the patched value and short-circuit. The patch is
    // therefore at most one write per process lifetime.
    DevCameraCvarPatchResult PatchOnce();

    // Diagnostic accessors.
    int runs() const { return runs_; }
    int patches() const { return patches_; }
    int skips_already_patched() const { return skips_already_patched_; }
    int skips_disabled() const { return skips_disabled_; }
    int skips_no_module() const { return skips_no_module_; }
    int skips_signature_mismatch() const { return skips_signature_mismatch_; }
    int skips_value_unreadable() const { return skips_value_unreadable_; }
    int skips_protect_failed() const { return skips_protect_failed_; }
    const DevCameraCvarPatchResult &last_result() const { return last_result_; }

    // -- Test-only --------------------------------------------------------
    // Override the module base the patcher scans. Test seam for the
    // data-path resolution that does not rely on GetModuleHandleA.
    // Production code never calls this.
    void SetModuleBaseForTesting(std::uint64_t base);

private:
    std::uint64_t module_base_for_testing_ = 0;

    bool enabled_ = true;
    std::uint64_t name_rva_ = 0x22408C0ULL;  // g_developer RVA, ATS 1.61.3.1s (.rdata, file offset 0x223F6C0)
    DevCameraCvarPatchResult last_result_{};
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