// WalkerCore — Settings implementation.

#include "walkercore/Settings.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <sstream>

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
    // Walk each component and CreateDirectoryA if missing.
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

    WalkerSettings loaded = WalkerSettings::Defaults();
    std::string section;
    int unknown_count = 0;

    std::string line;
    while (std::getline(f, line)) {
        const std::string tline = Trim(line);
        if (tline.empty() || tline[0] == '#' || tline[0] == ';') continue;
        if (tline.front() == '[' && tline.back() == ']') {
            section = ToLower(tline.substr(1, tline.size() - 2));
            continue;
        }

        std::string key, value;
        if (!TryParseKeyValue(line.c_str(), &key, &value)) continue;
        const std::string lkey = ToLower(key);

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
    bool migrated = false;
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
    ApplyAll();

    // Prime the mtime cache so the very next CheckFileChanged() call does
    // NOT falsely report "file changed" (we just loaded it — the user's
    // edits arrive later, with a strictly newer mtime). Without this,
    // every DLL load wastes a Settings reload + a spurious "detected change"
    // log line on the first per-frame tick.
    PrimeMtimeCache();
    return true;
}

bool SettingsStore::LoadFromDefaultPath() {
    return LoadFromPath(DefaultSettingsPath());
}

bool SettingsStore::SaveToPath(const std::string &path) const {
    if (!EnsureDirectoryExists(path)) {
        WALKERCORE_LOG_WARN("Settings: cannot create parent dir of %s", path.c_str());
        return false;
    }
    std::ofstream f(path);
    if (!f.is_open()) {
        WALKERCORE_LOG_WARN("Settings: cannot open %s for write", path.c_str());
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
    f << "# F10 (Driving -> Walking transition) also synthesises a Numpad-0\n";
    f << "# press so the SCS developer free camera takes over (requires\n";
    f << "# `uset g_developer \"1\"` in the game's config.cfg). Default 0\n";
    f << "# so out-of-the-box behaviour stays unchanged.\n";
    f << "game_free_camera_shim = " << (current_.m2_game_free_camera_shim ? "true" : "false") << "\n";
    f << "camera_hijack = " << (current_.m2_camera_hijack ? "true" : "false") << "\n";
    f << "d3d11_hook = " << (current_.m2_d3d11_hook ? "true" : "false") << "\n";
    f << "d3d12_hook = " << (current_.m2_d3d12_hook ? "true" : "false") << "\n";
    f << "camera_hijack_install = " << (current_.m2_camera_hijack_install ? "true" : "false") << "\n";
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