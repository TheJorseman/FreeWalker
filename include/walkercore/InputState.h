// WalkerCore — InputState domain type.
//
// The InputState is the snapshot that FrameLoop consumes each tick. It
// contains:
//   - Key states (held / just-pressed / just-released)
//   - Mouse delta (raw input)
//   - Mouse buttons
//
// The Hot key is a virtual key (VK_W, VK_LEFT, etc.) for the Windows API.
// Mapping to "forward / backward / jump / crouch" is the player's
// configuration; the InputState stores raw key states and the FrameLoop
// asks "is the forward key held?" via a keybinding.
//
// Just-pressed / just-released edges are computed in InputHook at the
// start of each frame from the previous frame's snapshot. The polling
// consumer drains the edges by reading the flag, which the InputHook
// clears the next time it polls.
//
// We use raw input for mouse deltas (RAWINPUT via RegisterRawInputDevices)
// rather than WH_MOUSE_LL, because:
//
//   - WH_MOUSE_LL only fires for the foreground window or under specific
//     conditions; the game's window may not be foreground when we read.
//   - WM_INPUT works regardless of focus, as long as the device is
//     registered for our hwnd. We register for the game's hwnd which the
//     SCS SDK exposes via a callback (TODO for M2 lander; for now we
//     guess it from FindWindow or fall back to WH_MOUSE_LL).

#ifndef WALKERCORE_INPUT_STATE_H
#define WALKERCORE_INPUT_STATE_H

#include <stdint.h>

namespace walkercore {

// Virtual key codes we care about. We don't try to cover every key the
// game uses — only the ones the walker needs. Other modules extend as
// required (e.g. M9 trailer coupling may want T).
//
// VK_* codes match the Windows VK_* definitions in <winuser.h>.
enum class WalkercoreKey : unsigned {
    Forward = 0,        // default W
    Backward,           // default S
    StrafeLeft,         // default A
    StrafeRight,        // default D
    Run,                // default Shift
    Crouch,             // default Ctrl
    Jump,               // default Space
    EyeUp,              // default E
    EyeDown,            // default Q
    EyeReset,           // default R
    Interact,           // default F (enter/exit, fuel, etc.)
    ToggleWalk,         // default F10 (also Ctrl+F10 for menu)
    Flashlight,         // default G
    Zoom,               // default LMB (mouse button)
    FlashlightToggle,   // default RMB
    DumpState,          // default F12 — write a state snapshot to the log
    ToggleMenu,         // default Ctrl+F10 — open / close the in-game settings menu
    LastKey,            // sentinel
};

// Bit index for key flags.
inline constexpr unsigned KeyIndex(WalkercoreKey k) {
    return static_cast<unsigned>(k);
}

// Human-readable name for a WalkercoreKey. Used by diagnostic dumps and
// log output. The string is a static literal — callers don't own it.
inline const char *WalkercoreKeyName(WalkercoreKey k) {
    switch (k) {
        case WalkercoreKey::Forward:          return "Forward";
        case WalkercoreKey::Backward:         return "Backward";
        case WalkercoreKey::StrafeLeft:       return "StrafeLeft";
        case WalkercoreKey::StrafeRight:      return "StrafeRight";
        case WalkercoreKey::Run:              return "Run";
        case WalkercoreKey::Crouch:           return "Crouch";
        case WalkercoreKey::Jump:             return "Jump";
        case WalkercoreKey::EyeUp:            return "EyeUp";
        case WalkercoreKey::EyeDown:          return "EyeDown";
        case WalkercoreKey::EyeReset:         return "EyeReset";
        case WalkercoreKey::Interact:         return "Interact";
        case WalkercoreKey::ToggleWalk:       return "ToggleWalk";
        case WalkercoreKey::Flashlight:       return "Flashlight";
        case WalkercoreKey::Zoom:             return "Zoom";
        case WalkercoreKey::FlashlightToggle: return "FlashlightToggle";
        case WalkercoreKey::DumpState:        return "DumpState";
        case WalkercoreKey::ToggleMenu:       return "ToggleMenu";
        case WalkercoreKey::LastKey:          return "LastKey";
    }
    return "(unknown)";
}

struct InputState {
    // Per-key bits. Index by KeyIndex(WalkercoreKey::X).
    // We keep 3 bits per key to track edge events efficiently.
    //   bit 0: held (currently down)
    //   bit 1: pressed since last consume (rising edge)
    //   bit 2: released since last consume (falling edge)
    uint8_t keys[32];

    // Mouse delta since last consume (in raw input units). Consume by
    // reading the value; the InputHook zeroes it on the next sample.
    int mouse_dx;
    int mouse_dy;

    // Mouse wheel delta since last consume. Positive == wheel up.
    int wheel_delta;

    // Time stamp of the last update (microseconds since session start).
    uint64_t last_update_us;

    // True if this struct is filled with fresh data. False until the
    // InputHook has at least one sample.
    bool valid;
};

// Reset to a known state. Cheap.
inline void InputState_Reset(InputState &s) {
    for (auto &b : s.keys) b = 0;
    s.mouse_dx = 0;
    s.mouse_dy = 0;
    s.wheel_delta = 0;
    s.last_update_us = 0;
    s.valid = false;
}

// Read helpers.
inline bool IsKeyHeld(const InputState &s, WalkercoreKey k) {
    return (s.keys[KeyIndex(k)] & 0x01) != 0;
}
inline bool IsKeyPressed(const InputState &s, WalkercoreKey k) {
    return (s.keys[KeyIndex(k)] & 0x02) != 0;
}
inline bool IsKeyReleased(const InputState &s, WalkercoreKey k) {
    return (s.keys[KeyIndex(k)] & 0x04) != 0;
}

}  // namespace walkercore

#endif  // WALKERCORE_INPUT_STATE_H