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
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>  // SHGetFolderPathA + CSIDL_PROFILE
#endif

namespace walkercore {

namespace {

// Test-time fake: a Windows-only `SendInput` call. On a non-Windows
// host (or under the test macro) we leave it as a no-op so the unit
// tests still compile and run end-to-end.
int SynthKey(unsigned int vk) {
#if defined(_WIN32) && !defined(WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED)
    if (vk == 0) {
        return 0;
    }
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
    return 2;  // 2 inputs sent: one down, one up
#else
    (void)vk;
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

// Best-effort path to the game's `config.cfg`. Reading from
// `Documents\<game>\config.cfg` is the standard SCS location. We
// probe both ATS and ETS2 stems so the same DLL works on either
// (the M2 lander does not pick one).
std::string ProbeConfigPath() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    if (SUCCEEDED(::SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr,
                                      SHGFP_TYPE_CURRENT, buf))) {
        // Try ATS first, then ETS2. Both live in
        // <Documents>\<game stem>\config.cfg.
        const char *candidates[] = {
            "\\My Documents\\American Truck Simulator\\config.cfg",
            "\\Documents\\American Truck Simulator\\config.cfg",
            "\\My Documents\\Euro Truck Simulator 2\\config.cfg",
            "\\Documents\\Euro Truck Simulator 2\\config.cfg",
        };
        for (const char *c : candidates) {
            std::string p = std::string(buf) + c;
            FILE *f = std::fopen(p.c_str(), "rb");
            if (f) {
                std::fclose(f);
                return p;
            }
        }
    }
#else
    (void)0;
#endif
    return std::string();
}

// Read the first `uset g_developer "X"` line in the file and return
// the value as a string ("0", "1", "2", ...). Empty string if not
// found.
std::string ReadGDeveloperValue(const std::string &path) {
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

}  // namespace

GameFreeCameraShim::GameFreeCameraShim() = default;
GameFreeCameraShim::~GameFreeCameraShim() = default;

void GameFreeCameraShim::SetEnabled(bool enabled) { enabled_ = enabled; }
void GameFreeCameraShim::SetToggleVk(unsigned int vk) { toggle_vk_ = vk; }
void GameFreeCameraShim::SetExitPressVk(unsigned int vk) {
    exit_press_vk_ = vk;
}

void GameFreeCameraShim::ProbeGDeveloperFromConfig() {
    const std::string path = ProbeConfigPath();
    const std::string value = ReadGDeveloperValue(path);
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
        "event(s) (toggle VK=0x%X, exit press VK=0x%X)",
        walks_entered_, dispatched, toggle_vk_, exit_press_vk_);
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
        total += SynthKey(exit_press_vk_);
    }
    // Stage 2 (always): toggle the SCS free camera. VK_NUMPAD0 is
    // the SCS convention for "fly mode" toggle.
    total += SynthKey(toggle_vk_);
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
