// WalkerCore — Game-Free-Camera shim (M2 GPU render push — MVP path).
//
// Why this exists:
//   The M2 lander was supposed to push our `CameraState::view` and
//   `CameraState::projection` to the GPU every frame so that pressing
//   F10 (entering Walking state) actually moved the camera on screen.
//   In practice SCS's renderer drives its own constant buffers and
//   vtable chains (the same chain that ReShade, DXVK and any other
//   in-process D3D11 hook attaches to), so replacing the SCS view+proj
//   from outside the engine without walking the full SCS render
//   pipeline is not feasible in our sandbox.
//
// What TMRealWalk (the working reference) does instead is documented
// in its README: it relies on the game's own developer free camera
//   (gated by `g_developer`) and only renders overlay geometry (shadow,
//   prompts, menu) on top of the present chain. The player's *world*
//   camera is the game's, ours only draws on top.
//
// This shim is the MVP path for our M2 lander:
//   - Owner: PluginEntry (M2 conductor).
//   - Trigger: WalkerFsm state transition Driving -> Walking.
//   - Action: synthesise a configurable sequence of Win32 keystrokes
//     (default: press and release Numpad-0, which is the SCS free
//     camera toggle in 1.61.3.1s; works because the user's
//     config.cfg already has `uset g_developer "1"`).
//   - Fallback: when the game cvar is 0 / not detected, the provider
//     only logs a guidance note and the user can flip the cvar.
//   - Diagnostics: walker_dump_game_free_camera_shim dumps the
//     shim state for F12-driven verification.
//
// ABI:
//   walker_game_free_camera_shim_set_enabled(int)
//   walker_game_free_camera_shim_supported()
//   walker_game_free_camera_shim_on_enter_walking()
//   walker_game_free_camera_shim_on_exit_walking()
//   walker_game_free_camera_shim_toggle_now()
//   walker_dump_game_free_camera_shim()
//
// The shim never modifies SCS state. It only synthesises keys.
// The default `enabled` is false; the user must opt in via
// `[m2] game_free_camera_shim = 1` in `WalkerCore.ini`.

#ifndef WALKERCORE_GAME_FREE_CAMERA_SHIM_H
#define WALKERCORE_GAME_FREE_CAMERA_SHIM_H

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace walkercore {

class GameFreeCameraShim {
public:
    GameFreeCameraShim();
    ~GameFreeCameraShim();

    GameFreeCameraShim(const GameFreeCameraShim &) = delete;
    GameFreeCameraShim &operator=(const GameFreeCameraShim &) = delete;

    // INI gate. False by default — must be explicitly enabled.
    void SetEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    // Bind the game window so TriggerToggle can PostMessage(WM_KEYDOWN)
    // directly to it instead of going through SendInput. PostMessage
    // reaches the target HWND regardless of which window is in the
    // foreground, which is critical for fullscreen / DXGI exclusive
    // modes where SendInput's foreground-targeted input gets discarded
    // by the game. Bound from PluginEntry via FindGameHwnd (which
    // enumerates top-level windows owned by the current process).
    // Safe to call with nullptr — SynthKey then falls back to
    // SendInput. The previous value is overwritten on every call.
    void BindGameHwnd(HWND hwnd);
    HWND game_hwnd() const { return hwnd_; }

    // Keystroke sequence tuning. Defaults: 0x60 = Numpad-0 (SCS free
    // camera toggle), 0 = no separate "exit cabin" press. The values
    // are Win32 VK_* codes.
    void SetToggleVk(unsigned int vk);
    void SetExitPressVk(unsigned int vk);
    unsigned int toggle_vk() const { return toggle_vk_; }
    unsigned int exit_press_vk() const { return exit_press_vk_; }

    // Best-effort probe: did the game load `g_developer = 1` at
    // startup? We read `config.cfg` (best guess from the env) and
    // look for `uset g_developer "1"`. If we cannot find the config
    // file or the value reads "0", `supported_` stays false and
    // `TriggerToggle()` only logs an advisory. The probe is cheap
    // (single text scan, ~64 KiB) and runs at most once per call.
    void ProbeGDeveloperFromConfig();
    bool isSupported() const { return supported_; }
    // Test-only: force supported_ to a specific value so the unit
    // tests can exercise the dispatch path (TriggerToggle is a no-op
    // when supported_ is false). Not part of the runtime API — only
    // the tests call this. The production path sets supported_ via
    // ProbeGDeveloperFromConfig, which reads the user's config.cfg.
    void SetSupportedForTesting(bool s) { supported_ = s; }
    int walks_entered() const { return walks_entered_; }
    int toggles_dispatched() const { return toggles_dispatched_; }
    int last_dispatch_ms() const { return last_dispatch_ms_; }

    // FSM-facing entry points.
    void OnEnterWalking();
    void OnExitWalking();

    // Manual trigger (e.g. from the settings menu or diagnostic dump).
    // Always returns the number of keys actually synthesised.
    int TriggerToggle();

    // Walker's own keyboard-hook call. If a key has been synthesised
    // via SendInput, the OS will deliver it back to our WH_KEYBOARD_LL
    // hook. We mark it as "echo" so the walker does not loop on it.
    bool ConsumeEchoKey(unsigned int vk);

    // Diagnostic getters for the F12 dump + walking-entry log line.
    //   last_dispatch_path() — 0 = no dispatch yet, 1 = SendInput
    //     (foreground-targeted, can be discarded in fullscreen),
    //     2 = PostMessage (HWND-targeted, always reaches the game).
    //   last_postmessage_target_diag() — lower 32 bits of the HWND
    //     used on the most recent PostMessage dispatch. Useful for
    //     confirming "the shim targeted the right window" without
    //     exposing the raw HWND in the public ABI.
    int last_dispatch_path() const { return last_dispatch_path_; }
    unsigned int last_postmessage_target_diag() const {
        return last_postmessage_hwnd_diag_;
    }

    // -- Test seams ---------------------------------------------------------
    // Pure helpers exposed for the unit-test suite. They are not part
    // of the runtime ABI and clients should never call them. The naming
    // suffix `_ForTesting` makes the intent explicit.
    //
    // `ProbeConfigCandidatesForTesting(profile)` returns the candidate
    // `Documents` sub-folders (relative to the supplied USERPROFILE
    // path) that the runtime probe tries in order. Used by tests to
    // pin the OneDrive-aware candidate list.
    static std::vector<std::string> ProbeConfigCandidatesForTesting(
        const std::string &profile);

    // `ReadGDeveloperValueForTesting(path)` parses the first
    // `uset g_developer "X"` line in the file at `path` and returns
    // the value (without quotes). Empty string on miss. Used by tests
    // to pin the cvar-line scanner.
    static std::string ReadGDeveloperValueForTesting(const std::string &path);

private:
    bool enabled_ = false;
    bool supported_ = false;
    int walks_entered_ = 0;
    int toggles_dispatched_ = 0;
    int last_dispatch_ms_ = 0;
    unsigned int toggle_vk_ = 0x60;     // VK_NUMPAD0
    unsigned int exit_press_vk_ = 0;    // 0 = no separate exit press
    bool exiting_walking_ = false;
    HWND hwnd_ = nullptr;              // bound game window for PostMessage path
    int last_dispatch_path_ = 0;       // 0 = none, 1 = SendInput, 2 = PostMessage
    unsigned int last_postmessage_hwnd_diag_ = 0;  // lower 32 bits of the HWND we last targeted
    // Echo-suppression: the keystrokes we SendInput come back to our
    // keyboard hook. We tag up to 8 echo VKs seen in the last 250 ms.
    static constexpr int kMaxEcho = 8;
    unsigned int echo_vks_[kMaxEcho] = {0};
    long long echo_until_ms_[kMaxEcho] = {0};
    int echo_count_ = 0;
};

}  // namespace walkercore

#endif  // WALKERCORE_GAME_FREE_CAMERA_SHIM_H
