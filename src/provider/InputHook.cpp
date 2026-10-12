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
#include "walkercore/WindowSubclass.h"

namespace walkercore {

// Singleton so the WndProc callback can find its instance. Set in
// Install() when there is exactly one InputHook per plugin; matches the
// "WalkerCore runs on the game thread, no thread sync needed" rule.
namespace {
InputHook *g_instance = nullptr;

// M11.4 — trampoline that the WndProc subclass calls on every
// WM_INPUT message. WindowSubclass_SetRawInputSink takes a (fn, ctx)
// pair where ctx is the InputHook instance and fn is this static.
// The trampoline is a free function so the function pointer has
// stable linkage; ctx carries the per-instance state. The function
// is a no-op when ctx is null (defensive — the WndProc already
// null-checks the fn pointer before calling, but a clear that
// happens between the check and the call would still land here).
void OnRawInputMessageTrampoline(void *ctx, LPARAM l_param) {
    if (ctx == nullptr) return;
    static_cast<InputHook *>(ctx)->OnRawInputMessage(l_param);
}
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

    // M11.4 — register this InputHook as the raw-input sink for the
    // WndProc subclass. The WndProc is installed later by
    // ImGuiOverlay::InitializeForD3D11Device (when D3D11Hook captures
    // the game hwnd). Registering the sink now — before the subclass
    // is installed — is safe: the WindowSubclass stores the (fn, ctx)
    // pair and only uses it once a WndProc subclass is alive. When
    // the D3D11 hook captures the hwnd and the subclass is installed,
    // the WndProc sees the registered sink on every WM_INPUT and
    // forwards via the trampoline.
    WindowSubclass_SetRawInputSink(&OnRawInputMessageTrampoline, this);

    WALKERCORE_LOG_INFO(
        "InputHook::Install: keyboard hook live%s",
        (game_window != nullptr)
            ? " (raw-input sink registered; WM_INPUT will route once "
              "the D3D11 hook installs the WndProc subclass)"
            : " (no game hwnd yet; raw-input sink registered globally "
              "so the WndProc subclass will route when it lands)");

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
    // M11.4 — clear the raw-input sink so the WndProc subclass
    // (which is uninstalled by ImGuiOverlay::Shutdown) no longer
    // dispatches to a stale this pointer. The trampoline would
    // still call OnRawInputMessage on a dangling pointer if the
    // sink were not cleared before the subclass stays alive
    // past our destruction.
    if (WindowSubclass_IsInstalled(nullptr)) {
        WindowSubclass_SetRawInputSink(nullptr, nullptr);
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
    // M4 MousePump: drain the per-frame motion / wheel deltas. Without
    // this drain the deltas would accumulate forever across Sample()
    // calls and PhaseApplyMouseLook would integrate the entire session's
    // mouse motion into a single yaw / pitch jump on the first Walking
    // frame. Mouse-button held / edge bits live in the keys[] array
    // and are drained by the loop above — only the motion accumulators
    // need an explicit zero.
    last_.mouse_dx = 0;
    last_.mouse_dy = 0;
    last_.wheel_delta = 0;
    last_.valid = true;
}

void InputHook::OnRawInputData(const RAWINPUT &raw) {
    // M4 MousePump — accumulate one parsed RAWINPUT event into last_.
    //
    // The structure is the union member from the OS — we only handle
    // RIM_TYPEMOUSE here. Keyboard events are already covered by the
    // WH_KEYBOARD_LL hook (it owns the keys[] bits end-to-end) and HID
    // events are not used by the walker. Filtering by header.dwType is
    // the canonical discriminator.
    if (raw.header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    const RAWMOUSE &m = raw.data.mouse;

    // Motion: usFlags & MOUSE_MOVE_ABSOLUTE means the OS reports absolute
    // coordinates (touchscreens / tablets); we only integrate relative
    // motion. Without this guard a tablet would inject a single huge dx
    // on first contact and yank the camera across the world.
    if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
        last_.mouse_dx += static_cast<int>(m.lLastX);
        last_.mouse_dy += static_cast<int>(m.lLastY);
    }

    // Wheel: usButtonFlags & RI_MOUSE_WHEEL means usButtonData is the
    // wheel delta in WHEEL_DELTA units (positive == wheel rotated away
    // from the user == "scroll up"). The cast through int16_t matches
    // the WM_MOUSEWHEEL wParam high-word contract; usButtonData is a
    // WORD (16-bit signed) per the RAWHID / RAWMOUSE union.
    if (m.usButtonFlags & RI_MOUSE_WHEEL) {
        last_.wheel_delta += static_cast<int16_t>(m.usButtonData);
    }

    // Button transitions: route the per-button press / release flags to
    // the matching WalkercoreKey slot in keys[]. The held bit is
    // derived from the transition (rising → held becomes true, falling →
    // held becomes false) so a raw report that only fires press / release
    // edges (which is what Windows actually sends) updates the same
    // per-key invariant the WH_KEYBOARD_LL callback maintains. We
    // route only LMB and RMB to the existing Zoom / FlashlightToggle
    // slots — middle / X1 / X2 are not used by the walker.
    auto update_button = [this, &m](USHORT pressed_flag, USHORT released_flag,
                                    WalkercoreKey k) {
        if ((m.usButtonFlags & (pressed_flag | released_flag)) == 0) {
            return;
        }
        uint8_t &bits = last_.keys[KeyIndex(k)];
        const bool was_held = (bits & 0x01) != 0;
        if (m.usButtonFlags & pressed_flag) {
            bits = static_cast<uint8_t>(bits | 0x01);
            if (!was_held) {
                bits = static_cast<uint8_t>(bits | 0x02);
            }
        } else {
            if (was_held) {
                bits = static_cast<uint8_t>(bits | 0x04);
            }
            bits = static_cast<uint8_t>(bits & ~0x01);
        }
    };
    update_button(RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP,
                  WalkercoreKey::Zoom);
    update_button(RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP,
                  WalkercoreKey::FlashlightToggle);
}

void InputHook::OnRawInputMessage(LPARAM l_param) {
    // M4 MousePump — production path. Called from a WndProc subclass on
    // WM_INPUT with the OS-delivered LPARAM (an HRAWINPUT). Two calls:
    //   1. GetRawInputData with pcbSize=NULL returns the buffer size
    //      required to hold the decoded RAWINPUT.
    //   2. GetRawInputData with a sized buffer decodes the message.
    //
    // We pre-size to a single RAWINPUT (which is what the OS guarantees
    // for one WM_INPUT); if a HID report ever exceeded that the second
    // call would return -1 and we silently drop. The walker doesn't use
    // HID and the pre-sized buffer is plenty for mouse / keyboard
    // reports.
    RAWINPUT raw{};
    UINT size = sizeof(RAWINPUT);
    const UINT got = GetRawInputData(reinterpret_cast<HRAWINPUT>(l_param),
                                     RID_INPUT, &raw, &size,
                                     sizeof(RAWINPUTHEADER));
    if (got == static_cast<UINT>(-1) || got == 0) {
        // OS couldn't decode the message (truncated buffer, invalid
        // handle, etc.) — drop silently. The walker is a passive
        // observer; we never want a malformed WM_INPUT to crash the
        // game thread.
        return;
    }
    OnRawInputData(raw);
}

}  // namespace walkercore