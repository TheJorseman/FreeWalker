// WalkerCore — InputHook tests.
//
// The InputHook is normally driven by a real WH_KEYBOARD_LL callback.
// We don't install one in tests (that would require a message pump and
// is platform-specific); instead we drive `last_` directly via the
// public `mutable_last()` accessor, then exercise `Sample()` to verify
// the rising/falling edge semantics.
//
// The key invariant we test: a rising edge set into last_ MUST be
// visible to the consumer on the very next Sample() call, and MUST NOT
// remain visible on the Sample() after that. This protects against the
// regression where Sample() cleared edges in the output copy (instead
// of the source), causing the FSM to never see F10 / Shift / Space
// being pressed.

#include "TestHarness.h"
#include "walkercore/InputHook.h"
#include "walkercore/InputState.h"

namespace walkercore {

namespace {

// Simulate a key-down on the given key, setting the held bit and the
// rising-edge bit in last_. Mirrors the production code in
// LowLevelKeyboardProc.
void SimulateKeyDown(InputHook &hook, WalkercoreKey k) {
    uint8_t &bits = hook.mutable_last().keys[KeyIndex(k)];
    const bool was_held = (bits & 0x01) != 0;
    bits |= 0x01;
    if (!was_held) {
        bits |= 0x02;
    }
}

// Simulate a key-up, clearing the held bit and setting the falling edge.
void SimulateKeyUp(InputHook &hook, WalkercoreKey k) {
    uint8_t &bits = hook.mutable_last().keys[KeyIndex(k)];
    const bool was_held = (bits & 0x01) != 0;
    if (was_held) {
        bits |= 0x04;
    }
    bits &= ~0x01;
}

}  // namespace

void TestInputHookRisingEdgeVisible() {
    // After a fresh key-down, the very next Sample() MUST show the
    // pressed bit. This is the regression we just fixed: Sample() used
    // to clear the edge in the copy, so the consumer never saw it.
    InputHook hook;
    SimulateKeyDown(hook, WalkercoreKey::ToggleWalk);

    InputState out;
    hook.Sample(out);
    EXPECT_TRUE(IsKeyPressed(out, WalkercoreKey::ToggleWalk));
    EXPECT_TRUE(IsKeyHeld(out, WalkercoreKey::ToggleWalk));
}

void TestInputHookRisingEdgeConsumedAfterSample() {
    // After one Sample(), the source's rising-edge flag MUST be drained,
    // so the next Sample() returns the same held state but no edge.
    // (This is what makes "F10 pressed once" toggle exactly once and
    // not every frame.)
    InputHook hook;
    SimulateKeyDown(hook, WalkercoreKey::Interact);

    InputState out1;
    hook.Sample(out1);
    EXPECT_TRUE(IsKeyPressed(out1, WalkercoreKey::Interact));

    // No new key event arrives.
    InputState out2;
    hook.Sample(out2);
    EXPECT_FALSE(IsKeyPressed(out2, WalkercoreKey::Interact));
    EXPECT_TRUE(IsKeyHeld(out2, WalkercoreKey::Interact));
}

void TestInputHookFallingEdgeVisible() {
    // A key-up fires a falling edge that survives one Sample() and is
    // then cleared. This is used by IsKeyReleased checks (M5+).
    InputHook hook;
    SimulateKeyDown(hook, WalkercoreKey::Run);
    SimulateKeyUp(hook, WalkercoreKey::Run);

    InputState out;
    hook.Sample(out);
    EXPECT_TRUE(IsKeyReleased(out, WalkercoreKey::Run));
    EXPECT_FALSE(IsKeyHeld(out, WalkercoreKey::Run));

    // Subsequent Sample drains the edge.
    InputState out2;
    hook.Sample(out2);
    EXPECT_FALSE(IsKeyReleased(out2, WalkercoreKey::Run));
}

void TestInputHookKeyRepeatNoExtraEdges() {
    // Holding the key fires WM_KEYDOWN repeatedly (Windows key-repeat).
    // Each repeat after the first should NOT set a new rising edge —
    // only the first one does. This protects the toggle-walk from firing
    // every frame the user holds F10.
    InputHook hook;
    SimulateKeyDown(hook, WalkercoreKey::ToggleWalk);

    // First Sample consumes the rising edge.
    InputState out1;
    hook.Sample(out1);
    EXPECT_TRUE(IsKeyPressed(out1, WalkercoreKey::ToggleWalk));

    // Simulate key-repeat: hook callback fires again with bit 0 still
    // set, so the rising-edge guard skips.
    uint8_t &bits = hook.mutable_last().keys[KeyIndex(WalkercoreKey::ToggleWalk)];
    const bool was_held = (bits & 0x01) != 0;
    bits |= 0x01;
    if (!was_held) {
        bits |= 0x02;  // would be wrong; this branch must not execute
    }
    EXPECT_TRUE(was_held);

    InputState out2;
    hook.Sample(out2);
    EXPECT_FALSE(IsKeyPressed(out2, WalkercoreKey::ToggleWalk));
    EXPECT_TRUE(IsKeyHeld(out2, WalkercoreKey::ToggleWalk));
}

void TestInputHookMultipleKeysIndependent() {
    // Edges on different keys must not interfere.
    InputHook hook;
    SimulateKeyDown(hook, WalkercoreKey::Forward);
    SimulateKeyDown(hook, WalkercoreKey::Jump);

    InputState out;
    hook.Sample(out);
    EXPECT_TRUE(IsKeyPressed(out, WalkercoreKey::Forward));
    EXPECT_TRUE(IsKeyPressed(out, WalkercoreKey::Jump));
    EXPECT_FALSE(IsKeyPressed(out, WalkercoreKey::Run));
}

void TestInputHookDumpStateKeyDefaultIsF12() {
    // The diagnostic-dump binding is a user-facing contract — F12 is
    // expected by every documentation, README and screenshot in the
    // repo. Pin the default so accidental edits get caught.
    InputHook hook;
    EXPECT_EQ(static_cast<int>(VK_F12), hook.bindings().dump_state_vk);
}

void TestInputHookDumpStateRisingEdgeConsumed() {
    // F12 press sets the DumpState rising edge; the next Sample() shows
    // it, the Sample() after drains it. Same semantics as F10 / F so the
    // walker_per_frame_tick handler fires exactly once per press.
    InputHook hook;
    SimulateKeyDown(hook, WalkercoreKey::DumpState);

    InputState out1;
    hook.Sample(out1);
    EXPECT_TRUE(IsKeyPressed(out1, WalkercoreKey::DumpState));
    EXPECT_TRUE(IsKeyHeld(out1, WalkercoreKey::DumpState));

    InputState out2;
    hook.Sample(out2);
    EXPECT_FALSE(IsKeyPressed(out2, WalkercoreKey::DumpState));
    EXPECT_TRUE(IsKeyHeld(out2, WalkercoreKey::DumpState));
}

void TestInputHookDumpStateFallingEdgeVisible() {
    // F12 release emits a falling edge that survives one Sample() and
    // is drained after. Lets future M-side handlers distinguish a press
    // from a hold if they need to.
    InputHook hook;
    SimulateKeyDown(hook, WalkercoreKey::DumpState);
    SimulateKeyUp(hook, WalkercoreKey::DumpState);

    InputState out;
    hook.Sample(out);
    EXPECT_TRUE(IsKeyReleased(out, WalkercoreKey::DumpState));
    EXPECT_FALSE(IsKeyHeld(out, WalkercoreKey::DumpState));

    InputState out2;
    hook.Sample(out2);
    EXPECT_FALSE(IsKeyReleased(out2, WalkercoreKey::DumpState));
}

void TestInputHookClassifyToggleWalkAlone() {
    // Bare F10 → ToggleWalk, NOT ToggleMenu. The classifier only
    // routes to ToggleMenu when the crouch key (default Ctrl) is held.
    EXPECT_EQ(InputHook::ClassifyToggleWalkKey(false),
              WalkercoreKey::ToggleWalk);
}

void TestInputHookClassifyToggleWalkWithCrouchHeld() {
    // Crouch held (default Ctrl) + F10 → ToggleMenu. This is the
    // documented "Ctrl+F10 opens the menu" binding.
    EXPECT_EQ(InputHook::ClassifyToggleWalkKey(true),
              WalkercoreKey::ToggleMenu);
}

// -----------------------------------------------------------------------------
// WalkercoreKeyName probe — same pattern as
// TestFeatureNameReturnsNameForEveryValue,
// TestWalkerFsmStateNameReturnsNameForEveryValue,
// TestHeadTrackerSourceNameReturnsNameForEveryValue.
//
// WalkercoreKeyName() lives inline in walkercore/InputState.h. Adding
// a new WalkercoreKey without updating the inline switch makes the
// focused input dump emit "(unknown)" for that key. The probe table
// below pins:
//   - Every documented key has a non-null, non-empty, "(unknown)"-free
//     display name.
//   - Names are distinct (the diagnostic dump produces one row per
//     key with a bit set; ambiguity would silently merge two rows).
//   - The LastKey sentinel has a name too, so a maintainer who
//     reshuffles the enum keeps the LastKey case in the switch.
// -----------------------------------------------------------------------------

void TestWalkercoreKeyNameReturnsNameForEveryValue() {
    struct Case { WalkercoreKey k; const char *expected_name; };
    const Case kCases[] = {
        {WalkercoreKey::Forward,          "Forward"},
        {WalkercoreKey::Backward,         "Backward"},
        {WalkercoreKey::StrafeLeft,       "StrafeLeft"},
        {WalkercoreKey::StrafeRight,      "StrafeRight"},
        {WalkercoreKey::Run,              "Run"},
        {WalkercoreKey::Crouch,           "Crouch"},
        {WalkercoreKey::Jump,             "Jump"},
        {WalkercoreKey::EyeUp,            "EyeUp"},
        {WalkercoreKey::EyeDown,          "EyeDown"},
        {WalkercoreKey::EyeReset,         "EyeReset"},
        {WalkercoreKey::Interact,         "Interact"},
        {WalkercoreKey::ToggleWalk,       "ToggleWalk"},
        {WalkercoreKey::Flashlight,       "Flashlight"},
        {WalkercoreKey::Zoom,             "Zoom"},
        {WalkercoreKey::FlashlightToggle, "FlashlightToggle"},
        {WalkercoreKey::DumpState,        "DumpState"},
        {WalkercoreKey::ToggleMenu,       "ToggleMenu"},
        {WalkercoreKey::LastKey,          "LastKey"},
    };
    const size_t kCount = sizeof(kCases) / sizeof(kCases[0]);
    for (size_t i = 0; i < kCount; ++i) {
        const char *name = WalkercoreKeyName(kCases[i].k);
        EXPECT_TRUE(name != nullptr);
        EXPECT_TRUE(name[0] != '\0');
        EXPECT_TRUE(strcmp(name, "(unknown)") != 0);
        EXPECT_STREQ(name, kCases[i].expected_name);
    }
}

// Pin the contiguous-range contract that the InputHook keys[] array
// relies on (size is 32 to support a generous extension margin but
// every key from 0..LastKey must be addressable).

void TestWalkercoreKeyRangeIsContiguous() {
    EXPECT_EQ(static_cast<unsigned>(WalkercoreKey::Forward), 0u);
    EXPECT_EQ(static_cast<unsigned>(WalkercoreKey::LastKey),
              static_cast<unsigned>(WalkercoreKey::ToggleMenu) + 1u);
}

// -----------------------------------------------------------------------------
// M4 MousePump tests.
//
// OnRawInputData(const RAWINPUT&) is the test-friendly entry point — it
// takes a fully-decoded RAWINPUT struct so the unit suite can drive the
// motion / wheel / button-edges accumulation without spinning up a
// WM_INPUT pump. The tests below pin the four contracts the FrameLoop
// depends on:
//
//   1. Motion deltas accumulate across multiple OnRawInputData calls
//      and are drained by the next Sample() (so the per-frame motion
//      is exactly the sum of WM_INPUT events between two samples).
//   2. Wheel deltas accumulate the same way and survive the same drain.
//   3. LMB press / release flips the WalkercoreKey::Zoom held bit and
//      produces a one-frame rising / falling edge.
//   4. RMB press / release flips WalkercoreKey::FlashlightToggle.
//   5. Absolute-coordinate reports (touchscreen) are dropped so a
//      tablet contact doesn't yank the camera across the world.
//   6. Non-mouse raw input (keyboard) is ignored — keyboard events
//      still flow through the WH_KEYBOARD_LL callback.
// -----------------------------------------------------------------------------

namespace {

// Build a relative-motion RAWINPUT with the given dx/dy and no buttons.
RAWINPUT MakeMouseMotion(int dx, int dy) {
    RAWINPUT raw{};
    raw.header.dwType = RIM_TYPEMOUSE;
    raw.header.dwSize = sizeof(RAWINPUT);
    raw.data.mouse.usFlags = 0;  // relative
    raw.data.mouse.lLastX = static_cast<LONG>(dx);
    raw.data.mouse.lLastY = static_cast<LONG>(dy);
    return raw;
}

// Build a relative-motion RAWINPUT with absolute-coordinate flag set
// (the OS would deliver this for touchscreens / tablets).
RAWINPUT MakeMouseMotionAbsolute(int dx, int dy) {
    RAWINPUT raw = MakeMouseMotion(dx, dy);
    raw.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
    return raw;
}

// Build a wheel-only RAWINPUT.
RAWINPUT MakeMouseWheel(int wheel_delta) {
    RAWINPUT raw{};
    raw.header.dwType = RIM_TYPEMOUSE;
    raw.header.dwSize = sizeof(RAWINPUT);
    raw.data.mouse.usButtonFlags = RI_MOUSE_WHEEL;
    raw.data.mouse.usButtonData = static_cast<USHORT>(wheel_delta);
    return raw;
}

// Build a left-button RAWINPUT.
RAWINPUT MakeMouseLeftButton(bool down) {
    RAWINPUT raw{};
    raw.header.dwType = RIM_TYPEMOUSE;
    raw.header.dwSize = sizeof(RAWINPUT);
    raw.data.mouse.usButtonFlags = down ? RI_MOUSE_LEFT_BUTTON_DOWN
                                        : RI_MOUSE_LEFT_BUTTON_UP;
    return raw;
}

// Build a right-button RAWINPUT.
RAWINPUT MakeMouseRightButton(bool down) {
    RAWINPUT raw{};
    raw.header.dwType = RIM_TYPEMOUSE;
    raw.header.dwSize = sizeof(RAWINPUT);
    raw.data.mouse.usButtonFlags = down ? RI_MOUSE_RIGHT_BUTTON_DOWN
                                        : RI_MOUSE_RIGHT_BUTTON_UP;
    return raw;
}

}  // namespace

void TestInputHookOnRawInputMotionAccumulates() {
    // Two raw motion events in a frame sum into the next Sample's
    // mouse_dx / mouse_dy. Without accumulation the second motion
    // would overwrite the first and the player would only see half
    // the camera sweep.
    InputHook hook;
    hook.OnRawInputData(MakeMouseMotion(10, 5));
    hook.OnRawInputData(MakeMouseMotion(-3, 2));

    InputState out;
    hook.Sample(out);
    EXPECT_EQ(7, out.mouse_dx);
    EXPECT_EQ(7, out.mouse_dy);
}

void TestInputHookOnRawInputMotionDrainsOnNextSample() {
    // After the first Sample, the source motion accumulator MUST be
    // zero so the next frame starts fresh. Otherwise the player's
    // yaw / pitch would integrate the same delta twice.
    InputHook hook;
    hook.OnRawInputData(MakeMouseMotion(50, 50));

    InputState out1;
    hook.Sample(out1);
    EXPECT_EQ(50, out1.mouse_dx);
    EXPECT_EQ(50, out1.mouse_dy);

    InputState out2;
    hook.Sample(out2);
    EXPECT_EQ(0, out2.mouse_dx);
    EXPECT_EQ(0, out2.mouse_dy);
}

void TestInputHookOnRawInputAbsoluteMotionIsDropped() {
    // A touchscreen delivers MOUSE_MOVE_ABSOLUTE; integrating the
    // absolute coordinates as relative motion would yank the camera
    // across the world on first contact. The pump MUST drop the
    // report and leave the accumulators untouched.
    InputHook hook;
    hook.OnRawInputData(MakeMouseMotionAbsolute(1920, 1080));

    InputState out;
    hook.Sample(out);
    EXPECT_EQ(0, out.mouse_dx);
    EXPECT_EQ(0, out.mouse_dy);
}

void TestInputHookOnRawInputWheelAccumulates() {
    // Wheel events sum across the frame; the FrameLoop reads the
    // single accumulated delta on the consumer's sample.
    InputHook hook;
    hook.OnRawInputData(MakeMouseWheel(120));
    hook.OnRawInputData(MakeMouseWheel(-240));

    InputState out;
    hook.Sample(out);
    EXPECT_EQ(-120, out.wheel_delta);
}

void TestInputHookOnRawInputWheelDrainsOnNextSample() {
    // Wheel drain contract mirrors motion: once consumed, the
    // accumulator is zero for the next frame.
    InputHook hook;
    hook.OnRawInputData(MakeMouseWheel(360));

    InputState out1;
    hook.Sample(out1);
    EXPECT_EQ(360, out1.wheel_delta);

    InputState out2;
    hook.Sample(out2);
    EXPECT_EQ(0, out2.wheel_delta);
}

void TestInputHookOnRawInputLeftButtonPress() {
    // LMB press flips the WalkercoreKey::Zoom held bit and sets a
    // one-frame rising edge so the consumer's IsKeyPressed is true on
    // the next Sample.
    InputHook hook;
    hook.OnRawInputData(MakeMouseLeftButton(true));

    InputState out;
    hook.Sample(out);
    EXPECT_TRUE(IsKeyHeld(out, WalkercoreKey::Zoom));
    EXPECT_TRUE(IsKeyPressed(out, WalkercoreKey::Zoom));
}

void TestInputHookOnRawInputLeftButtonRelease() {
    // LMB release clears the held bit and sets a one-frame falling
    // edge. Same contract as the WH_KEYBOARD_LL keyboard path.
    InputHook hook;
    hook.OnRawInputData(MakeMouseLeftButton(true));
    hook.OnRawInputData(MakeMouseLeftButton(false));

    InputState out;
    hook.Sample(out);
    EXPECT_FALSE(IsKeyHeld(out, WalkercoreKey::Zoom));
    EXPECT_TRUE(IsKeyReleased(out, WalkercoreKey::Zoom));
}

void TestInputHookOnRawInputRightButtonPress() {
    // RMB press flips WalkercoreKey::FlashlightToggle, the second
    // mouse button the walker actually consumes.
    InputHook hook;
    hook.OnRawInputData(MakeMouseRightButton(true));

    InputState out;
    hook.Sample(out);
    EXPECT_TRUE(IsKeyHeld(out, WalkercoreKey::FlashlightToggle));
    EXPECT_TRUE(IsKeyPressed(out, WalkercoreKey::FlashlightToggle));
}

void TestInputHookOnRawInputButtonHeldNoExtraEdges() {
    // Holding LMB across multiple OnRawInputData calls (the OS keeps
    // firing LEFT_BUTTON_DOWN as the user holds — Windows repeats the
    // press flag under some configurations) MUST NOT add a new rising
    // edge. The held-bit guard inside the update_button lambda
    // mirrors the WH_KEYBOARD_LL key-repeat defence.
    InputHook hook;
    hook.OnRawInputData(MakeMouseLeftButton(true));
    hook.OnRawInputData(MakeMouseLeftButton(true));
    hook.OnRawInputData(MakeMouseLeftButton(true));

    InputState out1;
    hook.Sample(out1);
    EXPECT_TRUE(IsKeyPressed(out1, WalkercoreKey::Zoom));

    InputState out2;
    hook.Sample(out2);
    EXPECT_FALSE(IsKeyPressed(out2, WalkercoreKey::Zoom));
    EXPECT_TRUE(IsKeyHeld(out2, WalkercoreKey::Zoom));
}

void TestInputHookOnRawInputIgnoresNonMouse() {
    // Keyboard raw input is delivered through WH_KEYBOARD_LL, not
    // through WM_INPUT. The pump MUST ignore RIM_TYPEKEYBOARD so
    // there's no double-source for the keys[] array. Constructing
    // a minimal keyboard RAWINPUT with a real key transition would
    // require the FULL keyboard union layout, so we use a sentinel
    // type and check that nothing in last_ changed.
    InputHook hook;
    RAWINPUT kbd{};
    kbd.header.dwType = RIM_TYPEKEYBOARD;
    kbd.header.dwSize = sizeof(RAWINPUT);
    // No MakeCode / Flags mutation — the function MUST short-circuit
    // before touching anything.
    hook.OnRawInputData(kbd);

    InputState out;
    hook.Sample(out);
    EXPECT_EQ(0, out.mouse_dx);
    EXPECT_EQ(0, out.mouse_dy);
    EXPECT_EQ(0, out.wheel_delta);
    EXPECT_FALSE(IsKeyHeld(out, WalkercoreKey::Zoom));
    EXPECT_FALSE(IsKeyHeld(out, WalkercoreKey::FlashlightToggle));
}

void TestInputHookOnRawInputCombinedMotionAndButton() {
    // The OS can deliver a single WM_INPUT that carries both motion
    // AND button transitions (high-DPI mice often batch events to
    // save on the user -> kernel transition). The pump MUST apply
    // every field of the report, not bail on the first one set.
    RAWINPUT raw{};
    raw.header.dwType = RIM_TYPEMOUSE;
    raw.header.dwSize = sizeof(RAWINPUT);
    raw.data.mouse.lLastX = 7;
    raw.data.mouse.lLastY = -3;
    raw.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN;
    raw.data.mouse.usButtonData = 0;

    InputHook hook;
    hook.OnRawInputData(raw);

    InputState out;
    hook.Sample(out);
    EXPECT_EQ(7, out.mouse_dx);
    EXPECT_EQ(-3, out.mouse_dy);
    EXPECT_TRUE(IsKeyHeld(out, WalkercoreKey::Zoom));
}

}  // namespace walkercore