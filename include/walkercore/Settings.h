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
    // behaviour stays unchanged. The M11 settings menu can flip it
    // at runtime via the exported ABI.
    bool m2_game_free_camera_shim = false;

    // M11 ImGui overlay gate. False by default — opt in via
    // `[m11] imgui_overlay = true` in `WalkerCore.ini`. When true,
    // the overlay renders a status HUD on top of the game's swap
    // chain (the actual D3D11 Present-path integration is a TODO;
    // the surface + ABI are wired in this commit).
    bool m11_imgui_overlay = false;

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