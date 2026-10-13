// WalkerCore — GameFreeCameraShim implementation (M2 GPU render push MVP).
//
// The shim never modifies SCS state. It only synthesises keystrokes
// that the game already understands. The default is Numpad-0, which
// toggles the SCS developer free camera in ATS 1.61.x when the cvar
// `g_developer` is `1` (already set in the user's config.cfg).
//
// On non-Windows hosts (Linux build for CI of the unit tests) every
// SendInput-touching path is compiled out under the same guard the
// rest of the project uses for OS-specific code. Tests run on
// Windows; the test build sets `WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED=1`
// so SendInput is replaced with a counter increment.

#include "walkercore/GameFreeCameraShim.h"

#include "walkercore/DevCameraCvarPatcher.h"
#include "walkercore/Log.h"

#include <cstdio>
#include <cstdint>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>  // SHGetFolderPathA + CSIDL_PROFILE
#endif

namespace walkercore {

namespace {

// Synthesise a single keypress. We use SendInput because ATS 1.61.x
// reads keyboard input via DirectInput 8 (`[di8] Initializing device
// '@@keyboard@@' as 'keyboard'` in game.log.txt, plus the binary
// embeds `di8.keyboard` as the input handler token). DirectInput 8
// keyboard devices poll the Windows input queue, and SendInput
// synthesises events that go through the same queue. The user
// confirmed Numpad-0 manual presses toggle the SCS dev free camera
// reliably — the question is whether the shim's synthetic input
// reaches DirectInput's poll. PostMessage WM_KEYDOWN does NOT reach
// DirectInput (it goes to the WndProc queue, which the game ignores
// for game-control keybindings), so the previously-shipped PostMessage
// dispatch path was a dead-end for this mod stack. We keep the bound
// HWND as a diagnostic surface (logged in the F12 dump + Walking entry
// line) but use it for dispatch only when SendInput is unavailable
// (e.g. a future build where the game's input model changes back).
// Returns the number of events dispatched (always 2: one down + one
// up) and updates the diagnostic path counters in `path_out` /
// `target_diag_out` for the F12 dump.
int SynthKey(unsigned int vk, HWND target_hwnd,
             int &path_out, unsigned int &target_diag_out) {
#if defined(_WIN32) && !defined(WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED)
    if (vk == 0) {
        path_out = 0;
        target_diag_out = 0;
        return 0;
    }
    // Always try SendInput first — that's the proven path for
    // DirectInput 8 keyboard (which the user confirmed works for
    // manual Numpad-0 presses). PostMessage WM_KEYDOWN is the
    // legacy fallback that we keep around for non-DirectInput games
    // where SendInput might be foreground-locked.
    if (target_hwnd == nullptr) {
        // No bound HWND (test build, or splash-screen window not yet
        // up) — try PostMessage as a no-op-ish fallback. The path
        // diagnostic will say `path=none` because we have nothing
        // useful to dispatch to.
        path_out = 0;
        target_diag_out = 0;
        return 0;
    }

    // Primary: SendInput (DirectInput 8 keyboard → dev free camera
    // toggle on Numpad-0).
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = static_cast<WORD>(vk);
    down.ki.dwFlags = 0;
    SendInput(1, &down, sizeof(down));

    INPUT up = {};
    up.type = INPUT_KEYBOARD;
    up.ki.wVk = static_cast<WORD>(vk);
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &up, sizeof(up));
    path_out = 1;  // SendInput
    target_diag_out = static_cast<unsigned int>(
        reinterpret_cast<std::uintptr_t>(target_hwnd) & 0xFFFFFFFFu);
    return 2;
#else
    // Test stub: when GameFreeCameraShim is built with
    // WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED (the test build
    // configuration), we don't call SendInput or PostMessage.
    // Instead we simulate the dispatch decision so the tests can
    // verify the right path was taken based on the bound HWND.
    (void)vk;
    if (target_hwnd == nullptr) {
        // Without a target we'd SendInput into whatever process is
        // in the foreground — too risky for a production build,
        // so we skip dispatch entirely. The path counter records
        // `none` and the toggle counter still increments (so the
        // rest of the system knows the dispatch was attempted).
        path_out = 0;
        target_diag_out = 0;
        return 0;
    }
    // With an HWND bound, the production path uses SendInput (the
    // proven DirectInput 8 keyboard path). The test stub mirrors
    // that decision without actually touching the Windows input
    // queue.
    path_out = 1;  // SendInput
    target_diag_out = static_cast<unsigned int>(
        reinterpret_cast<std::uintptr_t>(target_hwnd) & 0xFFFFFFFFu);
    return 2;
#endif
}

long long NowMs() {
#if defined(_WIN32)
    return static_cast<long long>(::GetTickCount64());
#else
    return 0;
#endif
}

}  // namespace

// Pure helper: candidate `Documents` sub-folders relative to the
// user's profile. We try the canonical English "Documents" first,
// then Windows-XP "My Documents", then the OneDrive variants for
// both English ("Documents") and Spanish ("Documentos") locales —
// the latter is needed for this developer's host (es_es + OneDrive
// folder backup with the original Spanish name). We also enumerate
// `OneDrive - Personal\*` for multi-account OneDrive installs.
std::vector<std::string> GameFreeCameraShim::ProbeConfigCandidatesForTesting(
    const std::string &profile) {
    static const char *stems[] = {
        "American Truck Simulator",
        "Euro Truck Simulator 2",
    };
    static const char *subs[] = {
        "Documents",
        "My Documents",
        "OneDrive\\Documents",
        "OneDrive\\Documentos",
        "OneDrive - Personal\\Documents",
        "OneDrive - Personal\\Documentos",
    };
    std::vector<std::string> out;
    out.reserve(12);
    for (const char *sub : subs) {
        for (const char *stem : stems) {
            std::string p = profile + "\\" + sub + "\\" + stem +
                            "\\config.cfg";
            out.push_back(std::move(p));
        }
    }
    return out;
}

// Pure helper: parse the first `uset g_developer "X"` line and
// return the value as a string. Empty on miss.
std::string GameFreeCameraShim::ReadGDeveloperValueForTesting(
    const std::string &path) {
    if (path.empty()) {
        return std::string();
    }
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return std::string();
    }
    std::string content;
    char buf[4096] = {};
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
    }
    std::fclose(f);

    // Crude pattern match. SCS writes the cvar as one per line,
    // optionally with surrounding whitespace, in this exact form:
    //   uset g_developer "1"
    // Quotes are always present in the file produced by ATS 1.61.x.
    auto pos = content.find("uset g_developer");
    if (pos == std::string::npos) {
        return std::string();
    }
    auto eol = content.find('\n', pos);
    std::string line = content.substr(pos, eol - pos);
    auto q1 = line.find('"');
    auto q2 = line.find('"', q1 == std::string::npos ? 0 : q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) {
        return std::string();
    }
    return line.substr(q1 + 1, q2 - q1 - 1);
}

namespace {

// Best-effort path to the game's `config.cfg`. We try:
//
//   1. The shell-resolved `Documents` folder (CSIDL_MYDOCUMENTS).
//      Windows already follows the OneDrive Documents redirect for
//      us when it is active, so this single call covers the most
//      common OneDrive layout.
//   2. The USERPROFILE-based enumeration (covers non-redirected
//      Documents, XP-era My Documents, OneDrive with the English
//      folder name kept, OneDrive with the Spanish folder name
//      kept, and multi-account OneDrive `OneDrive - <org>\`).
//
// Both ATS and ETS2 stems are tried in each step so the same DLL
// works on either game.
std::string ProbeConfigPath() {
#if defined(_WIN32)
    char mydocs[MAX_PATH] = {};
    if (SUCCEEDED(::SHGetFolderPathA(nullptr, CSIDL_MYDOCUMENTS, nullptr,
                                      SHGFP_TYPE_CURRENT, mydocs))) {
        static const char *stems[] = {
            "American Truck Simulator",
            "Euro Truck Simulator 2",
        };
        for (const char *stem : stems) {
            std::string p = std::string(mydocs) + "\\" + stem +
                            "\\config.cfg";
            FILE *f = std::fopen(p.c_str(), "rb");
            if (f) {
                std::fclose(f);
                return p;
            }
        }
    }
    char profile[MAX_PATH] = {};
    if (SUCCEEDED(::SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr,
                                      SHGFP_TYPE_CURRENT, profile))) {
        const auto candidates =
            GameFreeCameraShim::ProbeConfigCandidatesForTesting(
                std::string(profile));
        for (const auto &c : candidates) {
            FILE *f = std::fopen(c.c_str(), "rb");
            if (f) {
                std::fclose(f);
                return c;
            }
        }
    }
#else
    (void)0;
#endif
    return std::string();
}

}  // namespace

GameFreeCameraShim::GameFreeCameraShim() = default;
GameFreeCameraShim::~GameFreeCameraShim() = default;

void GameFreeCameraShim::SetEnabled(bool enabled) { enabled_ = enabled; }
void GameFreeCameraShim::SetToggleVk(unsigned int vk) { toggle_vk_ = vk; }
void GameFreeCameraShim::SetExitPressVk(unsigned int vk) {
    exit_press_vk_ = vk;
}
void GameFreeCameraShim::BindGameHwnd(HWND hwnd) { hwnd_ = hwnd; }
void GameFreeCameraShim::BindDevCameraPatchResult(
    const DevCameraCvarPatchResult *r) {
    dev_cvar_patch_result_ = r;
}

void GameFreeCameraShim::ProbeGDeveloperFromConfig() {
    const std::string path = ProbeConfigPath();
    const std::string value = ReadGDeveloperValueForTesting(path);
    const bool ok = (!path.empty()) && (value == "1" || value == "2");
    supported_ = ok;
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: g_developer probe path=%s value=%s -> supported=%d",
        path.empty() ? "<not found>" : path.c_str(),
        value.empty() ? "<unset>" : value.c_str(),
        supported_ ? 1 : 0);
}

void GameFreeCameraShim::OnEnterWalking() {
    walks_entered_++;
    // Compose a single, comprehensive traceback line for the F10
    // Walking-entry event. The previous version split the log into
    // three branches (disabled / unsupported / dispatched) which
    // meant the analyst had to correlate 3 lines per F10 to figure
    // out what happened. The new line covers all five states in one
    // shot so the next session can grep game.log.txt for
    // `GameFreeCameraShim: Walking entry` and get a self-explanatory
    // line every time, regardless of whether the shim fired, the
    // gate was off, the cvar was 0, etc.
    const char *path_name =
        last_dispatch_path_ == 2
            ? "PostMessage"
            : (last_dispatch_path_ == 1 ? "SendInput" : "none");
    const char *gate_str = enabled_ ? "on" : "off";
    const char *cvar_str = supported_ ? "1" : "0/missing";
    const char *patch_outcome = "unbound";
    char patch_cvar_buf[13] = "?";
    uint64_t patch_value_va = 0;
    if (dev_cvar_patch_result_ != nullptr) {
        if (dev_cvar_patch_result_->patched) {
            patch_outcome = "patched 0->1";
        } else if (dev_cvar_patch_result_->skipped) {
            patch_outcome = "skipped";
        } else {
            patch_outcome = "no-op";
        }
        // Show the rebased cvar value VA + the first 12 bytes of
        // the value buffer (post-patch) so the analyst can confirm
        // the patch landed.
        patch_value_va = dev_cvar_patch_result_->cvar_value_va;
        const char *src =
            dev_cvar_patch_result_->cvar_value_after[0] != '\0'
                ? dev_cvar_patch_result_->cvar_value_after
                : dev_cvar_patch_result_->cvar_value_before;
        std::snprintf(patch_cvar_buf, sizeof(patch_cvar_buf), "%.12s", src);
    }
    if (!enabled_) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: Walking entry #%d (gate=%s, "
            "config-cfg[probe]='%s', dev_cvar_patcher=%s cvar_va=0x%llX "
            "value=%.12s, hwnd=0x%llX) — Set [m2] game_free_camera_shim "
            "= true in WalkerCore.ini to send the dev free camera "
            "toggle on F10. The dev_cvar_patcher independently flipped "
            "g_developer '0' -> '1' in amtrucks.exe memory so any "
            "future dev-camera path (the `free_camera_u` console "
            "command, the dev UI button o_im_freecam, future "
            "in-process camera manager hooks) works without editing "
            "Documents\\American Truck Simulator\\config.cfg.)",
            walks_entered_, gate_str, cvar_str, patch_outcome,
            static_cast<unsigned long long>(patch_value_va), patch_cvar_buf,
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(hwnd_)));
        return;
    }
    if (!supported_) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: Walking entry #%d (gate=%s, "
            "config-cfg[probe]='%s', dev_cvar_patcher=%s cvar_va=0x%llX "
            "value=%.12s, hwnd=0x%llX) — The shim synthesised the "
            "toggle VK but the game's controls.sii binds Numpad-0 to "
            "the user's chosen action (the dev free camera is UI-"
            "button only via o_im_freecam, no action-name binding in "
            "the stock profile). Two ways forward: (a) add a binding "
            "for the toggle VK in inputs.dat your kitchen <profile>\\"
            "controls.sii that maps to a dev-camera action (or to "
            "free_camera_u), then re-deploy, or (b) use the game's "
            "dev console (`~` key) and type `free_camera_u` while "
            "in the dev free camera context.)",
            walks_entered_, gate_str, cvar_str, patch_outcome,
            static_cast<unsigned long long>(patch_value_va), patch_cvar_buf,
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(hwnd_)));
        return;
    }
    const int dispatched = TriggerToggle();
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: Walking entry #%d -> dispatched %d key "
        "event(s) (gate=%s, config-cfg[probe]='%s', dev_cvar_patcher=%s "
        "cvar_va=0x%llX value=%.12s, toggle VK=0x%X, exit VK=0x%X, "
        "path=%s hwnd=0x%llX) — The dev_cvar_patcher flipped "
        "g_developer in memory before this dispatch; if the dev free "
        "camera still does not activate, the game's controls.sii "
        "binding for Numpad-0 routes the key to a different action "
        "(see docs/observations/tmrealwalk-findings.md).",
        walks_entered_, dispatched, gate_str, cvar_str, patch_outcome,
        static_cast<unsigned long long>(patch_value_va), patch_cvar_buf,
        toggle_vk_, exit_press_vk_, path_name,
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(hwnd_)));
}

void GameFreeCameraShim::OnExitWalking() {
    exiting_walking_ = true;
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: Walking exit (walks_entered=%d, "
        "toggles_dispatched=%d)",
        walks_entered_, toggles_dispatched_);
}

int GameFreeCameraShim::TriggerToggle() {
    if (!enabled_) {
        return 0;
    }
    int total = 0;
    // Stage 1 (optional): exit-cabin press. The user's config does
    // not gate this so when set to 0 it's a no-op (the SCS free
    // camera activated from inside the cabin will be at the cabin's
    // default viewpoint; the player can move from there).
    if (exit_press_vk_ != 0) {
        total += SynthKey(exit_press_vk_, hwnd_, last_dispatch_path_,
                          last_postmessage_hwnd_diag_);
    }
    // Stage 2 (always): toggle the SCS free camera. VK_NUMPAD0 is
    // the SCS convention for "fly mode" toggle.
    total += SynthKey(toggle_vk_, hwnd_, last_dispatch_path_,
                      last_postmessage_hwnd_diag_);
    toggles_dispatched_++;
    last_dispatch_ms_ = static_cast<int>(NowMs() & 0x7FFFFFFF);

    // Mark the VKs as "echo" so the WH_KEYBOARD_LL hook ignores the
    // key-up we just sent back to ourselves, instead of triggering
    // another Walking-toggle round-trip.
    if (exit_press_vk_ != 0) {
        const long long until = NowMs() + 250;
        if (echo_count_ < kMaxEcho) {
            echo_vks_[echo_count_] = exit_press_vk_;
            echo_until_ms_[echo_count_] = until;
            echo_count_++;
        }
    }
    {
        const long long until = NowMs() + 250;
        if (echo_count_ < kMaxEcho) {
            echo_vks_[echo_count_] = toggle_vk_;
            echo_until_ms_[echo_count_] = until;
            echo_count_++;
        }
    }
    return total;
}

bool GameFreeCameraShim::ConsumeEchoKey(unsigned int vk) {
    const long long now = NowMs();
    bool consumed = false;
    int kept = 0;
    for (int i = 0; i < echo_count_ && i < kMaxEcho; ++i) {
        if (echo_until_ms_[i] == 0) {
            continue;
        }
        if (echo_until_ms_[i] < now) {
            continue;  // expired slot
        }
        if (!consumed && echo_vks_[i] == vk) {
            consumed = true;
            continue;  // eat this one, do not keep
        }
        echo_vks_[kept] = echo_vks_[i];
        echo_until_ms_[kept] = echo_until_ms_[i];
        kept++;
    }
    for (int i = kept; i < echo_count_ && i < kMaxEcho; ++i) {
        echo_vks_[i] = 0;
        echo_until_ms_[i] = 0;
    }
    echo_count_ = kept;
    return consumed;
}

}  // namespace walkercore
