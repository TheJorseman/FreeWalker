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

    // Keystroke sequence tuning. Default toggle VK: 0x30 = top-row
    // `0` (the SCS dev free-camera shortcut in amtrucks.exe
    // 1.61.3.1s when `g_debug_camera = 1`, patched by
    // dev_cvar_patcher). Exit-press default: 0 = no separate
    // "exit cabin" press. Both are Win32 VK_* codes and can be
    // re-targeted via INI.
    void SetToggleVk(unsigned int vk);
    void SetExitPressVk(unsigned int vk);
    unsigned int toggle_vk() const { return toggle_vk_; }
    unsigned int exit_press_vk() const { return exit_press_vk_; }

    // G5.19: when true, TriggerToggle() routes the dev-camera
    // activation through the dev console (open `~`, type the
    // literal command `free_camera`, Enter, close `~`) instead of
    // dispatching a single VK via SendInput. The console path is
    // the only verified mechanism on ATS 1.61.3.1s because the
    // SCS-internal keypress handler for the dev free camera has
    // been routed around by `mix camdbg \`keyboard.f11?0\`` and
    // the action registry eats both F11 and F10 before they reach
    // the dev camera shortcut. Default true. INI knob: `[m2]
    // dev_camera_via_console`.
    void SetDevCameraViaConsole(bool via_console);
    bool dev_camera_via_console() const {
        return dev_camera_via_console_;
    }
    int console_cmds_dispatched() const {
        return console_cmds_dispatched_;
    }

    // Best-effort probe: did the game load `g_developer = 1` at
    // startup? We read `config.cfg` (best guess from the env) and
    // look for `uset g_developer "1"`. If we cannot find the config
    // file or the value reads "0", `supported_` stays false and
    // `TriggerToggle()` only logs an advisory. The probe is cheap
    // (single text scan, ~64 KiB) and runs at most once per call.
    void ProbeGDeveloperFromConfig();
    bool isSupported() const { return supported_; }
    // M2.7 follow-up: numeric value of `g_developer` parsed from
    // config.cfg (-1 = unprobed, 0 = off, 1 = dev shortcuts enabled,
    // 2 = dev cvar mode). Exposed for the auto-pin / walking-entry
    // log line so the analyst sees the parsed gate state inline
    // with the dispatch decision. M2.7d (Oct 2026): the
    // `g_developer_value_ == 1` gate that used to skip the auto-pin
    // is GONE — F4.0d live-verified that the SCS engine does NOT
    // intercept F11 before the action registry, so the previous
    // 'skip the auto-pin when on' rationale was incorrect for
    // ATS 1.61.3.1s. The accessor is still useful for diagnostics
    // but the runtime no longer branches on it.
    int g_developer_value() const { return g_developer_value_; }

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
    // Test seam for the `g_developer_value_` cached state. The
    // production path is ProbeGDeveloperFromConfig parsing
    // `uset g_developer "X"` from the user's config.cfg. Tests
    // install the value directly to exercise the camdbg/F11
    // auto-pin path under different `g_developer` values without
    // needing the parser. M2.7d (Oct 2026): the auto-pin gate
    // was removed, so any non-default `g_developer` value must
    // still trigger the re-pin when a controls.sii conflict exists.
    void SetGDeveloperValueForTesting(int v) { g_developer_value_ = v; }
    int walks_entered() const { return walks_entered_; }
    int toggles_dispatched() const { return toggles_dispatched_; }
    int last_dispatch_ms() const { return last_dispatch_ms_; }

    // FSM-facing entry points.
    void OnEnterWalking();
    void OnExitWalking();

    // Diagnostic: scan the user's controls.sii for any action bound
    // to `toggle_vk_` and log a heads-up so the user sees binding
    // conflicts at shim init time, not after a failed F10 dispatch.
    // Idempotent — the cache `controls_sii_conflict_logged_` guards
    // against duplicate log lines if `BindGameHwnd` is called
    // multiple times (the shim rebinds on profile/profile-id reload).
    // `controls_sii_path` is the resolved path to scan; pass an
    // empty string to use `auto_detected_controls_sii_path_` (the
    // runtime probe's last-known result).
    void LogControlsSiiVkConflictIfAny(const std::string &controls_sii_path = std::string());

    // Manual trigger (e.g. from the settings menu or diagnostic dump).
    // Always returns the number of keys actually synthesised.
    int TriggerToggle();

    // G5.22: pre-dispatch from the WH_KEYBOARD_LL callback when F10
    // (toggle walk) is detected as a rising edge. The hook calls this
    // SYNCHRONOUSLY before returning CallNextHookEx, so the synthesised
    // toggle key lands in the OS input queue BEFORE the original F10
    // is delivered to the game. The next DirectInput poll therefore
    // reads the toggle first (dev cam activates while state is still
    // Driving) and F10 second (Walking transition starts). This avoids
    // the G5.21 timing problem where OnEnterWalking fired the toggle
    // AFTER the FSM had already transitioned to Walking, which the
    // user reported "F10 only does screenshot, dev cam does not
    // activate". `trigger_reason` is a short tag for the log line
    // (e.g. "ToggleWalk rising edge"). Returns the number of keys
    // actually synthesised.
    int PreDispatchDevCamToggleFromF10(const char *trigger_reason);

    // G5.22: timestamp (ms since boot) of the last
    // PreDispatchDevCamToggleFromF10 call that actually dispatched at
    // least one key. 0 when no pre-dispatch has happened yet. The
    // OnEnterWalking / OnExitWalking fallback paths consult this to
    // skip a duplicate dispatch when the WH_KEYBOARD_LL pre-dispatch
    // already covered the toggle.
    int last_pre_dispatch_ms() const { return last_pre_dispatch_ms_; }

    // G5.22 test seam. Disables the foreground-process guard inside
    // PreDispatchDevCamToggleFromF10 so the unit tests can drive the
    // dispatch path without standing up a real ATS foreground
    // window. The production path leaves this false; only the test
    // suite flips it on. The seam is gated on a process-wide flag
    // read by the runtime, not a permanent API surface, so a user
    // who accidentally turns it on via INI would not weaken the
    // real-world safety — production builds have no INI knob.
    void SetForegroundGuardDisabledForTesting(bool disabled) {
        foreground_guard_disabled_for_testing_ = disabled;
    }

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
    // M2 diagnostic: cached controls.sii binding conflict set
    // (populated by `LogControlsSiiVkConflictIfAny`). The vector
    // is empty when no conflict was detected. Tests use this to
    // pin the cache surface that the F10 dispatch log reads.
    const std::vector<std::string> &controls_sii_conflict_actions_for_testing() const {
        return controls_sii_conflict_actions_;
    }
    bool controls_sii_conflict_detected_for_testing() const {
        return controls_sii_conflict_detected_;
    }
    const std::string &controls_sii_conflict_last_path_for_testing() const {
        return controls_sii_conflict_last_path_;
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
    //
    // As a recovery path for a known copy-paste typo
    // (`` \` `` instead of `` ` ``, an artifact from editors that
    // auto-escape markdown-style backticks), the scanner falls back
    // to `mix <action> \`...\` ` (escaped-backtick form) when the
    // plain form misses. The fallback form is a SYNTAX ERROR from
    // SCS's perspective — game.log will contain
    // `<ERROR> Expecting string literal, found '"mix ...'` — but
    // WalkerCore can still recover the keyboard token. Callers that
    // care about the typo (for user-facing warnings) should use
    // `ReadControlsSiiKeyForActionWithDiagForTesting` and inspect
    // the `had_escaped_backtick_typo` out-param.
    static std::string ReadControlsSiiKeyForActionForTesting(
        const std::string &path, const std::string &action);

    // Same as `ReadControlsSiiKeyForActionForTesting` but with an
    // extra out-param that tells the caller whether the binding
    // was recovered via the escaped-backtick fallback. When true,
    // the controls.sii file has a typo (`\`` instead of `` ` ``)
    // that the SCS parser will reject at game load; the keyboard
    // token is still returned so the runtime can attempt the
    // dispatch, but the user needs to fix the typo for the
    // dispatch to actually reach the dev_camera_toggle action
    // (the game's action registry never saw the malformed binding).
    // `had_escaped_backtick_typo` is optional and may be null.
    static std::string ReadControlsSiiKeyForActionWithDiagForTesting(
        const std::string &path, const std::string &action,
        bool *had_escaped_backtick_typo);

    // `ScsKeyNameToVkForTesting(name)` converts an SCS keyboard
    // token (e.g. `keyboard.num5`, `keyboard.f10`, `keyboard.f11`,
    // `keyboard.numpad5`, `keyboard.lshift`, `keyboard.space`) to
    // the matching Win32 VK_* code. Returns 0 on miss. Supports
    // the small subset of keys the SCS controls.sii grammar
    // exposes — see GameFreeCameraShim.cpp for the table.
    static unsigned int ScsKeyNameToVkForTesting(const std::string &name);

    // Inverse of `ReadControlsSiiKeyForActionForTesting`: returns
    // every action name in the controls.sii file whose `mix <action>`
    // binding includes a `keyboard.*` token that maps to
    // `target_vk`. Empty vector when no conflict is detected (the
    // common case — shim's target VK is not bound to any action).
    // The result preserves file order so the F10 log line shows the
    // conflict set in the order it appears in the file (top-down),
    // which matches the order the user reads their controls.sii.
    //
    // Diagnostic-only: this is the foundation for the conflict
    // warning the shim logs when its target VK collides with a
    // controls.sii binding. The user reported an F11 → cam 0 cycle
    // (Oct 2026) because F11 was bound to `camdbg` in their
    // controls.sii; the warning would have surfaced the binding
    // conflict at shim init time rather than after the F10 dispatch
    // failed to activate the dev free camera. See
    // GameFreeCameraShim::LogControlsSiiVkConflictIfAny.
    static std::vector<std::string> FindActionsBoundToVkForTesting(
        const std::string &path, unsigned int target_vk);

    // M2.5: runtime hook. The PluginEntry init path calls this once
    // after ProbeGDeveloperFromConfig. It locates the user's
    // controls.sii (best effort) and scans for `dev_camera_toggle`,
    // `camdbg`, `walk_mode`, `free_camera` in priority order (most-
    // specific first). `camdbg` is the actual stock SCS action
    // name (camera + debug) for toggling the dev free camera — RE-
    // confirmed from the CVar `g_debug_camera` registration in
    // `amtrucks.exe`; the SCS input parser rejects `dev_camera_toggle`
    // as "not valid object name" so the intuitive TMRealWalk-style
    // name has to defer to `camdbg` for the dispatch to actually
    // reach the dev camera. If a binding is found AND it differs
    // from the current toggle_vk_ AND the INI did not pin a custom
    // value (see INI override contract below), the shim adopts it.
    // If
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

    // M2.5 follow-up: smart-default auto-enable. Called by PluginEntry
    // AFTER AutoDetectToggleVkFromControlsSii and ProbeGDeveloperFromConfig
    // have both run. If the INI did not explicitly set
    // `game_free_camera_shim = ...` (the explicit flag is false), the
    // shim is currently disabled, a dev_camera_toggle binding was
    // detected in the user's controls.sii, and g_developer is
    // supported (value 1 or 2 in config.cfg), the shim flips its own
    // enabled_ to true and emits a one-line log explaining the
    // smart-default flip. The user can still disable it explicitly
    // via INI `[m2] game_free_camera_shim = false` — the explicit
    // flag gates the smart-default path so a user who has vetted
    // false is not surprised by a silent re-enable.
    //
    // Returns true if the shim was flipped from false → true by this
    // call. The return value is informational — PluginEntry logs the
    // same line independently via the WALKERCORE_LOG_INFO below.
    bool MaybeAutoEnableFromDetection();
    // M2.5 follow-up: test seam. Sets the INI-explicit-shim-enable
    // flag without requiring PluginEntry to install it. The
    // production path is PluginEntry setting
    // m2_game_free_camera_shim_explicit from the INI parser and
    // passing the bool to SetIniOverridesShimEnableForTesting. The
    // unit-test path installs the flag directly so we can exercise
    // MaybeAutoEnableFromDetection under controlled explicit /
    // non-explicit INI scenarios without spinning up the INI
    // parser.
    void SetIniOverridesShimEnableForTesting(bool b) {
        ini_overrides_shim_enable_ = b;
    }

private:
    bool enabled_ = false;
    bool supported_ = false;
    int g_developer_value_ = -1;  // -1 = unprobed; see g_developer_value()
    int walks_entered_ = 0;
    int toggles_dispatched_ = 0;
    int last_dispatch_ms_ = 0;
    // G5.19: count of console-command injections the shim has
    // dispatched (one per Walking-entry in the new code path, or
    // per F10 manual toggle). Used by the F12 dump to confirm how
    // many `free_camera` calls actually went through.
    int console_cmds_dispatched_ = 0;
    // G5.19: when true, TriggerToggle() opens the dev console and
    // types `free_camera` instead of dispatching a single VK.
    // Default true because ATS 1.61.3.1s has no working
    // dev-cam keypress handler. INI knob: `[m2]
    // dev_camera_via_console`.
    bool dev_camera_via_console_ = true;
    unsigned int toggle_vk_ = 0x30;     // VK_0 (top-row zero) — the SCS dev
                                         // free-camera toggle in amtrucks.exe
                                         // 1.61.3.1s when `g_debug_camera = 1`.
                                         // Per Sparked Host help docs and
                                         // YouTube tutorials on the SCS dev
                                         // console: "Press 0 to enter into the
                                         // debug camera and control the camera
                                         // using NUM keys." The action requires
                                         // `g_debug_camera=1` (the cvar that
                                         // unlocks the debug camera path) AND
                                         // `g_developer=1` (the master dev
                                         // switch). WalkerCore's dev_cvar_patcher
                                         // patches both cvars to "1" in memory
                                         // on the first scs_telemetry_init call
                                         // so the user does not have to edit
                                         // config.cfg manually.
                                         //
                                         // History (Oct 2026 cycle):
                                         //   * F11 (0x7A) — previous default,
                                         //     removed: in this ATS build the
                                         //     SCS engine does NOT intercept F11
                                         //     before the action registry, so
                                         //     pressing F11 with
                                         //     `mix camdbg \`keyboard.f11?0\``
                                         //     bound cycled interior cams
                                         //     ("cam 0") instead of opening the
                                         //     dev free camera.
                                         //   * 0x60 (VK_NUMPAD0) — pre-M2.6
                                         //     default, removed: SHAREM
                                         //     did not work either.
                                         //   * 0x30 (top-row `0`) — the
                                         //     actual SCS dev free-camera toggle
                                         //     in 1.61.3.1s (verified via
                                         //     external docs + RE-corrected from
                                         //     earlier M2.6 false premise).
                                         //
                                         // The user's controls.sii does not bind
                                         // `keyboard.0` to any action, so
                                         // dispatching VK_0 has no shadow risk.
                                         // even though the user's controls.sii
                                         // currently binds `keyboard.f11` to
                                         // the `camdbg` action — the SCS dev
                                         // shortcut intercepts F11 before the
                                         // action registry, so the camdbg
                                         // binding is a no-op in dev mode while
                                         // the dev cam toggle wins. Users whose
                                         // controls.sii or mod stack binds F11
                                         // to a non-camera action can
                                         // re-target the dispatch via
                                         // `[m2] dev_camera_toggle_vk = 0xHH`
                                         // in WalkerCore.ini (e.g. F8 = 0x77 to
                                         // avoid any collision). See
                                         // `docs/integration.md` for the full
                                         // M2.7 live verification.
    unsigned int exit_press_vk_ = 0;    // 0 = no separate exit press
    bool exiting_walking_ = false;
    // M2.5: set by PluginEntry when [m2] dev_camera_toggle_vk was
    // explicitly present in the INI (any value, including the default
    // literal). When true, AutoDetectToggleVkFromControlsSii skips the
    // controls.sii probe and the INI value wins. When false, the shim
    // auto-detects from controls.sii if a binding is found and falls
    // back to the hard-coded default 0x30 (top-row `0`, the actual
    // SCS dev free-camera shortcut in 1.61.3.1s when `g_debug_camera
    // = 1`) otherwise.
    bool ini_overrides_toggle_vk_ = false;
    // M2.5 follow-up: did the INI explicitly set
    // `game_free_camera_shim = ...`? Set by PluginEntry before
    // calling MaybeAutoEnableFromDetection. When true, the
    // smart-default auto-enable is skipped — the user has explicitly
    // vetted the value (false or true) and we must not silently flip
    // it. When false (the INI never named the knob), the
    // MaybeAutoEnableFromDetection path is allowed to flip from
    // false → true if both detection gates (binding found AND
    // g_developer supported) are satisfied. See the public method
    // doc for the full rationale.
    bool ini_overrides_shim_enable_ = false;
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
    // G5.22: timestamp of the most recent PreDispatchDevCamToggleFromF10
    // call that actually synthesised at least one key. The OnEnterWalking
    // / OnExitWalking fallback paths consult this counter (via
    // `last_pre_dispatch_ms()`) to skip a duplicate dispatch when the
    // WH_KEYBOARD_LL pre-dispatch already covered the toggle. 0 means
    // no pre-dispatch has happened this session — the OnEnterWalking /
    // OnExitWalking paths fall back to their legacy dispatch behaviour.
    int last_pre_dispatch_ms_ = 0;
    // G5.22 test seam for the foreground-process guard. Set true by
    // the unit tests (which have no real ATS foreground window)
    // before exercising PreDispatchDevCamToggleFromF10; production
    // builds never flip this. Defaults to false so a missing call
    // does not weaken the production safety.
    bool foreground_guard_disabled_for_testing_ = false;
    // M2 diagnostic: last controls.sii path we scanned for binding
    // conflicts. Empty when never scanned. Used by
    // `LogControlsSiiVkConflictIfAny` to short-circuit duplicate
    // scans across re-binds of the same hwnd.
    std::string controls_sii_conflict_last_path_;
    // M2 diagnostic: did the most recent controls.sii scan report
    // any binding conflicts? Used by the F10 dispatch log to flag
    // "the press was sent but a binding shadowed it" without
    // re-scanning the file every F10.
    bool controls_sii_conflict_detected_ = false;
    // M2 diagnostic: copy of the action names bound to toggle_vk_
    // (populated by `LogControlsSiiVkConflictIfAny`). Order matches
    // file order. Empty when no conflict detected. Cached so the
    // F10 dispatch log can include the conflict list without a
    // second file scan.
    std::vector<std::string> controls_sii_conflict_actions_;
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
