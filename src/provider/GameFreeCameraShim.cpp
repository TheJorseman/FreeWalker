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

#include "walkercore/Log.h"

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

// Synthesise a single keypress. When `target_hwnd` is non-null, use
// PostMessage(WM_KEYDOWN) + PostMessage(WM_KEYUP) to dispatch directly
// to the bound game window — this works in fullscreen / DXGI exclusive
// mode where SendInput's foreground-targeted input gets discarded by
// the game. When `target_hwnd` is null, fall back to SendInput (the
// legacy path). Returns the number of events dispatched (always 2 in
// practice: one KEYDOWN + one KEYUP) and updates the diagnostic path
// counters in `path_out` / `target_diag_out` for the F12 dump.
//
// The PostMessage route is what the M2 lander needs on ATS 1.61.3.1s +
// ReShade + OptiScaler + RenoDX: the user confirmed Numpad-0 manual
// presses DO toggle the SCS dev free camera, but the shim's previous
// SendInput call was not reaching the game window (visible as
// `dispatched 2 key event(s)` in the log with no on-screen effect).
// PostMessage targets the HWND directly, which is the standard way
// for in-process synthetic input to a known window regardless of
// focus / fullscreen state.
int SynthKey(unsigned int vk, HWND target_hwnd,
             int &path_out, unsigned int &target_diag_out) {
#if defined(_WIN32) && !defined(WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED)
    if (vk == 0) {
        path_out = 0;
        target_diag_out = 0;
        return 0;
    }
    if (target_hwnd != nullptr) {
        // PostMessage path — direct to the bound game window.
        // lParam encoding for WM_KEYDOWN: repeat count in low word,
        // scan code in bits 16-23, extended-key flag in bit 24, etc.
        // We use 0 (no repeat, no extended) which is the simplest
        // valid encoding for a non-extended VK like VK_NUMPAD0.
        PostMessageA(target_hwnd, WM_KEYDOWN, static_cast<WPARAM>(vk), 0);
        PostMessageA(target_hwnd, WM_KEYUP,   static_cast<WPARAM>(vk),
                     static_cast<LPARAM>(1 << 30 | 1 << 31));
        path_out = 2;  // PostMessage
        target_diag_out = static_cast<unsigned int>(
            reinterpret_cast<std::uintptr_t>(target_hwnd) & 0xFFFFFFFFu);
        return 2;
    }
    // SendInput fallback — foreground-targeted, can be discarded by
    // fullscreen / DXGI exclusive modes.
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
    target_diag_out = 0;
    return 2;
#else
    // Test stub: when GameFreeCameraShim is built with
    // WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED (the test build
    // configuration), we don't call SendInput or PostMessage.
    // Instead we simulate the dispatch decision so the tests can
    // verify the right path was taken based on the bound HWND.
    (void)vk;
    if (target_hwnd != nullptr) {
        path_out = 2;  // PostMessage
        target_diag_out = static_cast<unsigned int>(
            reinterpret_cast<std::uintptr_t>(target_hwnd) & 0xFFFFFFFFu);
        return 2;
    }
    path_out = 1;  // SendInput
    target_diag_out = 0;
    return 0;
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
    if (!enabled_) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: Walking entry #%d (shim disabled — "
            "set [m2] game_free_camera_shim = 1 to drive SCS free "
            "camera from F10)",
            walks_entered_);
        return;
    }
    if (!supported_) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: Walking entry #%d — supported=false "
            "(g_developer not '1' in config.cfg, or config not found). "
            "F10 will NOT toggle the camera until the user enables "
            "g_developer in the game's config.cfg.",
            walks_entered_);
        return;
    }
    const int dispatched = TriggerToggle();
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: Walking entry #%d -> dispatched %d key "
        "event(s) (toggle VK=0x%X, exit press VK=0x%X, path=%s "
        "hwnd=0x%X)",
        walks_entered_, dispatched, toggle_vk_, exit_press_vk_,
        last_dispatch_path_ == 2 ? "PostMessage"
                                 : (last_dispatch_path_ == 1 ? "SendInput"
                                                            : "none"),
        last_postmessage_hwnd_diag_);
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
