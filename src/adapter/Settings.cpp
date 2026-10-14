// WalkerCore — Settings implementation.

#include "walkercore/Settings.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#include "walkercore/FrameLoop.h"
#include "walkercore/Log.h"
#include "walkercore/PolishProviders.h"
#include "walkercore/Time.h"
#include "walkercore/WalkerFsm.h"

#if defined(_WIN32)
#include <shlobj.h>  // for SHGetFolderPathA (CSIDL_APPDATA)
#include <windows.h>
#endif

namespace walkercore {

namespace {

std::string Trim(const std::string &s) {
    const char *ws = " \t\r\n";
    const auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::string ToLower(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

WalkerSettings WalkerSettings::Defaults() {
    WalkerSettings s;
    s.movement = MovementConfig{};
    s.keys = KeyBindings{};
    s.door_config = DefaultTruckDoorConfig();
    s.head_tracker = HeadTrackerConfig{};
    return s;
}

std::string DefaultSettingsPath() {
#if defined(_WIN32)
    char appdata[MAX_PATH] = {0};
    // SHGetFolderPathA is deprecated in Vista+ but still works; we use it
    // here because the SDK headers target XP for some users. SHGetKnownFolderPath
    // requires <KnownFolders.h> which isn't always available with mingw.
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, appdata))) {
        std::string out = appdata;
        if (!out.empty() && out.back() != '\\') out += '\\';
        out += "WalkerCore\\WalkerCore.ini";
        return out;
    }
#endif
    // Fallback — write next to the DLL.
    return "WalkerCore.ini";
}

bool EnsureDirectoryExists(const std::string &path) {
    if (path.empty()) return false;
    // Find the last separator.
    const auto sep = path.find_last_of("\\/");
    if (sep == std::string::npos) return true;
    const std::string dir = path.substr(0, sep);
#if defined(_WIN32)
    // Walk each component and CreateDirectoryA if missing. We
    // ignore the return value here — CreateDirectoryA returns
    // ERROR_ALREADY_EXISTS for any directory already on disk, which
    // is the common case. The "could not create" detection is done
    // by checking GetFileAttributesA on the leaf below (covers the
    // "permission denied" / "invalid path" / "drive not mounted"
    // cases that the walker cannot recover from).
    std::string acc;
    for (size_t i = 0; i < dir.size(); ++i) {
        const char c = dir[i];
        acc.push_back(c);
        if (c == '\\' || c == '/' || i + 1 == dir.size()) {
            if (!acc.empty() && acc != "\\" && acc != "/") {
                CreateDirectoryA(acc.c_str(), nullptr);
            }
        }
    }
    // Verify the final directory exists and is actually a
    // directory. The SaveToPath caller treats a `false` return as
    // "could not create parent dir" so we need to be conservative
    // here: any non-directory result (file with the same path,
    // permission denied, INVALID_HANDLE_VALUE) returns false.
    const DWORD attrs = ::GetFileAttributesA(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        WALKERCORE_LOG_INFO(
            "Settings: EnsureDirectoryExists failed for %s (parent dir=%s "
            "missing; GetLastError=%lu) — subsequent SaveToPath will fail",
            path.c_str(), dir.c_str(), ::GetLastError());
        return false;
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        WALKERCORE_LOG_INFO(
            "Settings: EnsureDirectoryExists failed for %s (parent path=%s "
            "exists but is NOT a directory; GetLastError=%lu)",
            path.c_str(), dir.c_str(), ::GetLastError());
        return false;
    }
    return true;
#else
    (void)dir;
    return false;
#endif
}

bool TryParseKeyValue(const char *line, std::string *out_key,
                       std::string *out_value) {
    if (line == nullptr) return false;
    const std::string sline = Trim(line);
    if (sline.empty() || sline[0] == '#' || sline[0] == ';') return false;
    if (sline[0] == '[') return false;  // section header
    const auto eq = sline.find('=');
    if (eq == std::string::npos) return false;
    *out_key = Trim(sline.substr(0, eq));
    *out_value = Trim(sline.substr(eq + 1));
    // Strip inline comments after the value.
    const auto hash = out_value->find('#');
    if (hash != std::string::npos) {
        out_value->erase(hash);
    }
    const auto semi = out_value->find(';');
    if (semi != std::string::npos) {
        out_value->erase(semi);
    }
    *out_value = Trim(*out_value);
    return !out_key->empty();
}

bool TryParseFloat(const std::string &value, float *out) {
    if (out == nullptr || value.empty()) return false;
    try {
        *out = std::stof(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool TryParseDouble(const std::string &value, double *out) {
    if (out == nullptr || value.empty()) return false;
    try {
        *out = std::stod(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool TryParseInt(const std::string &value, int *out) {
    if (out == nullptr || value.empty()) return false;
    try {
        std::size_t parsed = 0;
        // stoi accepts a leading sign and digits, but it stops at the first
        // non-digit (e.g. "3.14" parses as 3 without throwing). Verify the
        // whole string was consumed so "3.14" / "1e3" are rejected here
        // (they belong in TryParseFloat).
        *out = std::stoi(value, &parsed);
        if (parsed != value.size()) return false;
        return true;
    } catch (...) {
        return false;
    }
}

bool TryParseBool(const std::string &value, bool *out) {
    if (out == nullptr) return false;
    const std::string l = ToLower(value);
    if (l == "1" || l == "true" || l == "yes" || l == "on") {
        *out = true;
        return true;
    }
    if (l == "0" || l == "false" || l == "no" || l == "off") {
        *out = false;
        return true;
    }
    return false;
}

SettingsStore::SettingsStore() {
    current_ = WalkerSettings::Defaults();
    path_ = DefaultSettingsPath();
}

SettingsStore::~SettingsStore() = default;

void SettingsStore::BindFrameLoop(FrameLoop *loop) {
    frame_loop_ = loop;
}

void SettingsStore::BindInput(InputHook *input) {
    input_ = input;
}

void SettingsStore::BindFsm(WalkerFsm *fsm) {
    fsm_ = fsm;
}

void SettingsStore::BindHeadTracker(HeadTrackerProvider *head) {
    head_tracker_ = head;
}

void SettingsStore::SetCurrent(const WalkerSettings &s) {
    current_ = s;
}

bool SettingsStore::LoadFromPath(const std::string &path) {
    // Even when the file is missing, set path_ so a subsequent
    // CheckFileChanged() stats the right file (and reports "missing" with
    // a clean false return). Before this fix, a failed LoadFromPath left
    // path_ at its previous value, so polling could read mtime from a
    // different file and falsely report a change.
    path_ = path;

    std::ifstream f(path);
    if (!f.is_open()) {
        return false;
    }

    // Read the entire file into memory so the parser can do look-ahead
    // for the M2.5 `# walkercore:managed:<keyname>` sentinel comments
    // that the SaveToPath / migrate paths emit right after every
    // auto-written key. The sentinel tells the loader "this key was
    // auto-written by the DLL (first-time defaults write or the
    // auto-migrate true→false flip), so the `*_explicit` flag should
    // NOT be set — otherwise the M2.5 smart-default auto-enable would
    // never flip the shim for users who upgraded from a pre-M2.5 DLL
    // where these lines were auto-migrated into their INI". The
    // sentinel is opt-in: lines that lack it are treated as
    // user-written (the previous behaviour, which is what we want for
    // every manually-edited INI).
    std::vector<std::string> raw_lines;
    {
        std::string line;
        while (std::getline(f, line)) {
            raw_lines.push_back(line);
        }
    }
    // First pass: collect every `# walkercore:managed:<keyname>` sentinel.
    // The key name in the sentinel is whatever the auto-write path
    // produced (always lowercase, matches the parser's normalised lkey).
    std::set<std::string> managed_keys;
    for (const std::string &l : raw_lines) {
        const std::string t = Trim(l);
        if (t.empty()) continue;
        // Sentinel shape: `# walkercore:managed:<keyname>` (keyname is a
        // single token — no spaces, no commas, no equals).
        constexpr const char *kPrefix = "# walkercore:managed:";
        if (t.rfind(kPrefix, 0) == 0) {
            std::string key = Trim(t.substr(strlen(kPrefix)));
            if (!key.empty()) {
                managed_keys.insert(ToLower(key));
            }
        }
    }

    WalkerSettings loaded = WalkerSettings::Defaults();
    std::string section;
    int unknown_count = 0;

    for (size_t idx = 0; idx < raw_lines.size(); ++idx) {
        const std::string &line = raw_lines[idx];
        const std::string tline = Trim(line);
        if (tline.empty() || tline[0] == '#' || tline[0] == ';') continue;
        if (tline.front() == '[' && tline.back() == ']') {
            section = ToLower(tline.substr(1, tline.size() - 2));
            continue;
        }

        std::string key, value;
        if (!TryParseKeyValue(line.c_str(), &key, &value)) continue;
        const std::string lkey = ToLower(key);
        // Helper: returns true iff the line IMMEDIATELY following this
        // one carries the auto-managed sentinel for `lkey`. The
        // sentinel is consumed by the next-iteration comment skip in
        // the loop above, so this is purely a parser-side flag for the
        // current `*_explicit` decision. The sentinel does NOT change
        // the parsed value — only the explicit flag.
        const bool auto_managed = (managed_keys.count(lkey) != 0);

        // Dispatch by section + key. We use lowercase to make the parser
        // tolerant of user formatting.
        if (section == "walker") {
            float fv = 0.0f;
            if (lkey == "walk_speed_mps" && TryParseFloat(value, &fv))
                loaded.movement.walkSpeedMps = fv;
            else if (lkey == "run_speed_mps" && TryParseFloat(value, &fv))
                loaded.movement.runSpeedMps = fv;
            else if (lkey == "crouch_speed_mps" && TryParseFloat(value, &fv))
                loaded.movement.crouchSpeedMps = fv;
            else if (lkey == "jump_initial_velocity_mps" && TryParseFloat(value, &fv))
                loaded.movement.jumpInitialVelocityMps = fv;
            else if (lkey == "gravity_mps2" && TryParseFloat(value, &fv))
                loaded.movement.gravityMps2 = fv;
            else if (lkey == "mouse_sensitivity" && TryParseFloat(value, &fv))
                loaded.movement.mouseSensitivityRadPerUnit = fv;
            else if (lkey == "eye_height_standing_m" && TryParseFloat(value, &fv))
                loaded.movement.eyeHeightStandingM = fv;
            else if (lkey == "eye_height_crouching_m" && TryParseFloat(value, &fv))
                loaded.movement.eyeHeightCrouchingM = fv;
            else if (lkey == "head_bob_vertical_amp_m" && TryParseFloat(value, &fv))
                loaded.movement.headBobVerticalAmpM = fv;
            else if (lkey == "head_bob_lateral_amp_m" && TryParseFloat(value, &fv))
                loaded.movement.headBobLateralAmpM = fv;
            else if (lkey == "head_bob_frequency_hz" && TryParseFloat(value, &fv))
                loaded.movement.headBobFrequencyHz = fv;
            else if (lkey == "max_step_up_m" && TryParseFloat(value, &fv))
                loaded.movement.maxStepUpM = fv;
            else if (lkey == "max_slope_angle_rad" && TryParseFloat(value, &fv))
                loaded.movement.maxSlopeAngleRad = fv;
            else if (lkey == "horizontal_fov_radians" && TryParseFloat(value, &fv))
                loaded.horizontal_fov_radians = fv;
            else if (lkey == "near_z" && TryParseFloat(value, &fv))
                loaded.near_z = fv;
            else if (lkey == "far_z" && TryParseFloat(value, &fv))
                loaded.far_z = fv;
            else if (lkey == "transition_seconds" && TryParseFloat(value, &fv))
                loaded.transition_seconds = fv;
            else
                unknown_count++;
        } else if (section == "keys") {
            int iv = 0;
            if (lkey == "forward_vk" && TryParseInt(value, &iv))
                loaded.keys.forward_vk = iv;
            else if (lkey == "backward_vk" && TryParseInt(value, &iv))
                loaded.keys.backward_vk = iv;
            else if (lkey == "strafe_left_vk" && TryParseInt(value, &iv))
                loaded.keys.strafe_left_vk = iv;
            else if (lkey == "strafe_right_vk" && TryParseInt(value, &iv))
                loaded.keys.strafe_right_vk = iv;
            else if (lkey == "run_vk" && TryParseInt(value, &iv))
                loaded.keys.run_vk = iv;
            else if (lkey == "crouch_vk" && TryParseInt(value, &iv))
                loaded.keys.crouch_vk = iv;
            else if (lkey == "jump_vk" && TryParseInt(value, &iv))
                loaded.keys.jump_vk = iv;
            else if (lkey == "interact_vk" && TryParseInt(value, &iv))
                loaded.keys.interact_vk = iv;
            else if (lkey == "toggle_walk_vk" && TryParseInt(value, &iv))
                loaded.keys.toggle_walk_vk = iv;
            else if (lkey == "flashlight_vk" && TryParseInt(value, &iv))
                loaded.keys.flashlight_vk = iv;
            else if (lkey == "eye_up_vk" && TryParseInt(value, &iv))
                loaded.keys.eye_up_vk = iv;
            else if (lkey == "eye_down_vk" && TryParseInt(value, &iv))
                loaded.keys.eye_down_vk = iv;
            else if (lkey == "eye_reset_vk" && TryParseInt(value, &iv))
                loaded.keys.eye_reset_vk = iv;
            else if (lkey == "dump_state_vk" && TryParseInt(value, &iv))
                loaded.keys.dump_state_vk = iv;
            else if (lkey == "zoom_mouse_button" && TryParseInt(value, &iv))
                loaded.keys.zoom_mouse_button = iv;
            else if (lkey == "flashlight_toggle_mouse_button" && TryParseInt(value, &iv))
                loaded.keys.flashlight_toggle_mouse_button = iv;
            else
                unknown_count++;
        } else if (section == "head_tracker") {
            // Source enable flags + per-source sensitivities + smoothing.
            // Defaults leave every source off (no double-count with the
            // FrameLoop's mouse-look path).
            bool bv = false;
            float fv = 0.0f;
            if (lkey == "use_mouse" && TryParseBool(value, &bv))
                loaded.head_tracker.use_mouse = bv;
            else if (lkey == "use_trackir" && TryParseBool(value, &bv))
                loaded.head_tracker.use_trackir = bv;
            else if (lkey == "use_opentrack" && TryParseBool(value, &bv))
                loaded.head_tracker.use_opentrack = bv;
            else if (lkey == "mouse_sensitivity_rad_per_unit" && TryParseFloat(value, &fv))
                loaded.head_tracker.mouse_sensitivity_rad_per_unit = fv;
            else if (lkey == "trackir_sensitivity_rad_per_unit" && TryParseFloat(value, &fv))
                loaded.head_tracker.trackir_sensitivity_rad_per_unit = fv;
            else if (lkey == "opentrack_sensitivity_rad_per_unit" && TryParseFloat(value, &fv))
                loaded.head_tracker.opentrack_sensitivity_rad_per_unit = fv;
            else if (lkey == "smoothing_factor" && TryParseFloat(value, &fv))
                loaded.head_tracker.smoothing_factor = fv;
            else
                unknown_count++;
        } else if (section == "m2") {
            // M2 render-push configuration. Currently the only knob is
            // `game_free_camera_shim`, a 0/1 boolean that turns on the
            // SendInput-based fallback that toggles the SCS developer
            // free camera when F10 transitions Driving → Walking.
            bool bv = false;
            if (lkey == "game_free_camera_shim" && TryParseBool(value, &bv)) {
                loaded.m2_game_free_camera_shim = bv;
                // M2.5 follow-up: even if the value happens to equal
                // the default false, the user has explicitly named the
                // knob in the INI — set the explicit flag so the
                // PluginEntry's smart-default auto-enable (when a
                // dev_camera_toggle binding is detected in
                // controls.sii AND g_developer is supported) is
                // skipped. The explicit-vs-default distinction matters
                // because auto-enable would otherwise flip a value
                // the user has explicitly vetted (and may have set to
                // false on purpose).
                //
                // M2.5 hotfix: skip the flag if the line was
                // auto-written by the DLL. The sentinel comment
                // `# walkercore:managed:game_free_camera_shim` is
                // emitted by SaveToPath (first-time defaults write)
                // and by the migrate block (true→false flip). Without
                // this exception, a user upgrading from a pre-M2.5 DLL
                // ends up with `game_free_camera_shim = false` + the
                // explicit flag set in their INI, and the smart-default
                // never fires even when the gates pass. Users who
                // deliberately write the line themselves (no sentinel)
                // still get the explicit flag — only auto-writes are
                // exempt.
                if (!auto_managed) {
                    loaded.m2_game_free_camera_shim_explicit = true;
                }
            } else if (lkey == "camera_hijack" && TryParseBool(value, &bv)) {
                // M2 long-term: MinHook the camera state dispatcher at
                // RVA 0xC22F40 and replace the matrices the SCS state
                // machine writes to the GPU constant buffer. Default 0;
                // opt in explicitly.
                loaded.m2_camera_hijack = bv;
            } else if (lkey == "d3d11_hook" && TryParseBool(value, &bv)) {
                // M2 lander (D3D11 hook) install gate. Default 0. Set
                // `true` to opt back in on a known-clean stack.
                loaded.m2_d3d11_hook = bv;
            } else if (lkey == "d3d12_hook" && TryParseBool(value, &bv)) {
                // M2 lander (D3D12 + DXGI factory hook) install gate.
                // Default 0. Set `true` to opt back in on a known-clean
                // stack.
                loaded.m2_d3d12_hook = bv;
            } else if (lkey == "dev_cvar_patcher" && TryParseBool(value, &bv)) {
                // M2 lander: enable the dev-camera cvar patcher.
                // Default `true` so the patcher is in effect out of
                // the box; the shim and the dev UI can rely on
                // g_developer=1 in memory after the first
                // scs_telemetry_init call.
                loaded.m2_dev_cvar_patcher = bv;
            } else if (lkey == "dev_camera_toggle_vk") {
                // M2: virtual key the shim synthesises on every
                // Walking entry. The M2.7 default is VK_F11 (0x7A)
                // — the SCS dev free camera toggle in
                // amtrucks.exe 1.61.3.1s, intercepted before the
                // action registry (no controls.sii binding
                // required). Requires `g_developer=1` AND
                // `g_console=1` in config.cfg. Live-verified Oct
                // 2026: pressing F11 with both cvars set opens the
                // dev free-roam view with position overlay.
                // Historical defaults: pre-M2.6 was VK_NUMPAD0
                // (0x60, RE guess), M2.6 was VK_0 (0x30, top-row
                // zero, RE guess), M2.7 is VK_F11 (0x7A, verified).
                // Users whose controls.sii or mod stack binds F11
                // to a non-camera action can re-target via
                // `[m2] dev_camera_toggle_vk = 0xHH` in this INI
                // (e.g. F8 = 0x77 to avoid collision with
                // `mix camdbg \`keyboard.f11?0\``). Accepts decimal
                // (e.g. 122 = VK_F11) or 0xHH (0x7A). Out-of-range
                // values fall back to the new default VK_F11 and
                // emit a one-shot warning so a typo in the INI is
                // not silently accepted.
                char *end = nullptr;
                const unsigned long parsed =
                    strtoul(value.c_str(), &end, 0);
                if (end != value && *end == '\0' && parsed <= 0xFF) {
                    loaded.m2_dev_camera_toggle_vk = static_cast<unsigned int>(parsed);
                    // M2.5: even if the value happens to equal the
                    // default 0x7A, the user has explicitly named the
                    // knob in the INI — set the explicit flag so the
                    // controls.sii auto-detect is skipped. The
                    // explicit-vs-default distinction matters because
                    // auto-detect would otherwise try to "improve" a
                    // value the user has already vetted.
                    //
                    // M2.5 hotfix: skip the flag if the line was
                    // auto-written by the DLL (the sentinel
                    // `# walkercore:managed:dev_camera_toggle_vk` was
                    // emitted). Same rationale as for
                    // game_free_camera_shim — pre-M2.5 DLLs wrote
                    // `dev_camera_toggle_vk = 0x60` and M2.6/M2.7a
                    // DLLs wrote `0x30` on every SaveToPath, so
                    // without this exemption the controls.sii
                    // auto-detect is dead on upgrade.
                    if (!auto_managed) {
                        loaded.m2_dev_camera_toggle_vk_explicit = true;
                    }
                } else {
                    WALKERCORE_LOG_WARN(
                        "Settings: [m2] dev_camera_toggle_vk='%s' is not a "
                        "valid VK code (decimal 0..255 or 0xHH); keeping default "
                        "VK_F11 (0x7A).", value);
                }
            } else if (lkey == "camera_hijack_install" && TryParseBool(value, &bv)) {
                // M2 long-term: in-process MinHook on the SCS camera
                // state dispatcher at RVA 0xC22F40 in amtrucks.exe.
                // Default 0 — the trampoline has been observed to
                // break downstream 16-byte alignment on ATS 1.61.3.1s
                // (movdqa #GP at RVA 0x1d5300a). Set `true` on a
                // known-clean stack.
                loaded.m2_camera_hijack_install = bv;
            } else {
                unknown_count++;
            }
        } else if (section == "m3") {
            // M3 lander gates. Currently a single knob: the
            // PxScene::addActor MinHook install. Default 0 — the
            // trampoline has been observed to break downstream
            // 16-byte alignment on ATS 1.61.3.1s.
            bool bv = false;
            if (lkey == "px_scene_hook_install" && TryParseBool(value, &bv)) {
                loaded.m3_px_scene_hook_install = bv;
            } else {
                unknown_count++;
            }
        } else if (section == "m11") {
            // M11 ImGui overlay gate. `imgui_overlay = true` turns
            // on the overlay rendering. The D3D11 Present-path
            // integration landed in 5a68662: D3D11Hook's
            // HookedPresent calls RenderImGuiFrameIfEnabled() after
            // the per-frame tick, the overlay class creates the
            // ImGui context on device capture, and the rendering
            // surfaces the live walker state (camera / player /
            // FSM) on top of the game's swap chain. The INI gate
            // here is the user opt-in.
            bool bv = false;
            if (lkey == "imgui_overlay" && TryParseBool(value, &bv)) {
                loaded.m11_imgui_overlay = bv;
            } else {
                unknown_count++;
            }
        } else {
            unknown_count++;
        }
    }

    if (unknown_count > 0) {
        WALKERCORE_LOG_WARN(
            "Settings: %d unknown key/value pairs ignored in %s",
            unknown_count, path.c_str());
    }
    WALKERCORE_LOG_INFO("Settings: loaded %s", path.c_str());
    current_ = loaded;

    // Auto-migrate the M2 D3D hook gates to safe defaults. The 0c109d1
    // lander shipped with `d3d11_hook = true` and `d3d12_hook = true`
    // as defaults; a user who just upgraded to a build with
    // `d3d11_hook = false` and `d3d12_hook = false` defaults still has
    // the old values persisted in their INI file from a prior save.
    // The file overrides the code default, so without this migration
    // the user would still hit the amtrucks.exe+0x1d5300a `movdqa` #GP
    // after the upgrade. The fix: if either gate is still `true`
    // after the file parse, force to `false`, log a one-shot WARNING
    // that names the path, and save the file back so the next load is
    // clean. The opt-in path remains `[m2] d3d11_hook = true` for
    // users on a known-clean stack — they will have to delete the
    // WalkerCore.ini file (so the code defaults win on a fresh load)
    // or accept that this auto-migration re-runs every time the file
    // is loaded. The migration is idempotent and cheap (one boolean
    // compare + one SaveToPath on the upgrade load only).
    //
    // M2.5 hotfix: the migrate runs regardless of the explicit flag
    // for `game_free_camera_shim` specifically — old DLLs did not
    // emit the `# walkercore:managed:game_free_camera_shim` sentinel,
    // so we cannot distinguish a user who deliberately wrote
    // `= true` from a user whose old DLL wrote it as the default.
    // The migrate fires once, saves the file with the sentinel
    // (emitted by SaveToPath), and from that load onward the smart-
    // default can flip the value. Users who genuinely want to keep
    // `= true` on a clean stack can delete the line from their INI
    // entirely (no line + no sentinel = no migrate) or set the value
    // to something non-default that survives the migrate round-trip.
    // The migrate fires only when the on-disk value is `true` —
    // once forced to `false`, the second load sees `false` and the
    // migrate is a no-op (the sentinel persists the not-explicit
    // status).
    bool migrated = false;
    if (current_.m2_game_free_camera_shim &&
        (current_.m2_dev_camera_toggle_vk == 0x30u ||
         current_.m2_dev_camera_toggle_vk == 0x60u)) {
        // The shim is currently no-op for the stock ATS 1.61.x
        // profile when paired with a legacy VK: 0x30 (top-row
        // '0') was the M2.6/M2.7a default and is NOT the SCS dev
        // free camera toggle in amtrucks.exe 1.61.3.1s; 0x60
        // (Numpad-0) is bound to `dbglockpos` (debug lock
        // position) in the stock profile, not to the dev free
        // camera toggle. The dev free camera toggle that ACTUALLY
        // works in this build is VK_F11 (0x7A): SCS intercepts
        // F11 before the action registry when g_developer=1 AND
        // g_console=1 are set in config.cfg. With any non-legacy
        // VK the shim is a useful feature (the F10 Walking entry
        // -> F11 SendInput path verified live Oct 2026), so the
        // migration only fires for the two known-broken legacy
        // defaults. Users who want the shim paired with a legacy
        // VK must bind the corresponding key to the dev free
        // camera action in their active profile's controls.sii
        // AND have a clean DI8 keyboard pipeline (no ReShade +
        // OptiScaler + RenoDX + DXVK mod stack on top of DXGI
        // exclusive fullscreen). Flip back to false so the shim
        // doesn't spam dispatch log lines that accomplish
        // nothing.
        WALKERCORE_LOG_WARN(
            "Settings: [m2] game_free_camera_shim was true; forcing "
            "to false (the stock ATS 1.61.x profile binds Numpad-0 "
            "to `dbglockpos`, not to the dev free camera toggle — "
            "the shim dispatches SendInput Numpad-0 which the game "
            "ignores. Bind Numpad-0 to the dev free camera action "
            "in your active profile's controls.sii AND set "
            "[m2] game_free_camera_shim = true in this INI to opt "
            "back in).");
        current_.m2_game_free_camera_shim = false;
        migrated = true;
    }
    // M2.7 hotfix: one-shot in-memory migration of `dev_camera_toggle_vk`
    // whose value still reads as a legacy default. Two historical
    // defaults to cover:
    //   * 0x30 = VK_0 (top-row '0') — M2.6 / M2.7a build's default.
    //     RE-confirmed NOT to be the SCS dev free camera toggle in
    //     amtrucks.exe 1.61.3.1s per user live-verify in Oct 2026
    //     (VK_F11 / 0x7A IS the effective toggle).
    //   * 0x60 = VK_NUMPAD0 — pre-M2.6 default. Routes through the
    //     regular interior-camera cycle on this build.
    //
    // The migration fires only when `*_explicit` is false (the
    // `# walkercore:managed:dev_camera_toggle_vk` sentinel is present,
    // meaning the value was auto-written by an older DLL version with
    // a now-legacy default). Users who hand-wrote a legacy value into
    // their INI (no sentinel) keep their pick — the explicit flag
    // gates the migration so a user who genuinely wants VK_0 or
    // VK_NUMPAD0 is not silently flipped to F11.
    //
    // The sentinel-pollution fix: a pre-M2.7a DLL wrote the value
    // matching its compile-time default (0x30 for M2.6/M2.7a) AND
    // emitted the sentinel. After the default changed to 0x7A, the
    // sentinel-pinned 0x30 would otherwise be stranded forever —
    // no fresh DLL load would migrate it because sentinel = "user
    // knows what they want". This block heals that case: sentinel
    // present + legacy value → migrate to the new default.
    //
    // Fires exactly once per INI lifetime because the post-migration
    // value matches the new code default; PatchSentinelsForLegacyIni
    // below re-emits the sentinel so future loads stay suppressed
    // from explicit.
    if (!current_.m2_dev_camera_toggle_vk_explicit &&
        (current_.m2_dev_camera_toggle_vk == 0x30u ||
         current_.m2_dev_camera_toggle_vk == 0x60u)) {
        const unsigned int old_vk = current_.m2_dev_camera_toggle_vk;
        current_.m2_dev_camera_toggle_vk = 0x7Au;
        WALKERCORE_LOG_WARN(
            "Settings: [m2] dev_camera_toggle_vk was 0x%X (legacy default); "
            "migrating to 0x7A (VK_F11, the verified SCS dev free camera "
            "toggle on amtrucks.exe 1.61.3.1s with g_developer=1 AND "
            "g_console=1). Live verification Oct 2026: F11 opens the dev "
            "free-roam view with position overlay; top-row '0' (0x30) "
            "and Numpad-0 (0x60) do nothing in this build.",
            old_vk);
        migrated = true;
    }
    if (current_.m2_d3d11_hook) {
        WALKERCORE_LOG_WARN(
            "Settings: [m2] d3d11_hook was true; forcing to false (ATS "
            "1.61.3.1s + ReShade + OptiScaler + RenoDX mod stack causes "
            "a downstream `movdqa` #GP at amtrucks.exe+0x1d5300a when the "
            "MinHook install path is live). Edit the INI and remove the "
            "auto-fix only on a known-clean stack.");
        current_.m2_d3d11_hook = false;
        migrated = true;
    }
    if (current_.m2_d3d12_hook) {
        WALKERCORE_LOG_WARN(
            "Settings: [m2] d3d12_hook was true; forcing to false (same "
            "ATS 1.61.3.1s + ReShade + OptiScaler + RenoDX rationale as "
            "d3d11_hook). Edit the INI to re-enable on a known-clean "
            "stack.");
        current_.m2_d3d12_hook = false;
        migrated = true;
    }
    if (current_.m2_camera_hijack_install) {
        WALKERCORE_LOG_WARN(
            "Settings: [m2] camera_hijack_install was true; forcing to "
            "false. The MinHook trampoline on the SCS camera state "
            "dispatcher (RVA 0xC22F40) has been observed to break "
            "downstream 16-byte alignment on ATS 1.61.3.1s — same "
            "amtrucks.exe+0x1d5300a `movdqa` #GP as the d3d11/d3d12 "
            "hooks but triggered by the in-process trampoline. Edit "
            "the INI to re-enable on a known-clean stack.");
        current_.m2_camera_hijack_install = false;
        migrated = true;
    }
    if (current_.m3_px_scene_hook_install) {
        WALKERCORE_LOG_WARN(
            "Settings: [m3] px_scene_hook_install was true; forcing to "
            "false. The MinHook trampoline on PxScene::addActor (RVA "
            "0x1AE0B62) has been observed to break downstream 16-byte "
            "alignment on ATS 1.61.3.1s — same amtrucks.exe+0x1d5300a "
            "`movdqa` #GP as the d3d11/d3d12 / camera-hijack hooks. "
            "Edit the INI to re-enable on a known-clean stack.");
        current_.m3_px_scene_hook_install = false;
        migrated = true;
    }
    if (migrated) {
        SaveToPath(path);
    }

    // M2.5 hotfix: in-place sentinel patch for legacy INIs. Users who
    // upgraded from a pre-M2.5 DLL have `game_free_camera_shim = false`
    // (and/or `dev_camera_toggle_vk = 0x60` from pre-M2.6 DLLs, or
    // `= 0x30` from M2.6/M2.7a DLLs — historical defaults that were
    // hard-coded before the user-verified switch to VK_F11 / 0x7A)
    // in their INI without the `# walkercore:managed:<key>`
    // sentinel — those DLLs did not emit the sentinel. Without it,
    // the loader treats the line as user-written and sets
    // `*_explicit = true`, which blocks the M2.5 smart-default
    // auto-enable. The sentinel is the loader's only signal that
    // the line was auto-written by an old DLL or a first-time
    // defaults save — there is no other way to distinguish.
    //
    // The patch is in-place (line-level insert, not full-file rewrite)
    // so it preserves every user-customised key the SaveToPath path
    // does not know about. It fires at most once per key per INI
    // lifetime: once the sentinel is present, the load no longer
    // marks the key as needing a patch. If the value is non-default
    // (e.g. user pinned `= true` or `dev_camera_toggle_vk = 0x77`
    // for F8), the patch is skipped — that line is user-vetted, not
    // auto.
    PatchSentinelsForLegacyIni(path);

    // M2 diagnostic: per-key source classification. After every
    // INI load, emit one line that names every managed M2 key
    // (the keys the smart-default + sentinel machinery governs),
    // the value the parser resolved, and the source the value
    // came from. The three possible sources are:
    //   - "default"      — value matches the compile-time default
    //                      AND no sentinel was present AND no
    //                      migrate ran; the parser fell back to the
    //                      Defaults() initializer.
    //   - "managed"      — value matches the compile-time default
    //                      AND a sentinel is present; the DLL
    //                      auto-wrote the line (first save / legacy
    //                      patch). The `*_explicit` flag is OFF and
    //                      smart-default may flip the value on the
    //                      next load.
    //   - "user-explicit"— value matches the compile-time default
    //                      AND no sentinel is present (line was
    //                      hand-edited by the user with the default
    //                      value); OR value differs from the
    //                      default (the user picked something
    //                      non-default). The `*_explicit` flag is
    //                      ON and the smart-default is OFF.
    //
    // The classify data is what the analyst needs when the user
    // reports "smart-default did not flip my shim on" — the line
    // shows whether the key was sentinel-managed or user-locked
    // at the time of load.
    //
    // The diagnostic is gated by a single log call so the per-key
    // detail stays in one line. The format is grep-friendly so the
    // analyst can scan `g.log` for `Settings: per-key resolution`
    // and see every managed key on one click.
    WALKERCORE_LOG_INFO(
        "Settings: per-key resolution path=%s shim="
        "value=%s,explicit=%s sentinel=%s; "
        "dev_camera_toggle_vk="
        "value=0x%X,explicit=%s sentinel=%s; "
        "game_free_camera_shim-managed=%s; "
        "dev_camera_toggle_vk-managed=%s",
        path.c_str(),
        current_.m2_game_free_camera_shim ? "true" : "false",
        current_.m2_game_free_camera_shim_explicit ? "true" : "false",
        managed_keys.count("game_free_camera_shim") != 0 ? "yes" : "no",
        current_.m2_dev_camera_toggle_vk,
        current_.m2_dev_camera_toggle_vk_explicit ? "true" : "false",
        managed_keys.count("dev_camera_toggle_vk") != 0 ? "yes" : "no",
        managed_keys.count("game_free_camera_shim") != 0 ? "yes" : "no",
        managed_keys.count("dev_camera_toggle_vk") != 0 ? "yes" : "no");

    ApplyAll();

    // Prime the mtime cache so the very next CheckFileChanged() call does
    // NOT falsely report "file changed" (we just loaded it — the user's
    // edits arrive later, with a strictly newer mtime). Without this,
    // every DLL load wastes a Settings reload + a spurious "detected change"
    // log line on the first per-frame tick.
    PrimeMtimeCache();
    return true;
}

// M2.5 hotfix: in-place sentinel patch for legacy INIs. Walks every
// line in the file. For each `key = value` line whose key is in the
// M2.5-managed set (`game_free_camera_shim` or `dev_camera_toggle_vk`)
// and whose value matches the code default, appends a
// `# walkercore:managed:<key>` comment line directly below it. The
// loader uses this sentinel to suppress the `*_explicit` flag, which
// is what unblocks the smart-default auto-enable for users whose INI
// was written by a pre-M2.5 DLL (those DLLs did not emit the
// sentinel). The patch is idempotent (no-op if the sentinel is
// already present) and preserves every other line including any
// user-customised keys that the SaveToPath full-rewrite path would
// drop. The patch is run after the migrate block has had a chance
// to fire — the sentinel-aware SaveToPath emits the sentinel on the
// migrate path, but the migrate only runs when the value is `true`;
// the patch covers the rest (the value is already at default).
void SettingsStore::PatchSentinelsForLegacyIni(const std::string &path) {
    // We only patch the two M2.5-managed keys. Each entry is the key
    // name (lowercase, matches the parser's normalised lkey) plus the
    // expected default value as a string. The default-value string
    // match is what lets us decide whether to emit the sentinel —
    // non-default values are user-vetted and must be left alone.
    struct Target {
        const char *key;
        const char *default_value_str;  // expected on-disk value at default
    };
    static const Target kTargets[] = {
        {"game_free_camera_shim", "false"},
        {"dev_camera_toggle_vk", "0x7a"},
    };

    // Read the entire file into memory. If the file does not exist
    // (e.g. a partial install), there is nothing to patch — the
    // next first-time defaults write will emit the sentinel for us.
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::vector<std::string> lines;
    {
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
    }

    // Build a set of sentinels already present so we know which keys
    // still need patching. The sentinel survives across edits — once
    // emitted, the loader no longer flags the key as explicit, and
    // future runs short-circuit the patch on the same file.
    std::set<std::string> already_managed;
    for (const std::string &l : lines) {
        const std::string t = Trim(l);
        constexpr const char *kPrefix = "# walkercore:managed:";
        if (t.rfind(kPrefix, 0) == 0) {
            already_managed.insert(ToLower(Trim(t.substr(strlen(kPrefix)))));
        }
    }

    // Walk every line, identifying target key=value lines that are
    // (a) at the default value, and (b) missing their sentinel. For
    // each such line, append a sentinel comment immediately after it.
    // We record insertion points in (line_index, sentinel_text) pairs
    // and apply them in reverse order so the indices stay valid.
    struct Insertion {
        size_t line_index;       // insert AFTER this index
        std::string sentinel;
    };
    std::vector<Insertion> insertions;
    std::string current_section;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = Trim(lines[i]);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            current_section = ToLower(t.substr(1, t.size() - 2));
            continue;
        }
        // Only the [m2] section is in scope for the M2.5-managed
        // sentinel. Other sections do not contain these keys.
        if (current_section != "m2") continue;
        std::string key, value;
        if (!TryParseKeyValue(lines[i].c_str(), &key, &value)) continue;
        const std::string lkey = ToLower(key);
        const std::string lvalue = ToLower(Trim(value));
        for (const Target &tgt : kTargets) {
            if (lkey != tgt.key) continue;
            if (lvalue != tgt.default_value_str) continue;
            if (already_managed.count(lkey) != 0) continue;
            insertions.push_back({i, std::string("# walkercore:managed:") + lkey});
            // Mark as already-managed so a duplicate target entry
            // (defensive — kTargets has unique keys today) does not
            // insert twice. Breaks because each target key only
            // appears once in the file.
            already_managed.insert(lkey);
            break;
        }
    }

    if (insertions.empty()) return;

    // Apply insertions in reverse so earlier indices stay valid.
    std::sort(insertions.begin(), insertions.end(),
              [](const Insertion &a, const Insertion &b) {
                  return a.line_index > b.line_index;
              });
    for (const Insertion &ins : insertions) {
        lines.insert(lines.begin() +
                  static_cast<std::ptrdiff_t>(ins.line_index) + 1,
              ins.sentinel);
    }

    // Write the patched file back. Use atomic-ish open + write so a
    // crash mid-write does not leave a half-written INI.
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        WALKERCORE_LOG_WARN(
            "Settings: sentinel patch could not rewrite %s — smart-default "
            "may stay blocked until you manually add the sentinel",
            path.c_str());
        return;
    }
    for (const std::string &l : lines) {
        out << l << "\n";
    }
    WALKERCORE_LOG_INFO(
        "Settings: patched %zu sentinel(s) into %s — pre-M2.5 INI upgraded "
        "in-place so smart-default can fire on the next load",
        insertions.size(), path.c_str());
}

bool SettingsStore::LoadFromDefaultPath() {
    return LoadFromPath(DefaultSettingsPath());
}

bool SettingsStore::SaveToPath(const std::string &path) const {
    if (!EnsureDirectoryExists(path)) {
        // M2 diagnostic: the previous one-line "cannot create
        // parent dir" message was useless for the analyst trying to
        // figure out why the INI save silently dropped. Surface
        // every signal available: the parent dir, whether it
        // exists at all, what the last Win32 error was, whether
        // the path is read-only (an installer may have set the
        // read-only attribute on the AppData folder), and the
        // current process token privilege for write. The
        // GetFileAttributesA + FILE_ATTRIBUTE_READONLY + GetLastError
        // check covers the "permission denied", "path doesn't
        // exist", "drive not mounted", and "read-only volume"
        // failure modes that previously left the user guessing.
        const auto sep_pos = path.find_last_of("\\/");
        const std::string parent_dir =
            sep_pos == std::string::npos ? std::string("<no-separator>")
                                          : path.substr(0, sep_pos);
        const DWORD attrs = ::GetFileAttributesA(parent_dir.c_str());
        const DWORD gle = ::GetLastError();
        const bool parent_exists = (attrs != INVALID_FILE_ATTRIBUTES);
        const bool parent_readonly =
            parent_exists && (attrs & FILE_ATTRIBUTE_READONLY);
        WALKERCORE_LOG_WARN(
            "Settings: cannot create parent dir of %s (parent_dir=%s "
            "exists=%s readonly=%s GetLastError=%lu). The settings "
            "menu will keep the user's edits in memory but the "
            "next DLL load will see the previous values. Check "
            "that the AppData target is writable and not on a "
            "read-only volume.",
            path.c_str(), parent_dir.c_str(),
            parent_exists ? "yes" : "no",
            parent_readonly ? "yes" : "no",
            gle);
        return false;
    }
    std::ofstream f(path);
    if (!f.is_open()) {
        // M2 diagnostic: capture the file-open failure context.
        // The previous "cannot open for write" line left the
        // analyst guessing — was it a readonly file from an old
        // install, a transient AV scanner holding the file, a
        // broken junction, or a permissions problem? We surface
        // whether the file exists, whether it is read-only, the
        // last Win32 error, and the parent dir's writable state.
        const DWORD file_attrs = ::GetFileAttributesA(path.c_str());
        const DWORD gle = ::GetLastError();
        const bool file_exists =
            (file_attrs != INVALID_FILE_ATTRIBUTES);
        const bool file_readonly =
            file_exists && (file_attrs & FILE_ATTRIBUTE_READONLY);
        const auto sep_pos = path.find_last_of("\\/");
        const std::string parent_dir =
            sep_pos == std::string::npos ? std::string("<no-separator>")
                                          : path.substr(0, sep_pos);
        const DWORD parent_attrs = ::GetFileAttributesA(parent_dir.c_str());
        const bool parent_readonly =
            (parent_attrs != INVALID_FILE_ATTRIBUTES) &&
            (parent_attrs & FILE_ATTRIBUTE_READONLY);
        WALKERCORE_LOG_WARN(
            "Settings: cannot open %s for write (file_exists=%s "
            "file_readonly=%s parent_readonly=%s GetLastError=%lu). "
            "Check AV quarantine, file ownership, and ACL write "
            "permissions.",
            path.c_str(),
            file_exists ? "yes" : "no",
            file_readonly ? "yes" : "no",
            parent_readonly ? "yes" : "no",
            gle);
        return false;
    }
    f << "# WalkerCore configuration file. Generated by the plugin; safe to "
         "edit while the game is running — changes are picked up within "
         "a few seconds via the polling reload.\n";
    f << "\n";
    f << "[walker]\n";
    f << "walk_speed_mps = " << current_.movement.walkSpeedMps << "\n";
    f << "run_speed_mps = " << current_.movement.runSpeedMps << "\n";
    f << "crouch_speed_mps = " << current_.movement.crouchSpeedMps << "\n";
    f << "jump_initial_velocity_mps = " << current_.movement.jumpInitialVelocityMps << "\n";
    f << "gravity_mps2 = " << current_.movement.gravityMps2 << "\n";
    f << "mouse_sensitivity = " << current_.movement.mouseSensitivityRadPerUnit << "\n";
    f << "eye_height_standing_m = " << current_.movement.eyeHeightStandingM << "\n";
    f << "eye_height_crouching_m = " << current_.movement.eyeHeightCrouchingM << "\n";
    f << "head_bob_vertical_amp_m = " << current_.movement.headBobVerticalAmpM << "\n";
    f << "head_bob_lateral_amp_m = " << current_.movement.headBobLateralAmpM << "\n";
    f << "head_bob_frequency_hz = " << current_.movement.headBobFrequencyHz << "\n";
    f << "max_step_up_m = " << current_.movement.maxStepUpM << "\n";
    f << "max_slope_angle_rad = " << current_.movement.maxSlopeAngleRad << "\n";
    f << "horizontal_fov_radians = " << current_.horizontal_fov_radians << "\n";
    f << "near_z = " << current_.near_z << "\n";
    f << "far_z = " << current_.far_z << "\n";
    f << "transition_seconds = " << current_.transition_seconds << "\n";
    f << "\n";
    f << "[keys]\n";
    f << "forward_vk = " << current_.keys.forward_vk << "    # 'W' = 87\n";
    f << "backward_vk = " << current_.keys.backward_vk << "    # 'S' = 83\n";
    f << "strafe_left_vk = " << current_.keys.strafe_left_vk << "    # 'A' = 65\n";
    f << "strafe_right_vk = " << current_.keys.strafe_right_vk << "    # 'D' = 68\n";
    f << "run_vk = " << current_.keys.run_vk << "    # VK_SHIFT = 16\n";
    f << "crouch_vk = " << current_.keys.crouch_vk << "    # VK_CONTROL = 17\n";
    f << "jump_vk = " << current_.keys.jump_vk << "    # VK_SPACE = 32\n";
    f << "interact_vk = " << current_.keys.interact_vk << "    # 'F' = 70\n";
    f << "toggle_walk_vk = " << current_.keys.toggle_walk_vk << "    # VK_F10 = 121\n";
    f << "flashlight_vk = " << current_.keys.flashlight_vk << "    # 'G' = 71\n";
    f << "eye_up_vk = " << current_.keys.eye_up_vk << "    # 'E' = 69\n";
    f << "eye_down_vk = " << current_.keys.eye_down_vk << "    # 'Q' = 81\n";
    f << "eye_reset_vk = " << current_.keys.eye_reset_vk << "    # 'R' = 82\n";
    // dump_state_vk: defaults to VK_F12 (123). NOTE — ATS / ETS2 own
    // F12 as the in-game screenshot key (the game captures via its
    // own WndProc, not Windows Game Bar). If you want a dump key that
    // does NOT collide, set this to VK_F11 (122), VK_INSERT (45), or
    // VK_NUMLOCK (144). The dump output is written to game.log.txt
    // anyway, so prefer a non-screenshot key here.
    f << "dump_state_vk = " << current_.keys.dump_state_vk << "    # VK_F12 = 123 (collides with ATS in-game screenshot)\n";
    f << "zoom_mouse_button = " << current_.keys.zoom_mouse_button << "    # 0 = VK_LBUTTON\n";
    f << "flashlight_toggle_mouse_button = " << current_.keys.flashlight_toggle_mouse_button << "    # 1 = VK_RBUTTON\n";
    f << "\n";
    f << "[head_tracker]\n";
    f << "# Source enable flags. All default to off to avoid double-counting\n";
    f << "# with the FrameLoop's mouse-look path. TrackIR / opentrack will\n";
    f << "# have real source feeds in the M7 lander.\n";
    f << "use_mouse = " << (current_.head_tracker.use_mouse ? "true" : "false") << "\n";
    f << "use_trackir = " << (current_.head_tracker.use_trackir ? "true" : "false") << "\n";
    f << "use_opentrack = " << (current_.head_tracker.use_opentrack ? "true" : "false") << "\n";
    f << "mouse_sensitivity_rad_per_unit = " << current_.head_tracker.mouse_sensitivity_rad_per_unit << "\n";
    f << "trackir_sensitivity_rad_per_unit = " << current_.head_tracker.trackir_sensitivity_rad_per_unit << "\n";
    f << "opentrack_sensitivity_rad_per_unit = " << current_.head_tracker.opentrack_sensitivity_rad_per_unit << "\n";
    f << "smoothing_factor = " << current_.head_tracker.smoothing_factor << "\n";
    f << "\n";
    f << "[m2]\n";
    f << "# Game-Free-Camera shim (MVP for the M2 GPU render push). When 1,\n";
    f << "# F10 (Driving -> Walking transition) also synthesises a F11\n";
    f << "# keypress via SendInput so the SCS developer free camera takes\n";
    f << "# over (requires `uset g_developer \"1\"` AND `uset g_console\n";
    f << "# \"1\"` in the game's config.cfg). Default 0 so out-of-the-box\n";
    f << "# behaviour stays unchanged.\n";
    f << "game_free_camera_shim = " << (current_.m2_game_free_camera_shim ? "true" : "false") << "\n";
    // M2.5 hotfix: emit the `# walkercore:managed:<key>` sentinel ONLY
    // when the value matches the code default. The sentinel tells the
    // loader "this was auto-written by the DLL, not by the user" so the
    // `m2_game_free_camera_shim_explicit` flag is not set — without this,
    // a user upgrading from a pre-M2.5 DLL has the line auto-migrated to
    // `= false` in their INI and the M2.5 smart-default never fires. If
    // the user changes the value to `true` (non-default), the sentinel
    // is omitted too — the absence is the loader's signal that the line
    // is user-vetted. Users who later flip back to `false` would re-arm
    // the sentinel on the next settings-menu save; this is an accepted
    // trade-off — the smart-default only flips when binding +
    // g_developer are also detected, and a user who wants to truly pin
    // `= false` can delete the sentinel comment manually.
    if (!current_.m2_game_free_camera_shim) {
        f << "# walkercore:managed:game_free_camera_shim\n";
    }
    f << "camera_hijack = " << (current_.m2_camera_hijack ? "true" : "false") << "\n";
    f << "d3d11_hook = " << (current_.m2_d3d11_hook ? "true" : "false") << "\n";
    f << "d3d12_hook = " << (current_.m2_d3d12_hook ? "true" : "false") << "\n";
    f << "camera_hijack_install = " << (current_.m2_camera_hijack_install ? "true" : "false") << "\n";
    f << "dev_cvar_patcher = " << (current_.m2_dev_cvar_patcher ? "true" : "false") << "\n";
    f << "# Virtual key the shim synthesises on every Walking entry.\n";
    f << "# M2.7 default: VK_F11 (0x7A) — the SCS dev free camera\n";
    f << "# toggle in amtrucks.exe 1.61.3.1s, intercepted before\n";
    f << "# the action registry (no controls.sii binding required).\n";
    f << "# Requires `g_developer=1` AND `g_console=1` in config.cfg.\n";
    f << "# Live-verified Oct 2026: pressing F11 with both cvars\n";
    f << "# set opens the dev free-roam view with position overlay.\n";
    f << "# Earlier builds used VK_NUMPAD0 (0x60, pre-M2.6) and\n";
    f << "# VK_0 (0x30, M2.6/M2.7a); live verification in Oct 2026\n";
    f << "# showed neither toggles the dev free camera in this build.\n";
    f << "# Users whose controls.sii or mod stack binds F11 to a\n";
    f << "# non-camera action can re-point the shim to a different\n";
    f << "# VK here (e.g. F8 = 0x77 to avoid `camdbg` collisions).\n";
    f << "# Accepts decimal or 0xHH. Out-of-range values fall back\n";
    f << "# to the default VK_F11 (0x7A).\n";
    f << "dev_camera_toggle_vk = 0x" << std::hex << current_.m2_dev_camera_toggle_vk << std::dec << "\n";
    // Same auto-managed sentinel as above — controls.sii auto-detect is
    // dead if the explicit flag is set, and the default-value line is
    // what pre-M2.7a DLLs wrote. The migrate block above snaps any
    // sentinel-pinned legacy value (0x30 or 0x60) to 0x7A so we should
    // never reach this branch holding a non-default value here.
    if (current_.m2_dev_camera_toggle_vk == 0x7Au) {
        f << "# walkercore:managed:dev_camera_toggle_vk\n";
    }
    f << "\n";
    f << "[m3]\n";
    f << "# PxScene::addActor MinHook install gate. Default 0 — the\n";
    f << "# trampoline has been observed to break downstream 16-byte\n";
    f << "# alignment on ATS 1.61.3.1s (movdqa #GP at RVA 0x1d5300a).\n";
    f << "# Set to 1 on a known-clean stack.\n";
    f << "px_scene_hook_install = " << (current_.m3_px_scene_hook_install ? "true" : "false") << "\n";
    f.close();
    WALKERCORE_LOG_INFO("Settings: saved %s", path.c_str());
    return true;
}

bool SettingsStore::SaveToDefaultPath() const {
    return SaveToPath(DefaultSettingsPath());
}

void SettingsStore::ApplyAll() {
    if (frame_loop_ != nullptr) {
        frame_loop_->SetMovementConfig(current_.movement);
    }
    if (input_ != nullptr) {
        input_->SetBindings(current_.keys);
    }
    if (fsm_ != nullptr) {
        fsm_->SetDoorConfig(current_.door_config);
        fsm_->SetTransitionSeconds(static_cast<double>(current_.transition_seconds));
    }
    if (head_tracker_ != nullptr) {
        head_tracker_->SetConfig(current_.head_tracker);
    }
}

bool SettingsStore::CheckFileChanged() {
#if defined(_WIN32)
    // Throttle: skip the syscall + mtime compare if we polled the file
    // less than kCheckPollingIntervalUs ago. The per-frame conductor
    // (PluginEntry's walker_per_frame_tick) calls us at 60 Hz; without
    // this we'd hit GetFileAttributesExA 60×/sec, which is wildly more
    // often than the user can possibly save the INI. With this, we
    // poll at ~4 Hz, well within human-perceptible latency on edit.
    //
    // The throttle window is intentionally wider than a single frame
    // (250 ms vs 16.7 ms at 60 Hz) so that under heavy CPU load the
    // budget doesn't double-fire if the conductor calls twice with a
    // long delay between them. last_check_us_ starts at 0 so the very
    // first call always polls.
    const uint64_t now = walkercore::NowMicros();
    if (now - last_check_us_ < kCheckPollingIntervalUs) return false;
    last_check_us_ = now;

    if (path_.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path_.c_str(), GetFileExInfoStandard, &fad)) {
        // File missing — treat as "no change".
        return false;
    }
    // Encode mtime as a single uint64 (100-ns intervals since 1601). Plenty
    // of resolution for "did the user save the file in the last second?".
    const uint64_t ft =
        (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
        static_cast<uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
    if (ft == last_mtime_us_) return false;
    last_mtime_us_ = ft;
    WALKERCORE_LOG_INFO("Settings: detected change to %s, reloading", path_.c_str());
    return LoadFromPath(path_);
#else
    return false;
#endif
}

void SettingsStore::ResetLastCheckTimerForTesting() {
    // Clears the throttle so the very next CheckFileChanged() does an
    // actual syscall + mtime compare. Production code never calls this;
    // tests use it to drive multiple meaningful polls in rapid
    // succession without sleeping for the throttle window to expire.
    last_check_us_ = 0;
}

void SettingsStore::PrimeMtimeCache() {
#if defined(_WIN32)
    if (path_.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path_.c_str(), GetFileExInfoStandard, &fad)) {
        // Missing file — leave the cache at its current value (0 on a fresh
        // store). The next CheckFileChanged() will also see "no file" and
        // return false, so this is internally consistent.
        return;
    }
    last_mtime_us_ =
        (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
        static_cast<uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
#endif
}

}  // namespace walkercore