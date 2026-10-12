// WalkerCore — InputHook implementation.
//
// M4 prereq. Real implementation strategy:
//
//   Keyboard:
//     WH_KEYBOARD_LL via SetWindowsHookEx(WH_KEYBOARD_LL, ...). The
//     callback receives KBDLLHOOKSTRUCT. We update an internal shadow of
//     key states. We DO NOT swallow the event (we return CallNextHookEx)
//     unless M5 wires a "block the game from receiving this key while
//     in walk mode" policy.
//
//   Mouse:
//     RegisterRawInputDevices on the game's hwnd, RIDEV_INPUTSINK so we
//     receive WM_INPUT even when not in foreground. Process WM_INPUT in
//     a hidden message-only window that pumps the message queue
//     continuously. Each sample captures the delta from the last sample.
//
//   The message-pump window is created in Install() and destroyed in
//     Uninstall(). It lives on the game thread (no separate thread, so
//     no thread sync needed).
//
// M4 SCOPE here: STUB. We register the keyboard hook (it's a one-liner
// API) so the dll is exercised at runtime, but we don't implement the
// mouse pump yet (it needs a window). Once M2 lander wires up the game's
// hwnd, we drop in RegisterRawInputDevices.
//
// Reason for partial impl: WH_KEYBOARD_LL is a low-risk hook, easy to
// wire. WM_INPUT needs a window on the game thread, which is one more
// thing to coordinate. We'll do it as part of M2 when we already touch
// the game's window.

#include "walkercore/InputHook.h"

#include <windows.h>

#include "walkercore/Log.h"

namespace walkercore {

// Singleton so the WndProc callback can find its instance. Set in
// Install() when there is exactly one InputHook per plugin; matches the
// "WalkerCore runs on the game thread, no thread sync needed" rule.
namespace {
InputHook *g_instance = nullptr;
}  // namespace

// Low-level keyboard hook callback. Translates VK -> WalkercoreKey bits
// in our last_ snapshot. Returning 0 (no CallNextHookEx bypass) keeps the
// event visible to the game; if M5 wants to swallow keys, that goes in
// here.
LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM w_param, LPARAM l_param) {
    if (code != HC_ACTION || g_instance == nullptr) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const KBDLLHOOKSTRUCT *kb = reinterpret_cast<KBDLLHOOKSTRUCT *>(l_param);
    const bool is_down = (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN);
    const bool is_up = (w_param == WM_KEYUP || w_param == WM_SYSKEYUP);

    if (!is_down && !is_up) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }

    // Translate VK to WalkercoreKey.
    const int vk = static_cast<int>(kb->vkCode);
    const auto &b = g_instance->bindings();
    WalkercoreKey k = WalkercoreKey::LastKey;  // sentinel: not a walker key
    if (vk == b.forward_vk) k = WalkercoreKey::Forward;
    else if (vk == b.backward_vk) k = WalkercoreKey::Backward;
    else if (vk == b.strafe_left_vk) k = WalkercoreKey::StrafeLeft;
    else if (vk == b.strafe_right_vk) k = WalkercoreKey::StrafeRight;
    else if (vk == b.run_vk) k = WalkercoreKey::Run;
    else if (vk == b.crouch_vk) k = WalkercoreKey::Crouch;
    else if (vk == b.jump_vk) k = WalkercoreKey::Jump;
    else if (vk == b.eye_up_vk) k = WalkercoreKey::EyeUp;
    else if (vk == b.eye_down_vk) k = WalkercoreKey::EyeDown;
    else if (vk == b.eye_reset_vk) k = WalkercoreKey::EyeReset;
    else if (vk == b.interact_vk) k = WalkercoreKey::Interact;
    else if (vk == b.toggle_walk_vk) {
        // Don't return early — fall through to the bit-update block so
        // the held bit gets updated and the rising-edge logic can also
        // mark `pressed` correctly. The toggle-walk side effect (the
        // FSM's pending flag) lives in the bit-update code too.
        // Ctrl+F10 is the documented menu binding — held crouch (which
        // defaults to VK_CONTROL) routes the press to ToggleMenu instead
        // of ToggleWalk.
        k = InputHook::ClassifyToggleWalkKey(
            IsKeyHeld(g_instance->last(), WalkercoreKey::Crouch));
    } else if (vk == b.flashlight_vk) k = WalkercoreKey::Flashlight;
    else if (vk == b.dump_state_vk) k = WalkercoreKey::DumpState;

    if (k == WalkercoreKey::LastKey) {
        // Not a walker-controlled key. Let it through.
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }

    // Update the last_ snapshot bit. Rising / falling edges are derived
    // from the held bit BEFORE we mutate it; this gives correct edge
    // semantics under Windows key-repeat (otherwise a held key would
    // look pressed every frame). The consumer drains the edges via
    // Sample() which clears bit 1 / bit 2 in the source so the next
    // accumulator starts fresh.
    uint8_t &bits = g_instance->mutable_last().keys[KeyIndex(k)];
    const bool was_held = (bits & 0x01) != 0;
    if (is_down) {
        bits = static_cast<uint8_t>(bits | 0x01);
        if (!was_held) {
            bits = static_cast<uint8_t>(bits | 0x02);  // rising edge → mark pressed
        }
    } else {
        if (was_held) {
            bits = static_cast<uint8_t>(bits | 0x04);  // falling edge → mark released
        }
        bits = static_cast<uint8_t>(bits & ~0x01);
    }

    return CallNextHookEx(nullptr, code, w_param, l_param);
}

InputHook::InputHook() {
    InputState_Reset(last_);
}

InputHook::~InputHook() {
    if (installed_) {
        Uninstall();
    }
}

void InputHook::SetBindings(const KeyBindings &b) {
    bindings_ = b;
}

bool InputHook::Install(HWND game_window) {
    if (installed_) {
        return false;
    }
    raw_target_ = game_window;

    kb_hook_ = SetWindowsHookExW(
        WH_KEYBOARD_LL, &LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    if (kb_hook_ == nullptr) {
        const DWORD err = GetLastError();
        WALKERCORE_LOG_ERROR(
            "InputHook::Install: SetWindowsHookEx(WH_KEYBOARD_LL) failed: error %lu",
            err);
        return false;
    }

    g_instance = this;
    installed_ = true;
    last_.valid = true;

    WALKERCORE_LOG_INFO(
        "InputHook::Install: keyboard hook live%s",
        (game_window != nullptr)
            ? " (mouse WM_INPUT pending M2 lander)"
            : " (no game hwnd yet; mouse WM_INPUT will need a re-Install)");

    return true;
}

void InputHook::Uninstall() {
    if (!installed_) {
        return;
    }
    if (kb_hook_ != nullptr) {
        UnhookWindowsHookEx(kb_hook_);
        kb_hook_ = nullptr;
    }
    if (g_instance == this) {
        g_instance = nullptr;
    }
    installed_ = false;
    raw_target_ = nullptr;
    InputState_Reset(last_);
}

void InputHook::Sample(InputState &out) {
    // Copy the most recent accumulated snapshot to the consumer. The
    // consumer reads rising / falling edges from `out` immediately after
    // this returns.
    out = last_;
    // Clear the edge bits in the SOURCE (last_) so the next call to
    // Sample() starts from a fresh accumulator. We do NOT clear them in
    // `out` — that would erase the rising edge in the same frame the
    // consumer is reading it, and the FSM would never see F10 / Shift /
    // Space being pressed.
    for (auto &b : last_.keys) {
        b &= 0x01;  // keep only the held bit
    }
}

}  // namespace walkercore