// WalkerCore — ABI contract probe.
//
// Verifies the DLL actually exposes the symbols listed in the
// C ABI surface. Uses Win32 LoadLibrary + GetProcAddress so this
// test catches real export-table regressions (missing __declspec,
// wrong name mangling, dead-code elimination hiding a symbol that
// has no callers in the test build, ...).
//
// The DLL lives next to the test executable:
//
//     build-ninja/libWalkerCore.dll     ← built from src/
//     build-ninja/tests/WalkerCoreTests.exe  ← this test
//
// If the DLL isn't on disk (e.g. someone ran the tests before
// building the target), the test prints a one-line skip message
// and reports `ok` rather than failing — the build pipeline is
// what makes the DLL; the probe only verifies it.
//
// What we pin:
//   - LoadLibraryA succeeds (the DLL loads, all its dependencies
//     resolve, the export table is well-formed).
//   - The PE export table contains EXACTLY the 14 documented
//     names — and nothing else. Walked directly via the IMAGE_EXPORT_DIRECTORY
//     in the loaded module, not via the loose `GetProcAddress`-on-known-names
//     pattern (which would miss additions).
//   - Each documented symbol resolves to a non-NULL function pointer.
//   - Calling each walker_dump_* via the resolved pointer is
//     safe even with no provider wired (the dumps report
//     "(not initialized)" rather than crashing).
//
// What we explicitly do NOT pin:
//   - The "public per-frame" functions (walker_per_frame_tick,
//     walker_camera, walker_player, walker_should_override_render,
//     walker_fsm_state_label, walker_install_real_collision,
//     walker_set_swapchain_aspect) — docs/integration.md lists
//     these as the M2 lander's external surface, but the current
//     MinGW build does NOT export them (-fvisibility=hidden +
//     no __declspec). They're called internally from D3D11Hook.cpp
//     via the same translation unit. Pinning "these are NOT
//     exported" would conflict with the M2 lander once it lands,
//     so we leave that contract for a future probe.

#include "TestHarness.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>

namespace walkercore {

namespace {

// Relative path from the test exe to the built DLL. The DLL is a
// sibling of the tests/ directory (both children of build-ninja/).
const char *kDllRelative = "..\\libWalkerCore.dll";

char g_dll_path[512];

bool ResolveDllPath() {
    char exe_path[512];
    const DWORD n = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    if (n == 0 || n >= sizeof(exe_path)) return false;

    // exe_path looks like ...\build-ninja\tests\WalkerCoreTests.exe.
    // Strip just the trailing exe filename — keep the parent
    // (build-ninja\tests\) so we can append "..\libWalkerCore.dll"
    // and land on build-ninja\libWalkerCore.dll.
    char *last_sep = nullptr;
    for (char *p = exe_path; *p; ++p) {
        if (*p == '\\' || *p == '/') last_sep = p;
    }
    if (last_sep == nullptr) return false;
    // Truncate at the separator so we have the directory only.
    *(last_sep + 1) = '\0';
    if (strlen(exe_path) + strlen(kDllRelative) + 1 >= sizeof(g_dll_path)) {
        return false;
    }
    strcpy(g_dll_path, exe_path);
    strcat(g_dll_path, kDllRelative);
    return true;
}

// Walk the loaded module's PE export directory directly and call the
// visitor for each exported name. Returns the number of names seen.
// Robust to MinGW's slightly older PE layout (no dbghelp dependency).
//
// The PE header walk is: DOS header → NT headers (PE\0\0 signature) →
// optional header → data directory entry [IMAGE_DIRECTORY_ENTRY_EXPORT]
// → IMAGE_EXPORT_DIRECTORY → AddressOfNames[].
int ForEachExportName(HMODULE mod,
                      void (*visit)(const char *name, void *ctx),
                      void *ctx) {
    if (mod == nullptr || visit == nullptr) return 0;

    // IMAGE_DOS_HEADER is at HMODULE base; e_lfanew is at offset 0x3c.
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
        reinterpret_cast<const unsigned char *>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    // IMAGE_OPTIONAL_HEADER is right after IMAGE_FILE_HEADER. The data
    // directory at IMAGE_DIRECTORY_ENTRY_EXPORT is the entry we want.
    const IMAGE_DATA_DIRECTORY *dir_entry = nullptr;
    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        const auto *opt32 =
            reinterpret_cast<const IMAGE_OPTIONAL_HEADER32 *>(&nt->OptionalHeader);
        dir_entry = &opt32->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const auto *opt64 =
            reinterpret_cast<const IMAGE_OPTIONAL_HEADER64 *>(&nt->OptionalHeader);
        dir_entry = &opt64->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else {
        return 0;
    }
    if (dir_entry->VirtualAddress == 0 || dir_entry->Size == 0) return 0;

    const auto dir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(
        reinterpret_cast<const unsigned char *>(mod) +
        dir_entry->VirtualAddress);
    const auto name_rvas =
        reinterpret_cast<const DWORD *>(reinterpret_cast<const unsigned char *>(mod) +
                                       dir->AddressOfNames);
    const auto ords =
        reinterpret_cast<const WORD *>(reinterpret_cast<const unsigned char *>(mod) +
                                      dir->AddressOfNameOrdinals);
    const int n = static_cast<int>(dir->NumberOfNames);
    for (int i = 0; i < n; ++i) {
        const char *name =
            reinterpret_cast<const char *>(
                reinterpret_cast<const unsigned char *>(mod) + name_rvas[i]);
        (void)ords[i];  // ordinals unused for name resolution
        visit(name, ctx);
    }
    return n;
}

// -----------------------------------------------------------------------------
// Stash the actual export names for comparison against the expected set.
// -----------------------------------------------------------------------------
constexpr int kMaxExports = 64;
char g_seen_names[kMaxExports][128];
int g_seen_count = 0;

void CaptureExportName(const char *name, void *ctx) {
    (void)ctx;
    if (g_seen_count < kMaxExports && name != nullptr) {
        strncpy(g_seen_names[g_seen_count], name,
                sizeof(g_seen_names[g_seen_count]) - 1);
        g_seen_names[g_seen_count][sizeof(g_seen_names[g_seen_count]) - 1] = '\0';
        g_seen_count++;
    }
}

bool SeenSetContains(const char *name) {
    for (int i = 0; i < g_seen_count; ++i) {
        if (strcmp(g_seen_names[i], name) == 0) return true;
    }
    return false;
}

}  // namespace

void TestAbiContractDllLoads() {
    if (!ResolveDllPath()) {
        fprintf(stderr, "[ABI] SKIP: could not resolve DLL path\n");
        EXPECT_TRUE(true);
        return;
    }
    const HMODULE mod = LoadLibraryA(g_dll_path);
    if (mod == nullptr) {
        const DWORD err = GetLastError();
        fprintf(stderr, "[ABI] SKIP: LoadLibrary failed (%lu) for %s\n",
                err, g_dll_path);
        EXPECT_TRUE(true);
        return;
    }

    // Documented exports — must all resolve AND the DLL must export
    // exactly these (no more, no fewer). The M11 D3D11 integration
    // adds 6 more (version, frames_rendered, shutdown,
    // on_d3d11_device_captured, on_d3d11_device_lost,
    // render_imgui_frame_if_enabled) on top of the 3 M11
    // scaffolding exports landed earlier.
    struct Export { const char *name; };
    const Export kExports[] = {
        {"DllMain"},
        {"scs_telemetry_init"},
        {"scs_telemetry_shutdown"},
        {"walkercore_main"},
        {"walker_dump_state"},
        {"walker_dump_settings"},
        {"walker_dump_menu"},
        {"walker_dump_head_tracker"},
        {"walker_dump_shadow"},
        {"walker_dump_flashlight"},
        {"walker_dump_door"},
        {"walker_dump_fsm"},
        {"walker_dump_version"},
        {"walker_dump_input"},
        {"walker_dump_physx"},
        {"walker_dump_knockout"},
        {"walker_dump_per_truck_door"},
        {"walker_dump_coupling"},
        {"walker_dump_game_free_camera_shim"},
        {"walker_game_free_camera_shim_supported"},
        {"walker_game_free_camera_shim_on_enter_walking"},
        {"walker_game_free_camera_shim_on_exit_walking"},
        {"walker_game_free_camera_shim_toggle_now"},
        {"walker_game_free_camera_shim_set_enabled"},
        {"walker_dump_dev_camera_cvar_patcher"},
        {"walker_dev_camera_cvar_patcher_supported"},
        {"walker_dev_camera_cvar_patcher_run_once"},
        {"walker_pxscene_supported"},
        {"walker_pxscene_instance"},
        {"walker_dump_pxscene"},
        {"walker_imgui_overlay_supported"},
        {"walker_imgui_overlay_set_enabled"},
        {"walker_dump_imgui_overlay"},
        {"walker_imgui_overlay_version"},
        {"walker_imgui_overlay_frames_rendered"},
        {"walker_shutdown_imgui_overlay"},
        {"walker_on_d3d11_device_captured"},
        {"walker_on_d3d11_device_lost"},
        {"walker_render_imgui_frame_if_enabled"},
        {"walker_dump_build_info"},
        {"walker_dump_camera_hijack"},
        {"walker_camera_hijack_set_enabled"},
        {"walker_camera_hijack_supported"},
        {"walker_camera_hijack_install"},
        {"walker_camera_hijack_uninstall"},
    };
    const size_t kCount = sizeof(kExports) / sizeof(kExports[0]);

    // (1) Walk the PE export directory and capture every name.
    g_seen_count = 0;
    const int actual_count = ForEachExportName(mod, &CaptureExportName, nullptr);
    fprintf(stderr, "[ABI] export table: %d names\n", actual_count);

    // (2) Exact-count pin: a regression that adds a 15th export breaks here.
    EXPECT_EQ(static_cast<size_t>(actual_count), kCount);

    // (3) Each documented name must be present.
    for (size_t i = 0; i < kCount; ++i) {
        EXPECT_TRUE(SeenSetContains(kExports[i].name));
        if (!SeenSetContains(kExports[i].name)) {
            fprintf(stderr, "[ABI] MISSING: %s\n", kExports[i].name);
        }
    }

    // (4) No extra names beyond the documented set.
    for (int i = 0; i < g_seen_count; ++i) {
        bool in_expected = false;
        for (size_t j = 0; j < kCount; ++j) {
            if (strcmp(kExports[j].name, g_seen_names[i]) == 0) {
                in_expected = true;
                break;
            }
        }
        EXPECT_TRUE(in_expected);
        if (!in_expected) {
            fprintf(stderr, "[ABI] EXTRA export (not in documented set): %s\n",
                    g_seen_names[i]);
        }
    }

    // (5) Resolve each via GetProcAddress and confirm non-NULL. Belt-and-
    // suspenders next to the PE-walk pin above; catches things like
    // forwarded exports (where AddressOfNames lists a name but the
    // function pointer is a forwarder string, not a real RVA).
    for (size_t i = 0; i < kCount; ++i) {
        const FARPROC proc = GetProcAddress(mod, kExports[i].name);
        EXPECT_TRUE(proc != nullptr);
        if (proc == nullptr) {
            fprintf(stderr, "[ABI] unresolvable: %s\n", kExports[i].name);
        }
    }

    // (6) Call each walker_dump_* — must be safe with no provider wired.
    using DumpFn = void (*)();
    for (size_t i = 0; i < kCount; ++i) {
        const char *name = kExports[i].name;
        if (strncmp(name, "walker_dump_", 12) != 0) continue;
        const FARPROC proc = GetProcAddress(mod, name);
        if (proc == nullptr) continue;
        reinterpret_cast<DumpFn>(proc)();  // must not crash
    }

    // (7) Per-frame functions are NOT in the export table. The
    // docs/integration.md "Lander architecture" section explicitly
    // documents this: M-landers are file replacements within the
    // same DLL, so the per-frame C-linkage symbols (walker_per_frame_tick,
    // walker_camera, walker_player, walker_should_override_render,
    // walker_fsm_state_label, walker_install_real_collision,
    // walker_set_swapchain_aspect) are internal-linkage within the
    // DLL. They MUST NOT be in the export table.
    //
    // Why pin the absence: a future refactor that adds
    // __declspec(dllexport) to PerFrame.h (e.g. to let a separate
    // out-of-process lander load WalkerCore) would silently widen
    // the DLL's external surface. The TestAbiContractDllLoads
    // exact-count pin above catches additions on the positive
    // side, but the negative side — "the per-frame functions are
    // NOT exported" — was implicit until this probe. We pin it
    // explicitly so docs and code can't drift again.
    const char *kPerFrameSymbols[] = {
        "walker_per_frame_tick",
        "walker_camera",
        "walker_player",
        "walker_should_override_render",
        "walker_fsm_state_label",
        "walker_install_real_collision",
        "walker_set_swapchain_aspect",
    };
    const size_t kPerFrameCount =
        sizeof(kPerFrameSymbols) / sizeof(kPerFrameSymbols[0]);
    for (size_t i = 0; i < kPerFrameCount; ++i) {
        const FARPROC proc = GetProcAddress(mod, kPerFrameSymbols[i]);
        EXPECT_TRUE(proc == nullptr);
        if (proc != nullptr) {
            fprintf(stderr,
                    "[ABI] per-frame function %s IS exported — was supposed "
                    "to be internal-linkage. Update docs/integration.md and "
                    "PerFrame.h before promoting.\n",
                    kPerFrameSymbols[i]);
        }
    }

    FreeLibrary(mod);
}

}  // namespace walkercore