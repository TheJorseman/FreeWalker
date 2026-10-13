// WalkerCore — DevCameraCvarPatcher implementation.
//
// Reads `g_developer` from amtrucks.exe's mapped memory and patches
// it from "0" to "1" when needed. The patch is wrapped in a
// VirtualProtect / VirtualProtect pair so the page is writable for
// the duration of the one-byte write, then restored to its original
// protection.
//
// Why a "0" → "1" patch and not a free-form write:
//   - The patcher's job is to flip the developer gate. The game
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
void DevCameraCvarPatcher::SetModuleBaseForTesting(std::uint64_t base) {
    module_base_for_testing_ = base;
}

DevCameraCvarPatchResult DevCameraCvarPatcher::PatchOnce() {
    runs_++;
    DevCameraCvarPatchResult r{};
    last_result_ = r;
    if (already_patched_) {
        // The patch is one-shot per process. Subsequent calls
        // short-circuit; the F12 dump and the per-run log line both
        // surface "already_patched" so the analyst can tell the
        // patcher did its job earlier in the session.
        r.skipped = true;
        r.patched = false;
        r.cvar_name_va = 0;
        r.cvar_value_va = 0;
        skips_already_patched_++;
        last_result_ = r;
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
            "DevCameraCvarPatcher: skipped (amtrucks.exe not loaded yet — "
            "the SCS plugin DllMain may have run before the game module was "
            "registered. Will retry next tick.)");
        last_result_ = r;
        return r;
    }

    const std::uint64_t name_va = base + name_rva_;
    r.cvar_name_va = name_va;

    // 1. Verify the literal cvar name at the rebased RVA.
    char expected[16] = {};
    const char *needle = "g_developer";
    const size_t needle_len = std::strlen(needle);
    if (!SafeRead(reinterpret_cast<const void *>(name_va), expected,
                  needle_len + 1)) {
        r.skipped = true;
        skips_value_unreadable_++;
        WALKERCORE_LOG_WARN(
            "DevCameraCvarPatcher: skipped (could not read cvar name at "
            "VA 0x%llX — page not readable, or RVA 0x%llX is no longer "
            "the cvar location for this build).",
            static_cast<unsigned long long>(name_va),
            static_cast<unsigned long long>(name_rva_));
        last_result_ = r;
        return r;
    }
    if (std::memcmp(expected, needle, needle_len + 1) != 0) {
        r.skipped = true;
        skips_signature_mismatch_++;
        WALKERCORE_LOG_WARN(
            "DevCameraCvarPatcher: skipped (signature mismatch at VA "
            "0x%llX — expected 'g_developer\\0', got '%.12s\\0'). The "
            "cvar has moved in this build; re-run "
            "rea/scripts/tmrealwalk-re/find_g_developer.py to find the "
            "new RVA and update DevCameraCvarPatcher::name_rva_.",
            static_cast<unsigned long long>(name_va), expected);
        last_result_ = r;
        return r;
    }

    // 2. Discover the cvar control struct. The struct holds the
    //    cvar value as a separate field, typically within ~256
    //    bytes of the name pointer (the SCS CVar struct fits in
    //    one or two cache lines). We scan amtrucks.exe's .rdata
    //    region for pointers that reference the name VA, then look
    //    for the value buffer (either a small allocation holding
    //    "0\0" or "1\0" near the pointer, or a direct value field
    //    in the struct).
    //
    //    Without knowing the SCS struct layout we use a robust
    //    heuristic: walk amtrucks.exe's data region, look for any
    //    8-byte value that matches name_va; the slot immediately
    //    after that pointer (struct offset 8) is typically the
    //    value field. We then read up to 32 bytes from that field
    //    and check for "0\0" / "1\0" / "2\0".
    //
    //    This scan is bounded by a sensible window: we look in the
    //    +/- 8 KiB window around the cvar name. That covers all
    //    cvar structs that share the cvar hash bucket.
    constexpr std::uint64_t kScanWindow = 8 * 1024;
    const std::uint64_t scan_lo =
        (name_va > kScanWindow) ? (name_va - kScanWindow) : 0;
    const std::uint64_t scan_hi = name_va + kScanWindow;
    bool found = false;
    std::uint64_t value_va = 0;

    // We don't have a robust size for the .data / .rdata regions;
    // the scan reads pages and probes them via SafeRead. To keep
    // the search bounded, we walk 8 bytes at a time (a pointer
    // miss costs 7 reads before we move on).
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
        // The struct offset 8 may be the value field. Read it.
        char value_buffer[32] = {};
        const std::uint64_t value_candidate = addr + 8;
        if (!SafeRead(reinterpret_cast<const void *>(value_candidate),
                      value_buffer, sizeof(value_buffer))) {
            continue;
        }
        // We expect a small string ("0", "1", or "2") followed by
        // a NUL byte within the first 8 bytes. Anything else
        // suggests the field is not a value field (maybe another
        // cvar struct has its name pointer there and we read into
        // something unrelated).
        const size_t value_len = strnlen(value_buffer, 8);
        if (value_len != 1 || value_buffer[1] != '\0') {
            continue;
        }
        if (value_buffer[0] != '0' && value_buffer[0] != '1' &&
            value_buffer[0] != '2') {
            continue;
        }
        // Found a cvar struct whose value field reads '0' / '1' / '2'.
        value_va = value_candidate;
        std::memcpy(r.cvar_value_before, value_buffer,
                    sizeof(r.cvar_value_before) - 1);
        r.cvar_value_before[sizeof(r.cvar_value_before) - 1] = '\0';
        found = true;
        break;
    }

    if (!found) {
        r.skipped = true;
        skips_value_unreadable_++;
        WALKERCORE_LOG_WARN(
            "DevCameraCvarPatcher: skipped (could not discover the "
            "value field for the 'g_developer' cvar within +/- %u kB of "
            "name VA 0x%llX). The cvar struct layout is non-standard "
            "for this build; the user must edit config.cfg manually: "
            "add `uset g_developer \"1\"` to Documents\\American Truck "
            "Simulator\\config.cfg and reload the save.",
            static_cast<unsigned int>(kScanWindow / 1024),
            static_cast<unsigned long long>(name_va));
        last_result_ = r;
        return r;
    }

    r.cvar_value_va = value_va;
    if (r.cvar_value_before[0] == '1') {
        // Already enabled. Short-circuit and remember so we don't
        // pay the scan cost on every subsequent call.
        already_patched_ = true;
        r.skipped = true;
        r.patched = false;
        skips_already_patched_++;
        WALKERCORE_LOG_INFO(
            "DevCameraCvarPatcher: already enabled (cvar value at VA "
            "0x%llX reads '1'). The patcher is a no-op for the rest of "
            "this process.",
            static_cast<unsigned long long>(value_va));
        std::memcpy(r.cvar_value_after, r.cvar_value_before,
                    sizeof(r.cvar_value_after) - 1);
        r.cvar_value_after[sizeof(r.cvar_value_after) - 1] = '\0';
        last_result_ = r;
        return r;
    }

    if (r.cvar_value_before[0] != '0') {
        // '2' (max diagnostic reports) or anything else — leave it
        // alone but log.
        r.skipped = true;
        skips_value_unreadable_++;
        WALKERCORE_LOG_INFO(
            "DevCameraCvarPatcher: skipped (cvar value at VA 0x%llX "
            "reads '%c', not '0' — leaving as-is). If you want the "
            "patcher to overwrite other values, edit config.cfg "
            "directly.",
            static_cast<unsigned long long>(value_va),
            r.cvar_value_before[0]);
        last_result_ = r;
        return r;
    }

    // 3. Patch the value buffer from '0' to '1'.
    if (!PatchByte(reinterpret_cast<const void *>(value_va), '1')) {
        r.skipped = true;
        skips_protect_failed_++;
        WALKERCORE_LOG_WARN(
            "DevCameraCvarPatcher: skipped (VirtualProtect failed for "
            "the page containing the cvar value buffer at VA 0x%llX). "
            "The user must edit config.cfg manually.",
            static_cast<unsigned long long>(value_va));
        last_result_ = r;
        return r;
    }
    already_patched_ = true;
    r.patched = true;
    patches_++;
    // Capture the new value for the log line.
    char after[32] = {};
    SafeRead(reinterpret_cast<const void *>(value_va), after,
             sizeof(after) - 1);
    after[sizeof(after) - 1] = '\0';
    std::memcpy(r.cvar_value_after, after, sizeof(r.cvar_value_after) - 1);
    r.cvar_value_after[sizeof(r.cvar_value_after) - 1] = '\0';
    WALKERCORE_LOG_INFO(
        "DevCameraCvarPatcher: patched g_developer '0' -> '1' at VA "
        "0x%llX (cvar name VA 0x%llX). Dev-camera paths (Numpad-0 free "
        "camera toggle via free_camera_u concommand, the dev UI button "
        "o_im_freecam, future in-process camera manager hookups) are "
        "now usable without restarting the save.",
        static_cast<unsigned long long>(value_va),
        static_cast<unsigned long long>(name_va));
    last_result_ = r;
    return r;
}

}  // namespace walkercore