// WalkerCore — InputHook provider.
//
// M4 prereq. Windows-side keyboard + mouse input capture.
//
// Strategy:
//   - Keyboard: WH_KEYBOARD_LL (low-level keyboard hook) installed via
//     SetWindowsHookEx. We see the OS-wide stream of key events. To avoid
//     stealing input from the game we walk, the hook swallows events that
//     fall within the walker's keyboard area while walk mode is active.
//   - Mouse movement: WM_INPUT via RegisterRawInputDevices on the game's
//     window handle. The hwnd is provided by the SDK in M2 lander (it
//     exposes the window via scs_sdk_init_params_v100_t callbacks); for
//     M4 we discover it via FindWindowEx on the game's window class.
//
// Key bindings are stored in a small table (WalkerCore -> Win32 VK).
// The table is populated from defaults and overridden by the M6 INI.

#ifndef WALKERCORE_INPUT_HOOK_H
#define WALKERCORE_INPUT_HOOK_H

// Windows types (HWND, HHOOK) and VK_* macros used in the header below.
// The header is safe to include on non-Windows hosts — the implementation
// file is the only one that calls Windows APIs.
#ifdef _WIN32
#include <windows.h>
#endif

#include "walkercore/InputState.h"

namespace walkercore {

// Default key bindings (Win32 VK_*). Used until M6 INI overrides them.
struct KeyBindings {
    int forward_vk = 'W';
    int backward_vk = 'S';
    int strafe_left_vk = 'A';
    int strafe_right_vk = 'D';
    int run_vk = VK_SHIFT;
    int crouch_vk = VK_CONTROL;
    int jump_vk = VK_SPACE;
    int eye_up_vk = 'E';
    int eye_down_vk = 'Q';
    int eye_reset_vk = 'R';
    int interact_vk = 'F';
    int toggle_walk_vk = VK_F10;
    int flashlight_vk = 'G';
    int zoom_mouse_button = 0;     // VK_LBUTTON
    int flashlight_toggle_mouse_button = 1;  // VK_RBUTTON
    int dump_state_vk = VK_F12;    // triggers walker_dump_state()
};

class InputHook {
public:
    InputHook();
    ~InputHook();

    InputHook(const InputHook &) = delete;
    InputHook &operator=(const InputHook &) = delete;

    // Update key bindings. Safe to call at any time. The change takes
    // effect on the next sample.
    void SetBindings(const KeyBindings &b);

    // Install the hooks. Idempotent. The game's window handle is required
    // for WM_INPUT; pass nullptr to use FindWindow fallback.
    bool Install(HWND game_window);

    // Remove hooks. Safe to call when not installed.
    void Uninstall();

    bool isInstalled() const { return installed_; }

    // Drain the latest sample into out. The hook continues to record
    // events while the consumer reads; calling Sample() multiple times
    // in the same frame returns the same snapshot (after the first call,
    // edge bits are cleared in the source and the consumer sees zero
    // edges on subsequent reads).
    void Sample(InputState &out);

    // Accessors used by the WH_KEYBOARD_LL callback. Public so the
    // free-function callback in the .cpp can reach them without needing
    // friend declarations.
    const KeyBindings &bindings() const { return bindings_; }
    InputState &mutable_last() { return last_; }

    // Read-only snapshot for diagnostics. The hook continues to update
    // last_ concurrently, but each field is a small POD read — torn
    // reads are not observable in practice on x86, and the dump prints
    // an "approximate" snapshot by design.
    const InputState &last() const { return last_; }

    // Mutable accessor — test code writes a fixture snapshot so the
    // focused input dump has meaningful rows to surface. Production
    // never uses this path; the WH_KEYBOARD_LL callback writes to
    // last_ through the concurrency-safe internal setters.
    InputState &last() { return last_; }

    // Pure classifier used by the keyboard callback. Returns which
    // WalkercoreKey a fresh F10 press should map to based on whether the
    // crouch (default Ctrl) is currently held. Held → ToggleMenu;
    // otherwise → ToggleWalk. Exposed as a static so tests can drive it
    // without spinning up a WH_KEYBOARD_LL hook.
    static WalkercoreKey ClassifyToggleWalkKey(bool crouch_held) {
        return crouch_held ? WalkercoreKey::ToggleMenu
                            : WalkercoreKey::ToggleWalk;
    }

private:
    KeyBindings bindings_;
    HHOOK kb_hook_ = nullptr;
    HWND raw_target_ = nullptr;
    bool installed_ = false;

    // Internal state — last sample snapshot. The hook updates this from
    // its message loop, and Sample() returns a copy.
    InputState last_;
};

}  // namespace walkercore

#endif  // WALKERCORE_INPUT_HOOK_H