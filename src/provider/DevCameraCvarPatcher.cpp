// WalkerCore — DevCameraCvarPatcher implementation.
//
// Patches two SCS cvars (`g_developer`, `g_debug_camera`) in
// amtrucks.exe memory from "0" to "1" so the dev free-camera path
// becomes reachable from the dev console without the user editing
// `config.cfg` manually.
//
// Why a "0" → "1" patch and not a free-form write:
//   - The patcher's job is to flip the dev-camera gates. The game
//     accepts any non-zero value but the default cvar value of "0"
//     means a developer who wants the gate on writes "1" or "2". We
//     follow the same convention.
//   - If the user has already explicitly set `g_developer "2"` in
//     config.cfg (max diagnostic reports), the cvar will read "2"
//     in memory. The patcher detects that as "already enabled" and
//     short-circuits without writing.
//
// Module base is amtrucks.exe's rebased image base. We resolve it via
// GetModuleHandleA at PatchOnce() time, not at construction, so the
// DLL stays a no-op before the game process is fully loaded (the
// SCS loader runs DllMain with no SDK yet — same as shim's
// BindGameHwnd).

#include "walkercore/DevCameraCvarPatcher.h"

#include "walkercore/Log.h"

#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace walkercore {

namespace {

// Resolve amtrucks.exe's image base. On non-Windows hosts returns 0.
// Production code uses GetModuleHandleA (the game is loaded before
// the SCS plugin loader runs our DllMain, so by the time
// scs_telemetry_init is called amtrucks.exe is always loaded).
std::uint64_t ResolveAmtrucksBase() {
#if defined(_WIN32)
    HMODULE m = ::GetModuleHandleA("amtrucks.exe");
    if (m == nullptr) {
        return 0;
    }
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m));
#else
    return 0;
#endif
}

// Read up to `cap` bytes from `source_addr` into `out`. Returns false
// if the read crosses an unmapped page. Used to peek at the cvar
// value buffer without crashing on a bad signature.
bool SafeRead(const void *source_addr, void *out, size_t bytes_to_read) {
#if defined(_WIN32)
    if (source_addr == nullptr) {
        return false;
    }
    // VirtualQuery gives us the protection + base of the page
    // containing `source_addr`. If the page is not
    // PAGE_READABLE-ish (any combination of R, W, X with R or W)
    // we refuse to read it.
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(source_addr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFFF;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) {
        return false;
    }
    // Copy with exception_filter is overkill — the page is readable
    // so a plain read succeeds.
    std::memcpy(out, source_addr, bytes_to_read);
    return true;
#else
    (void)source_addr;
    (void)out;
    (void)bytes_to_read;
    return false;
#endif
}

// Patch one byte at `target_addr` from `old_val` to `new_val`.
// Wrapped in VirtualProtect so the page is writable for the
// duration of the write, then restored. Returns true on success.
bool PatchByte(const void *target_addr, char new_val) {
#if defined(_WIN32)
    if (target_addr == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(target_addr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    DWORD old_protect = 0;
    if (!::VirtualProtect(mbi.BaseAddress, mbi.RegionSize,
                          PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    *static_cast<char *>(const_cast<void *>(target_addr)) = new_val;
    DWORD restored = 0;
    const bool ok = ::VirtualProtect(mbi.BaseAddress, mbi.RegionSize,
                                      old_protect, &restored);
    (void)ok;
    return true;
#else
    (void)target_addr;
    (void)new_val;
    return false;
#endif
}

}  // namespace

DevCameraCvarPatcher::DevCameraCvarPatcher() = default;
DevCameraCvarPatcher::~DevCameraCvarPatcher() = default;

void DevCameraCvarPatcher::SetEnabled(bool enabled) { enabled_ = enabled; }
void DevCameraCvarPatcher::SetNameRva(std::uint64_t rva) { name_rva_ = rva; }
void DevCameraCvarPatcher::SetDebugCameraNameRva(std::uint64_t rva) {
    debug_camera_name_rva_ = rva;
}
void DevCameraCvarPatcher::SetModuleBaseForTesting(std::uint64_t base) {
    module_base_for_testing_ = base;
}

namespace {

// Inner worker: patch one named cvar in amtrucks.exe memory.
// `needle` is the cvar name ("g_developer" / "g_debug_camera") and
// `rva` is its rebased address in ATS 1.61.3.1s.
//
// Counts and the result struct passed in are updated by side effect
// so the public PatchOnce can fold both runs into one final Result.
// Returns true iff the cvar was patched (or already on) — used by
// PatchOnce to skip the per-process idempotency check on the
// secondary cvar.
bool PatchOneCvar(const char *needle, std::uint64_t rva, std::uint64_t base,
                  DevCameraCvarPatchResult &r, int &patches_total,
                  int &skips_already, int &skips_sig_mismatch,
                  int &skips_value_unreadable, int &skips_protect_failed) {
    const std::uint64_t name_va = base + rva;
    r.cvar_name_va = name_va;

    // 1. Verify the literal cvar name at the rebased RVA.
    char expected[32] = {};
    const size_t needle_len = std::strlen(needle);
    if (needle_len + 1 > sizeof(expected)) {
        r.skipped = true;
        skips_value_unreadable++;
        return false;
    }
    if (!SafeRead(reinterpret_cast<const void *>(name_va), expected,
                  needle_len + 1)) {
        r.skipped = true;
        skips_value_unreadable++;
        WALKERCORE_LOG_WARN(
            "DevCameraCvarPatcher: skipped '%s' (could not read cvar "
            "name at VA 0x%llX — page not readable, or RVA 0x%llX is no "
            "longer the cvar location for this build).",
            needle,
            static_cast<unsigned long long>(name_va),
            static_cast<unsigned long long>(rva));
        return false;
    }
    if (std::memcmp(expected, needle, needle_len + 1) != 0) {
        r.skipped = true;
        skips_sig_mismatch++;
        WALKERCORE_LOG_WARN(
            "DevCameraCvarPatcher: skipped '%s' (signature mismatch at "
            "VA 0x%llX — expected '%s\\0', got '%.16s\\0'). The cvar "
            "has moved in this build; re-run "
            "rea/scripts/tmrealwalk-re/find_g_developer.py to find the "
            "new RVA and update DevCameraCvarPatcher.",
            needle,
            static_cast<unsigned long long>(name_va),
            needle, expected);
        return false;
    }

    // 2. Discover the cvar control struct by scanning the data
    //    region for pointers that reference the name VA, then
    //    looking for the value field at one of {+8, +16, +24, +32,
    //    +40}. Two layouts are accepted at each offset:
    //
    //    Layout A: an INLINE 1-byte buffer holding '0' / '1' / '2'
    //    followed by '\0'. The original RE pass on ATS 1.61.3.1s
    //    (Oct 2026) matched this for `g_developer` at +24 and
    //    `g_debug_camera` at +8 — the so-called "small cvar"
    //    struct.
    //
    //    Layout B: an INDIRECT pointer to a 1-byte buffer elsewhere
    //    on the heap, holding '0' / '1' / '2' '\0'. The live
    //    patch (Oct 2026) revealed the actual SCS cvar_t struct on
    //    1.61.3.1s uses this layout — `+8` holds a `char* value_`
    //    pointing at a separately heap-allocated buffer. Without
    //    handling this case, the old heuristic reports "could not
    //    discover the value field" and the cvar stays at its
    //    config-driven default ('0'), leaving the dev free-camera
    //    toggle a no-op.
    //
    //    We scan +/- 32 KiB around the name VA and try both
    //    layouts at each offset before moving on. The wider
    //    window comfortably covers both layouts and the cost is
    //    negligible (32K iterations × 8 bytes each).
    constexpr std::uint64_t kScanWindow = 32 * 1024;
    const std::uint64_t scan_lo =
        (name_va > kScanWindow) ? (name_va - kScanWindow) : 0;
    const std::uint64_t scan_hi = name_va + kScanWindow;
    bool found = false;
    std::uint64_t value_va = 0;

    for (std::uint64_t addr = scan_lo; addr + 8 <= scan_hi;
         addr += 8) {
        std::uint64_t candidate = 0;
        if (!SafeRead(reinterpret_cast<const void *>(addr), &candidate,
                      sizeof(candidate))) {
            continue;
        }
        if (candidate != name_va) {
            continue;
        }
        constexpr std::uint64_t kValueOffsets[] = {8, 16, 24, 32, 40};
        bool found_at_this_addr = false;
        std::uint64_t value_candidate = 0;
        char value_buffer[32] = {};
        for (std::uint64_t off : kValueOffsets) {
            std::memset(value_buffer, 0, sizeof(value_buffer));
            value_candidate = addr + off;
            // Layout A: inline 1-byte value buffer.
            if (SafeRead(reinterpret_cast<const void *>(value_candidate),
                         value_buffer, sizeof(value_buffer))) {
                const size_t value_len = strnlen(value_buffer, 8);
                if (value_len == 1 && value_buffer[1] == '\0' &&
                    (value_buffer[0] == '0' || value_buffer[0] == '1' ||
                     value_buffer[0] == '2')) {
                    found_at_this_addr = true;
                    break;
                }
            }
            // Layout B: indirect pointer to a heap-allocated
            // 1-byte value buffer.
            std::uint64_t value_ptr = 0;
            if (!SafeRead(reinterpret_cast<const void *>(value_candidate),
                          &value_ptr, sizeof(value_ptr))) {
                continue;
            }
            // Reject obviously-bad pointer values before deref:
            //   - must be non-zero
            //   - must look like a user-mode VA (high 16 bits
            //     unused on x64 in user space)
            //   - must not be a small integer (< 64 KiB is
            //     reserved / guard / null-page territory)
            if (value_ptr < 0x10000ULL ||
                (value_ptr & 0xFFFF000000000000ULL) != 0) {
                continue;
            }
            std::memset(value_buffer, 0, sizeof(value_buffer));
            if (!SafeRead(reinterpret_cast<const void *>(value_ptr),
                          value_buffer, sizeof(value_buffer))) {
                continue;
            }
            const size_t value_len = strnlen(value_buffer, 8);
            if (value_len == 1 && value_buffer[1] == '\0' &&
                (value_buffer[0] == '0' || value_buffer[0] == '1' ||
                 value_buffer[0] == '2')) {
                // Found indirect value. Update value_candidate
                // to the actual buffer address; PatchByte at the
                // bottom of this function then writes there.
                value_candidate = value_ptr;
                found_at_this_addr = true;
                break;
            }
        }
        if (!found_at_this_addr) {
            continue;
        }
        value_va = value_candidate;
        std::memcpy(r.cvar_value_before, value_buffer,
                    sizeof(r.cvar_value_before) - 1);
        r.cvar_value_before[sizeof(r.cvar_value_before) - 1] = '\0';
        found = true;
        break;
    }

    if (!found) {
        r.skipped = true;
        skips_value_unreadable++;
        WALKERCORE_LOG_WARN(
            "DevCameraCvarPatcher: skipped '%s' (could not discover the "
            "value field within +/- %u kB of name VA 0x%llX). The user "
            "must edit config.cfg manually: add `uset %s \"1\"` to "
            "Documents\\American Truck Simulator\\config.cfg and "
            "reload the save.",
            needle,
            static_cast<unsigned int>(kScanWindow / 1024),
            static_cast<unsigned long long>(name_va),
            needle);
        return false;
    }

    r.cvar_value_va = value_va;
    if (r.cvar_value_before[0] == '1') {
        // Already enabled. Short-circuit and remember so we don't
        // pay the scan cost on every subsequent call.
        skips_already++;
        r.skipped = true;
        r.patched = false;
        WALKERCORE_LOG_INFO(
            "DevCameraCvarPatcher: '%s' already enabled (value at VA "
            "0x%llX reads '1'). No write needed.",
            needle,
            static_cast<unsigned long long>(value_va));
        std::memcpy(r.cvar_value_after, r.cvar_value_before,
                    sizeof(r.cvar_value_after) - 1);
        r.cvar_value_after[sizeof(r.cvar_value_after) - 1] = '\0';
        return true;
    }

    if (r.cvar_value_before[0] != '0') {
        // '2' (max diagnostic reports) or anything else — leave
        // it alone but log.
        r.skipped = true;
        skips_value_unreadable++;
        WALKERCORE_LOG_INFO(
            "DevCameraCvarPatcher: skipped '%s' (cvar value at VA "
            "0x%llX reads '%c', not '0' — leaving as-is).",
            needle,
            static_cast<unsigned long long>(value_va),
            r.cvar_value_before[0]);
        return false;
    }

    // 3. Patch the value buffer from '0' to '1'.
    if (!PatchByte(reinterpret_cast<const void *>(value_va), '1')) {
        r.skipped = true;
        skips_protect_failed++;
        WALKERCORE_LOG_WARN(
            "DevCameraCvarPatcher: skipped '%s' (VirtualProtect "
            "failed for the page containing the cvar value buffer "
            "at VA 0x%llX). The user must edit config.cfg manually.",
            needle,
            static_cast<unsigned long long>(value_va));
        return false;
    }
    patches_total++;
    r.patched = true;
    // Capture the new value for the log line.
    char after[32] = {};
    SafeRead(reinterpret_cast<const void *>(value_va), after,
             sizeof(after) - 1);
    after[sizeof(after) - 1] = '\0';
    std::memcpy(r.cvar_value_after, after, sizeof(r.cvar_value_after) - 1);
    r.cvar_value_after[sizeof(r.cvar_value_after) - 1] = '\0';
    WALKERCORE_LOG_INFO(
        "DevCameraCvarPatcher: patched '%s' '0' -> '1' at VA 0x%llX "
        "(cvar name VA 0x%llX).",
        needle,
        static_cast<unsigned long long>(value_va),
        static_cast<unsigned long long>(name_va));
    return true;
}

}  // namespace

DevCameraCvarPatchResult DevCameraCvarPatcher::PatchOnce() {
    runs_++;
    DevCameraCvarPatchResult r{};
    DevCameraCvarPatchResult r_debug{};
    last_result_ = r;
    last_debug_camera_result_ = r_debug;

    if (already_patched_) {
        // The patch is one-shot per process. Both cvars reached
        // the "already on" or "patched" state on a prior call —
        // there is nothing to do, and re-scanning the data window
        // is wasted work. Both per-cvar Result structs report a
        // pure skip so the F12 dump surfaces the state without
        // misleading the analyst.
        r.skipped = true;
        r.patched = false;
        r.cvar_name_va = 0;
        r.cvar_value_va = 0;
        skips_already_patched_++;
        last_result_ = r;
        // Mirror the same skip into the secondary cvar's Result
        // struct so the dual-cvar F12 dump does not show stale
        // info from an earlier successful run.
        r_debug.skipped = true;
        r_debug.patched = false;
        r_debug.cvar_name_va = 0;
        r_debug.cvar_value_va = 0;
        last_debug_camera_result_ = r_debug;
        return r;
    }

    if (!enabled_) {
        r.skipped = true;
        skips_disabled_++;
        WALKERCORE_LOG_INFO(
            "DevCameraCvarPatcher: skipped (gate disabled, "
            "[m2] dev_cvar_patcher = false). F10 dev-camera paths "
            "will not toggle until g_developer is '1' in config.cfg.");
        last_result_ = r;
        return r;
    }

    const std::uint64_t base = module_base_for_testing_ != 0
                                   ? module_base_for_testing_
                                   : ResolveAmtrucksBase();
    if (base == 0) {
        r.skipped = true;
        skips_no_module_++;
        WALKERCORE_LOG_INFO(
            "DevCameraCvarPatcher: skipped (amtrucks.exe not loaded "
            "yet — the SCS plugin DllMain may have run before the "
            "game module was registered. Will retry next tick.)");
        last_result_ = r;
        return r;
    }

    // 1. Patch g_developer (master dev console switch).
    //    Whichever way this resolves (patched, already-on,
    //    skipped-unreadable, or signature-mismatch) we treat the
    //    process as having "tried" — the per-run scan cost is the
    //    expensive part of PatchOnce, and we never need to repeat
    //    it within the same process even if `g_debug_camera` later
    //    fails to resolve. The early-return at the top of this
    //    method guards any subsequent calls.
    (void)PatchOneCvar(
        "g_developer", name_rva_, base, r, patches_,
        skips_already_patched_, skips_signature_mismatch_,
        skips_value_unreadable_, skips_protect_failed_);
    last_result_ = r;
    already_patched_ = true;

    // 2. Patch g_debug_camera (the explicit dev free-camera unlock).
    //    The shim's `~`-toggleable dev free roam view requires this
    //    cvar too. If the cvar has moved or the page is unmapped, the
    //    helper logs a clean diagnostic and we continue — `g_developer`
    //    alone is enough for the dev console's `free_camera` command.
    //    The already_patched_ flag was set above so subsequent
    //    PatchOnce calls hit the early-return and never re-scan the
    //    secondary cvar either.
    (void)PatchOneCvar(
        "g_debug_camera", debug_camera_name_rva_, base, r_debug,
        patches_, skips_already_patched_, skips_signature_mismatch_,
        skips_value_unreadable_, skips_protect_failed_);
    last_debug_camera_result_ = r_debug;

    // Return the g_developer result for ABI stability (all existing
    // tests and the F12 dump read this field). The secondary cvar's
    // result is exposed through last_debug_camera_result().
    return r;
}

}  // namespace walkercore
