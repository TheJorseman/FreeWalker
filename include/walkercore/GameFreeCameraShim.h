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

// Forward declaration so the shim can read the patcher's last
// result without depending on the patcher's full definition. The
// definition is in DevCameraCvarPatcher.h; PluginEntry pulls both
// in the same translation unit.
namespace walkercore {
struct DevCameraCvarPatchResult;
}  // namespace walkercore

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

    // Bind the game window. The bound HWND is the gating condition
    // for SynthKey dispatch — without a target we would either
    // PostMessage into the void (no HWND to receive it) or
    // SendInput into whatever process happens to be in the
    // foreground (which would dispatch a stray Numpad-0 into the
    // host environment). With a target bound we SendInput (the
    // direct path the user confirmed works for manual Numpad-0 —
    // ATS 1.61.x reads keyboard via DirectInput 8, which polls
    // the Windows input queue that SendInput synthesises into).
    // Bound from PluginEntry via FindGameHwnd (which enumerates
    // top-level windows owned by the current process). Safe to
    // call with nullptr — SynthKey then skips dispatch and the
    // path counter records `none`. The previous value is
    // overwritten on every call.
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

    // Bind the dev-camera cvar patcher's last result so the shim
    // can include it in the F10 Walking-entry log line. Pointer is
    // owned by the patcher; the shim never deletes or copies it.
    // Pass nullptr to clear the binding (e.g. after shutdown).
    // The patched / skipped / cvar-name / cvar-value fields appear
    // in the next OnEnterWalking log line for traceback.
    void BindDevCameraPatchResult(const DevCameraCvarPatchResult *r);
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
    //   last_dispatch_path() — 0 = no dispatch (no bound HWND),
    //     1 = SendInput (the DirectInput 8 keyboard path).
    //   last_postmessage_target_diag() — lower 32 bits of the
    //     bound game HWND that SynthKey most recently targeted.
    //     Useful for confirming "the shim targeted the right
    //     window" without exposing the raw HWND in the public ABI.
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

    // M2.5 controls.sii helpers. The shim needs to know which key (if
    // any) the user has bound to the dev-camera-toggle action in
    // controls.sii. The user profile uses the same OneDrive-aware
    // folder layout as the g_developer config probe, so we reuse
    // ProbeConfigCandidatesForTesting and just swap the trailing
    // filename for `controls.sii`. The probe is best-effort — the
    // user might not have an active profile on the host (they could be
    // running the DLL on a build server with no game install), and an
    // empty / missing file is treated as "no binding found".
    //
    // `ProbeControlsSiiCandidatesForTesting(profile)` returns the
    // candidate `<profile>/.../<game>/controls.sii` paths. Used by
    // tests to pin the OneDrive-aware candidate list (parallel to
    // ProbeConfigCandidatesForTesting for config.cfg).
    static std::vector<std::string> ProbeControlsSiiCandidatesForTesting(
        const std::string &profile);

    // `ReadControlsSiiKeyForActionForTesting(path, action)` returns
    // the first keyboard.* token bound to `mix <action>` in the
    // controls.sii file at `path`. Empty string on miss or parse
    // failure. The token is the bare SCS name (e.g. `keyboard.num5`
    // or `keyboard.f10`) — the caller converts it to a Win32 VK
    // with ScsKeyNameToVkForTesting. The scanner is line-based and
    // looks for `mix <action> ``...``` literal tokens; it stops at
    // the first match so users with multiple bindings (e.g. a
    // joystick + keyboard) get the keyboard one because we put it
    // first in the source line. Reverse-order tokens are tolerated
    // because some profiles put `keyboard.f10?0 | semantical.foo?0`
    // in either order.
    static std::string ReadControlsSiiKeyForActionForTesting(
        const std::string &path, const std::string &action);

    // `ScsKeyNameToVkForTesting(name)` converts an SCS keyboard
    // token (e.g. `keyboard.num5`, `keyboard.f10`, `keyboard.f11`,
    // `keyboard.numpad5`, `keyboard.lshift`, `keyboard.space`) to
    // the matching Win32 VK_* code. Returns 0 on miss. Supports
    // the small subset of keys the SCS controls.sii grammar
    // exposes — see GameFreeCameraShim.cpp for the table.
    static unsigned int ScsKeyNameToVkForTesting(const std::string &name);

    // M2.5: runtime hook. The PluginEntry init path calls this once
    // after ProbeGDeveloperFromConfig. It locates the user's
    // controls.sii (best effort), looks for `dev_camera_toggle`
    // first, then `walk_mode`, then `free_camera` (the three action
    // names the SCS dev-camera UI / consolehandles enum in the
    // binary exposes — see tmrealwalk-findings.md for the RE
    // provenance). If a binding is found AND it differs from the
    // current toggle_vk_ AND the INI did not pin a custom value
    // (see INI override contract below), the shim adopts it. If
    // none is found, the shim emits a one-line, copy-pasteable
    // guidance message to game.log so the user knows exactly which
    // line to add to their controls.sii. The auto-detected value is
    // not persisted to the INI — the user must explicitly opt in
    // via `[m2] dev_camera_toggle_vk = 0xHH` to override the
    // default + auto-detect chain.
    //
    // INI override contract:
    //   - INI knob set (any non-default) → use the INI value, do
    //     NOT auto-detect (the user knows what they want).
    //   - INI knob unset (default 0x60 Numpad-0) → auto-detect if
    //     a binding is found; fall back to the default otherwise.
    // The runtime differentiates "default" from "user-set" via the
    // IniOverrideFlag that PluginEntry installs before calling
    // AutoDetectToggleVkFromControlsSii.
    void AutoDetectToggleVkFromControlsSii(bool ini_overrides_default);
    bool ini_overrides_toggle_vk() const { return ini_overrides_toggle_vk_; }
    // Last auto-detected key name from the controls.sii probe. Empty
    // when nothing was found or the INI took precedence. Used by the
    // F10 Walking-entry log to show "auto-detected keyboard.num5 from
    // <path>/controls.sii" so the user can verify the right key was
    // picked up.
    const std::string &auto_detected_key_name() const {
        return auto_detected_key_name_;
    }
    const std::string &auto_detected_controls_sii_path() const {
        return auto_detected_controls_sii_path_;
    }

private:
    bool enabled_ = false;
    bool supported_ = false;
    int walks_entered_ = 0;
    int toggles_dispatched_ = 0;
    int last_dispatch_ms_ = 0;
    unsigned int toggle_vk_ = 0x60;     // VK_NUMPAD0
    unsigned int exit_press_vk_ = 0;    // 0 = no separate exit press
    bool exiting_walking_ = false;
    // M2.5: set by PluginEntry when [m2] dev_camera_toggle_vk was
    // explicitly present in the INI (any value, including the default
    // literal). When true, AutoDetectToggleVkFromControlsSii skips the
    // controls.sii probe and the INI value wins. When false, the shim
    // auto-detects from controls.sii if a binding is found and falls
    // back to the hard-coded default 0x60 otherwise.
    bool ini_overrides_toggle_vk_ = false;
    // M2.5: diagnostic fields exposed via auto_detected_key_name() /
    // auto_detected_controls_sii_path() so the F10 Walking-entry log
    // can confirm "picked up keyboard.num5 from
    // C:\Users\...\controls.sii line 184" without requiring a
    // separate accessor. Empty when nothing was auto-detected.
    std::string auto_detected_key_name_;
    std::string auto_detected_controls_sii_path_;
    HWND hwnd_ = nullptr;              // bound game window — gates dispatch
    const DevCameraCvarPatchResult *dev_cvar_patch_result_ = nullptr;
    int last_dispatch_path_ = 0;       // 0 = none, 1 = SendInput
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
