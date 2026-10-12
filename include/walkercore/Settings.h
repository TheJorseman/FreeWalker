// WalkerCore — user-tunable configuration.
//
// M6 scope: parse + persist + reload-on-change the WalkerCore.ini located at
// %APPDATA%\WalkerCore\WalkerCore.ini.
//
// The format is hand-written (no third-party INI library) — keep it minimal:
//
//   # comment
//   [section]
//   key = value
//
// Sections we read:
//   [walker]   — locomotion + camera knobs
//   [keys]     — VK_* overrides for the walker key map
//   [truck]    — per-truck door geometry overrides (placeholder)
//
// We don't validate exhaustively; unknown keys are logged once and ignored.
// Unknown values fall back to defaults — the user can copy the defaults
// from the comments in the file WalkerCore writes on first run.

#ifndef WALKERCORE_SETTINGS_H
#define WALKERCORE_SETTINGS_H

#include <stdint.h>

#include <string>

#include "walkercore/InputHook.h"     // for KeyBindings
#include "walkercore/MovementConfig.h"
#include "walkercore/PolishProviders.h"  // for HeadTrackerConfig
#include "walkercore/TruckDoorConfig.h"

namespace walkercore {

// Aggregates all user-tunable settings in one POD struct. The SettingsStore
// is responsible for reading/writing this from disk and applying it to the
// bound FrameLoop / InputHook / WalkerFsm / HeadTrackerProvider.
struct WalkerSettings {
    MovementConfig movement;
    KeyBindings keys;
    TruckDoorConfig door_config;
    HeadTrackerConfig head_tracker;
    float transition_seconds = 0.6f;
    float horizontal_fov_radians = 1.4f;  // ~80°
    float near_z = 0.1f;
    float far_z = 4000.0f;
    // M2 Game-Free-Camera shim gate. When `true`, F10 also synthesises
    // the configured key sequence on Driving → Walking so the SCS
    // developer free camera takes over (assuming `g_developer` is "1"
    // in the game's config.cfg). Default `false` so out-of-the-box
    // M2 lander: when true, the F10 (Driving -> Walking) transition
    // also synthesises a Numpad-0 keypress (via PostMessage to the
    // bound game HWND) so the SCS developer free camera takes over
    // — that's what makes "F10 produces a visible camera change"
    // work for the user. Requires `uset g_developer "1"` in the
    // game's config.cfg (the user already has it). Default `true`
    // because F10 -> free-camera-toggle is the core M2 lander
    // observable; without the shim, F10 only does the FSM
    // transition (which the user can't see). The M11 settings menu
    // can flip it at runtime via the exported ABI.
    bool m2_game_free_camera_shim = true;
    // M2 long-term camera hijack gate. False by default — opt in via
    // `[m2] camera_hijack = 1`. When enabled, WalkerCore captures
    // amtrucks.exe's image base via GetModuleHandleA and MinHooks the
    // camera state dispatcher at RVA 0xC22F40 (function
    // 0x140C22F40, identified by REA/scripts/tmrealwalk-re/
    // find_camera_function.py + find_vtable_slot16.py). v2 will
    // replace the matrices the SCS state machine writes to the GPU
    // constant buffer.
    bool m2_camera_hijack = false;
    // M2 lander: D3D11 hook gate. Default `false` — the original
    // MinHook-install path (D3D11 hook has been live since M2 lander
    // landed) is opt-in only because on the worst-known ATS mod
    // stacks (amtrucks.exe 1.61.3.1s + ReShade + OptiScaler + RenoDX)
    // the SYSTEM32 d3d11.dll MinHook-patch corrupts downstream
    // 16-byte alignment and triggers a `movdqa` `#GP` at RVA
    // 0x1d5300a in the game's render path. Shipping with the gate
    // off by default means the worst-case stack runs out of the
    // box. Set `[m2] d3d11_hook = true` in the INI to opt back in
    // when the user's stack is known to be clean. When the gate is
    // false, the Install() call is a no-op and a single "SKIPPED
    // (INI gate [m2] d3d11_hook = false)" line is logged. The
    // PerFrameThread (60 Hz ticker) keeps running regardless, so
    // the FSM and per-frame sim are unaffected. The M11 ImGui
    // overlay (which targets the D3D11 device capture) will
    // surface `supported=false` while the gate is off, until the
    // D3D12 path is fixed in a later lander.
    bool m2_d3d11_hook = false;
    // M2 lander (D3D12 path): DXGI factory + Present hook gate.
    // Default `false` for the same reason as m2_d3d11_hook above.
    // Set `[m2] d3d12_hook = true` in the INI to opt back in when
    // the user's stack is known to be clean.
    bool m2_d3d12_hook = false;
    // M2 long-term: in-process MinHook on the SCS camera state
    // dispatcher at RVA 0xC22F40 in amtrucks.exe. The hook is
    // observation-only (v1+v2 scope: counts calls, surfaces state
    // in the F12 dump; does not write to GPU). Default `false` —
    // the MinHook trampoline itself has been observed to break
    // downstream 16-byte alignment on ATS 1.61.3.1s (same
    // amtrucks.exe+0x1d5300a `movdqa` #GP as the d3d11/d3d12 hooks,
    // but triggered by the trampoline rather than the SYSTEM32
    // DLL patch). Set `[m2] camera_hijack_install = true` in the
    // INI to opt in on a known-clean stack. When the gate is
    // false, the Install() is a no-op and the SCS state machine
    // drives the camera unobserved. The shim's PostMessage path
    // (which dispatches Numpad-0 to the game window on F10
    // Walking entry) still works — the dev free camera activates
    // the same way manual Numpad-0 would.
    bool m2_camera_hijack_install = false;

    // M11 ImGui overlay gate. False by default — opt in via
    // `[m11] imgui_overlay = true` in `WalkerCore.ini`. When true,
    // the overlay renders a status HUD on top of the game's swap
    // chain (the actual D3D11 Present-path integration is a TODO;
    // the surface + ABI are wired in this commit).
    bool m11_imgui_overlay = false;

    // M3 lander: in-process MinHook on PxScene::addActor at RVA
    // 0x1AE0B62 in amtrucks.exe. The hook captures the live
    // PxScene instance pointer (waiting for the first addActor
    // call from the game) so the M3 lander can query the PhysX
    // scene for collision meshes, actor counts, etc. Default
    // `false` — the MinHook trampoline has been observed to break
    // downstream 16-byte alignment on ATS 1.61.3.1s (same
    // amtrucks.exe+0x1d5300a `movdqa` #GP as the other in-process
    // MinHooks). Set `[m3] px_scene_hook_install = true` in the
    // INI to opt in on a known-clean stack. When the gate is
    // false, the Install() is a no-op and the M3 lander remains a
    // no-hit stub (the world's sphere-sweep returns no collision,
    // the integrator's Y-clamp keeps the player from falling to
    // -∞). M3 functionality is not yet required for the F10
    // Walking transition to work — only the per-frame loop + FSM
    // + shim are.
    bool m3_px_scene_hook_install = false;

    // Returns the same values as the inline defaults in MovementConfig +
    // KeyBindings + HeadTrackerConfig. Centralised so Save() produces a
    // known-good file even when the bound objects have not been wired yet.
    static WalkerSettings Defaults();
};

// Where the INI file lives. Always returns a non-empty absolute path. On
// Windows it's %APPDATA%\WalkerCore\WalkerCore.ini; if %APPDATA% is unset
// we use the working directory.
std::string DefaultSettingsPath();

// Make sure the parent directory exists. Returns true on success or if it
// already exists. Used by SaveToPath before opening the file for write.
bool EnsureDirectoryExists(const std::string &path);

class SettingsStore {
public:
    SettingsStore();
    ~SettingsStore();

    SettingsStore(const SettingsStore &) = delete;
    SettingsStore &operator=(const SettingsStore &) = delete;

    // Bind the consumers that ApplyAll() pushes settings into. The store
    // does not own them.
    void BindFrameLoop(class FrameLoop *loop);
    void BindInput(class InputHook *input);
    void BindFsm(class WalkerFsm *fsm);
    void BindHeadTracker(class HeadTrackerProvider *head);

    // Load current_ from disk (DefaultSettingsPath). Missing file is not
    // an error; we keep Defaults(). Logs the path used.
    bool LoadFromDefaultPath();
    bool LoadFromPath(const std::string &path);

    // Write current_ to disk. Creates parent directories as needed.
    // Returns false if the OS refused the write (e.g. read-only profile).
    bool SaveToDefaultPath() const;
    bool SaveToPath(const std::string &path) const;

    // Push current_ into all bound consumers. Idempotent.
    void ApplyAll();

    // Polling-based reload. Returns true if the file changed since the
    // last time we polled. Self-throttles to at most one GetFileAttributesEx
    // syscall per kCheckPollingIntervalUs (250 ms — fast enough that the
    // user never notices the polling delay when they save the INI, but
    // 240× cheaper than the 60 Hz call rate the per-frame conductor
    // would otherwise drive). The throttle is the source-of-truth: the
    // docstring previously said "once per second" but the conductor
    // actually polls every frame at 60 Hz; this enforces a sane cadence.
    //
    // Worst-case edit-to-detect latency is kCheckPollingIntervalUs
    // (~250 ms). That's much faster than human-perceptible and order-of-
    // magnitude better than the 1–17 s latency the previous always-poll
    // version had at idle.
    //
    // The throttle also runs BEFORE the path_/mtime cache check, so
    // rapid calls inside one 250 ms window return false without any
    // syscall and without touching last_mtime_us_.
    bool CheckFileChanged();

    // Reset the polling throttle so the very next CheckFileChanged()
    // does an actual GetFileAttributesEx + mtime compare. Production
    // code does not call this; tests use it to drive multiple
    // meaningful polls in rapid succession without waiting for the
    // throttle window to expire.
    void ResetLastCheckTimerForTesting();

    // Polling cadence in microseconds. Public so the per-frame conductor
    // (PluginEntry.cpp) and tests can read the value without copying
    // the constant. 250 ms = 4 polls/sec, plenty for any human-driven
    // INI edit.
    static constexpr uint64_t kCheckPollingIntervalUs = 250 * 1000;

    // Snap last_mtime_us_ to the file's current mtime. Called internally by
    // LoadFromPath after a successful read so the first CheckFileChanged()
    // after load doesn't falsely report a change. Exposed for tests that
    // exercise the mtime path directly.
    void PrimeMtimeCache();

    const WalkerSettings &current() const { return current_; }
    void SetCurrent(const WalkerSettings &s);

private:
    WalkerSettings current_;
    std::string path_;
    uint64_t last_mtime_us_ = 0;
    // Wall-clock (NowMicros) of the last CheckFileChanged() that ACTUALLY
    // polled the filesystem. Throttle window: skip syscall + mtime compare
    // when (now - last_check_us_) < kCheckPollingIntervalUs. Initialised
    // to 0 so the first call always polls.
    uint64_t last_check_us_ = 0;

    FrameLoop *frame_loop_ = nullptr;
    InputHook *input_ = nullptr;
    WalkerFsm *fsm_ = nullptr;
    HeadTrackerProvider *head_tracker_ = nullptr;
};

// -----------------------------------------------------------------------------
// Parser helpers (exposed for tests).
// -----------------------------------------------------------------------------

// Parse a single line. Returns true if the line was a key=value pair and
// writes the key/value out. Comments and blank lines return false.
bool TryParseKeyValue(const char *line,
                       std::string *out_key,
                       std::string *out_value);

// Try to interpret `value` as one of the supported scalar types. Returns
// true on success and writes the parsed value to *out.
bool TryParseFloat(const std::string &value, float *out);
bool TryParseDouble(const std::string &value, double *out);
bool TryParseInt(const std::string &value, int *out);
bool TryParseBool(const std::string &value, bool *out);

}  // namespace walkercore

#endif  // WALKERCORE_SETTINGS_H