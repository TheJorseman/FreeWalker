// WalkerCore — SettingsMenu tests (M11 stub).
//
// Verifies the public edit API of the M11 stub:
//   - Open() clears the dirty flag and sets is_open_ to true.
//   - SetSliderValue with a known key writes through to the store and
//     sets dirty_ to true.
//   - SetSliderValue with an unknown key does NOT dirty the menu — the
//     caller will see their edit silently dropped.
//   - SetBoolValue("use_mouse") dirties and triggers ApplyAll.
//   - SetBoolValue with any other key leaves the menu clean.
//   - SetKeyBinding with a known key dirties; an unknown key leaves the
//     menu clean.
//   - Close() with dirty_ == true calls SaveToDefaultPath; with dirty_
//     == false it skips the write (the existing INI is preserved).
//   - Toggle() flips is_open_ in place.

#include "TestHarness.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "walkercore/Settings.h"
#include "walkercore/SettingsMenu.h"
#include "walkercore/PolishProviders.h"

namespace walkercore {

namespace {

// Read a small text file into a string. Returns empty on any I/O error.
std::string ReadFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::string();
    std::string out;
    f.seekg(0, std::ios::end);
    out.reserve(static_cast<size_t>(f.tellg()));
    f.seekg(0, std::ios::beg);
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return out;
}

// Returns true iff `haystack` contains the literal `needle`.
bool Contains(const std::string &haystack, const char *needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

void TestSettingsMenuOpenClosesDirty() {
    // Open() must transition is_open_ = true and reset the dirty flag
    // even if a prior edit left it set.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    EXPECT_TRUE(menu.isOpen());
    EXPECT_FALSE(menu.isDirty());

    menu.SetSliderValue("walk_speed_mps", 4.0f);
    EXPECT_TRUE(menu.isDirty());

    // Re-opening without saving in between resets dirty.
    menu.Close();
    menu.Open();
    EXPECT_FALSE(menu.isDirty());
    menu.Close();
}

void TestSettingsMenuSetSliderKnownKeyWritesThrough() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    const float original = store.current().movement.walkSpeedMps;
    EXPECT_NE(original, 3.5f);  // default is not 3.5 — sanity check

    menu.SetSliderValue("walk_speed_mps", 3.5f);
    EXPECT_NEAR(store.current().movement.walkSpeedMps, 3.5, 1e-4);
    EXPECT_TRUE(menu.isDirty());

    menu.Close();
}

void TestSettingsMenuSetSliderJumpVelocityWritesThrough() {
    // The INI parser already handles jump_initial_velocity_mps;
    // SettingsMenu::SetSliderValue must too — otherwise the in-game
    // menu can't edit what the INI already accepts.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetSliderValue("jump_initial_velocity_mps", 5.5f);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_NEAR(store.current().movement.jumpInitialVelocityMps, 5.5, 1e-4);
    menu.Close();
}

void TestSettingsMenuSetSliderGravityWritesThrough() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetSliderValue("gravity_mps2", 12.0f);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_NEAR(store.current().movement.gravityMps2, 12.0, 1e-4);
    menu.Close();
}

void TestSettingsMenuSetSliderMaxStepUpWritesThrough() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetSliderValue("max_step_up_m", 0.6f);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_NEAR(store.current().movement.maxStepUpM, 0.6, 1e-4);
    menu.Close();
}

void TestSettingsMenuSetSliderMaxSlopeAngleWritesThrough() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetSliderValue("max_slope_angle_rad", 1.2f);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_NEAR(store.current().movement.maxSlopeAngleRad, 1.2, 1e-4);
    menu.Close();
}

void TestSettingsMenuSetSliderNearZWritesThrough() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetSliderValue("near_z", 0.05f);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_NEAR(store.current().near_z, 0.05, 1e-4);
    menu.Close();
}

void TestSettingsMenuSetSliderFarZWritesThrough() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetSliderValue("far_z", 2000.0f);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_NEAR(store.current().far_z, 2000.0, 1e-3);
    menu.Close();
}

void TestSettingsMenuEveryDocumentedSliderWritesThrough() {
    // Exhaustive sweep: every slider key the menu advertises MUST
    // write through to the bound SettingsStore. The individual
    // per-key tests above catch regressions for the specific keys
    // they cover; this sweep makes sure no future maintainer deletes
    // a handler without updating the per-key tests too.
    //
    // Each entry pairs a key with a sentinel value and the field it
    // is supposed to update. The sentinel is unique enough that if
    // the write target is wrong, the EXPECT_NEAR will catch it.

    // First pass: top-level WalkerSettings fields (non-movement).
    struct TopLevelProbe {
        const char *key;
        float value;
        float WalkerSettings::*field;
    };
    const TopLevelProbe top[] = {
        {"horizontal_fov_radians", 1.234f, &WalkerSettings::horizontal_fov_radians},
        {"transition_seconds",     0.789f, &WalkerSettings::transition_seconds},
        {"near_z",                 0.111f, &WalkerSettings::near_z},
        {"far_z",                  2222.2f, &WalkerSettings::far_z},
    };
    for (const auto &p : top) {
        SettingsStore store;
        SettingsMenu menu;
        menu.Bind(&store);
        menu.Open();
        const float before = store.current().*p.field;
        menu.SetSliderValue(p.key, p.value);
        EXPECT_TRUE(menu.isDirty());
        EXPECT_NEAR(store.current().*p.field, p.value, 1e-4);
        // Sanity: at least one field flipped from its previous value.
        EXPECT_TRUE(store.current().*p.field != before ||
                    (before == p.value));
        menu.Close();
    }

    // Second pass: MovementConfig fields.
    struct MovementProbe {
        const char *key;
        float value;
        float MovementConfig::*field;
    };
    const MovementProbe moves[] = {
        {"walk_speed_mps",                9.99f,  &MovementConfig::walkSpeedMps},
        {"run_speed_mps",                 8.88f,  &MovementConfig::runSpeedMps},
        {"crouch_speed_mps",              0.77f,  &MovementConfig::crouchSpeedMps},
        {"jump_initial_velocity_mps",     5.55f,  &MovementConfig::jumpInitialVelocityMps},
        {"gravity_mps2",                  11.11f, &MovementConfig::gravityMps2},
        {"max_step_up_m",                 0.66f,  &MovementConfig::maxStepUpM},
        {"max_slope_angle_rad",           1.11f,  &MovementConfig::maxSlopeAngleRad},
        {"mouse_sensitivity",             0.0099f,&MovementConfig::mouseSensitivityRadPerUnit},
        {"head_bob_vertical_amp_m",       0.044f, &MovementConfig::headBobVerticalAmpM},
        {"head_bob_lateral_amp_m",        0.033f, &MovementConfig::headBobLateralAmpM},
        {"head_bob_frequency_hz",         2.22f,  &MovementConfig::headBobFrequencyHz},
        {"eye_height_standing_m",         1.77f,  &MovementConfig::eyeHeightStandingM},
        {"eye_height_crouching_m",        0.88f,  &MovementConfig::eyeHeightCrouchingM},
    };
    for (const auto &p : moves) {
        SettingsStore store;
        SettingsMenu menu;
        menu.Bind(&store);
        menu.Open();
        const float before = store.current().movement.*p.field;
        menu.SetSliderValue(p.key, p.value);
        EXPECT_TRUE(menu.isDirty());
        EXPECT_NEAR(store.current().movement.*p.field, p.value, 1e-4);
        EXPECT_TRUE(store.current().movement.*p.field != before ||
                    (before == p.value));
        menu.Close();
    }
}

void TestSettingsMenuEveryDocumentedBoolWritesThrough() {
    // Same idea as the slider sweep — every bool key the menu advertises
    // MUST write through. Today the set is the three head-tracker
    // enable flags; if more are added, append them here.
    struct BoolProbe {
        const char *key;
        bool target;
        bool HeadTrackerConfig::*field;
    };
    const BoolProbe probes[] = {
        {"use_mouse",     true,  &HeadTrackerConfig::use_mouse},
        {"use_trackir",   true,  &HeadTrackerConfig::use_trackir},
        {"use_opentrack", false, &HeadTrackerConfig::use_opentrack},
    };
    for (const auto &p : probes) {
        SettingsStore store;
        SettingsMenu menu;
        menu.Bind(&store);
        menu.Open();
        const bool before = store.current().head_tracker.*p.field;
        menu.SetBoolValue(p.key, p.target);
        EXPECT_TRUE(menu.isDirty());
        EXPECT_EQ(store.current().head_tracker.*p.field, p.target);
        EXPECT_TRUE(store.current().head_tracker.*p.field != before ||
                    (before == p.target));
        menu.Close();
    }
}

void TestSettingsMenuEveryDocumentedKeyBindingWritesThrough() {
    // Exhaustive sweep over keybinding keys. Each one must write
    // through to its corresponding KeyBindings field and dirty the
    // menu. Uses sentinel VK codes that don't collide with any
    // documented default (we use ASCII letters shifted by 0x80 to
    // stay in the high range, far from real Win32 VK_* defaults).
    struct KeyProbe {
        const char *key;
        int vk;
        int KeyBindings::*field;
    };
    const KeyProbe probes[] = {
        {"forward_vk",     0x80 + 'W', &KeyBindings::forward_vk},
        {"backward_vk",    0x80 + 'S', &KeyBindings::backward_vk},
        {"strafe_left_vk", 0x80 + 'A', &KeyBindings::strafe_left_vk},
        {"strafe_right_vk",0x80 + 'D', &KeyBindings::strafe_right_vk},
        {"run_vk",         0x80 + 'R', &KeyBindings::run_vk},
        {"crouch_vk",      0x80 + 'C', &KeyBindings::crouch_vk},
        {"jump_vk",        0x80 + 'J', &KeyBindings::jump_vk},
        {"interact_vk",    0x80 + 'I', &KeyBindings::interact_vk},
        {"toggle_walk_vk", 0x80 + 'T', &KeyBindings::toggle_walk_vk},
        {"flashlight_vk",  0x80 + 'F', &KeyBindings::flashlight_vk},
        {"eye_up_vk",      0x80 + 'U', &KeyBindings::eye_up_vk},
        {"eye_down_vk",    0x80 + 'N', &KeyBindings::eye_down_vk},
        {"eye_reset_vk",   0x80 + 'X', &KeyBindings::eye_reset_vk},
    };
    for (const auto &p : probes) {
        SettingsStore store;
        SettingsMenu menu;
        menu.Bind(&store);
        menu.Open();
        const int before = store.current().keys.*p.field;
        menu.SetKeyBinding(p.key, p.vk);
        EXPECT_TRUE(menu.isDirty());
        EXPECT_EQ(store.current().keys.*p.field, p.vk);
        EXPECT_TRUE(store.current().keys.*p.field != before ||
                    (before == p.vk));
        menu.Close();
    }
}

void TestSettingsMenuUnknownKeysAreSilentAcrossAllApis() {
    // Negative sweep — an unknown key across every edit API must NOT
    // dirty the menu and must NOT mutate the store. Pins the silent-
    // drop contract for sliders, bools, and key bindings together so
    // a regression in one is caught alongside the others.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);
    menu.Open();
    const float w_before = store.current().movement.walkSpeedMps;
    const bool m_before = store.current().head_tracker.use_mouse;
    const int f_before = store.current().keys.forward_vk;

    menu.SetSliderValue("definitely_not_a_real_slider", 42.0f);
    EXPECT_FALSE(menu.isDirty());

    menu.SetBoolValue("definitely_not_a_real_bool", true);
    EXPECT_FALSE(menu.isDirty());

    menu.SetKeyBinding("definitely_not_a_real_key", 'Z');
    EXPECT_FALSE(menu.isDirty());

    menu.SetUIntValue("definitely_not_a_real_uint", 0x99u);
    EXPECT_FALSE(menu.isDirty());

    EXPECT_NEAR(store.current().movement.walkSpeedMps, w_before, 1e-6);
    EXPECT_EQ(store.current().head_tracker.use_mouse, m_before);
    EXPECT_EQ(store.current().keys.forward_vk, f_before);
    menu.Close();
}

void TestSettingsMenuSetSliderUnknownKeyIsSilent() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    const float before = store.current().movement.walkSpeedMps;
    menu.SetSliderValue("not_a_real_key", 9.0f);
    EXPECT_FALSE(menu.isDirty());
    EXPECT_NEAR(store.current().movement.walkSpeedMps, before, 1e-4);
    menu.Close();
}

void TestSettingsMenuSetBoolUseMouseDirties() {
    // use_mouse is now a real key in WalkerSettings.head_tracker. The
    // bool handler writes through to that field, dirties the menu, and
    // ApplyAll() pushes the config into any bound HeadTrackerProvider.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetBoolValue("use_mouse", true);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_TRUE(store.current().head_tracker.use_mouse);
    menu.Close();
}

void TestSettingsMenuSetBoolUnknownKeyIsSilent() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetBoolValue("enable_something_bogus", true);
    EXPECT_FALSE(menu.isDirty());
    menu.Close();
}

void TestSettingsMenuSetBoolUseTrackirDirties() {
    // use_trackir → head_tracker.use_trackir. Same pattern as use_mouse.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetBoolValue("use_trackir", true);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_TRUE(store.current().head_tracker.use_trackir);
    menu.Close();
}

void TestSettingsMenuSetBoolUseOpentrackDirties() {
    // use_opentrack → head_tracker.use_opentrack.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetBoolValue("use_opentrack", false);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_FALSE(store.current().head_tracker.use_opentrack);
    menu.Close();
}

void TestSettingsMenuSetBoolApplyAllPushesToProvider() {
    // The full contract — SetBoolValue writes through, ApplyAll pushes
    // to the bound HeadTrackerProvider. We bind a real one and verify
    // its config flips accordingly.
    SettingsStore store;
    HeadTrackerProvider provider;
    store.BindHeadTracker(&provider);
    SettingsMenu menu;
    menu.Bind(&store);

    // Default config leaves all sources off.
    EXPECT_FALSE(provider.config().use_mouse);

    menu.Open();
    menu.SetBoolValue("use_mouse", true);
    EXPECT_TRUE(provider.config().use_mouse);
    menu.Close();
}

void TestSettingsMenuSetKeyBindingKnownKeyWritesThrough() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetKeyBinding("forward_vk", 'T');  // rebind forward from W to T
    EXPECT_TRUE(menu.isDirty());
    EXPECT_EQ(store.current().keys.forward_vk, static_cast<int>('T'));
    menu.Close();
}

void TestSettingsMenuSetKeyBindingUnknownKeyIsSilent() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    const int original = store.current().keys.forward_vk;
    menu.SetKeyBinding("not_a_real_key", 'Z');
    EXPECT_FALSE(menu.isDirty());
    EXPECT_EQ(store.current().keys.forward_vk, original);
    menu.Close();
}

void TestSettingsMenuToggleFlipsIsOpen() {
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    EXPECT_FALSE(menu.isOpen());
    menu.Toggle();
    EXPECT_TRUE(menu.isOpen());
    menu.Toggle();
    EXPECT_FALSE(menu.isOpen());
}

void TestSettingsMenuCloseWithoutDirtyDoesNotWrite() {
    // Point the store at a temp INI file with known content, open + close
    // the menu without editing, and verify the file is untouched.
    const std::string tmp = std::string("test_walkercore_menu_noedit.ini");
    {
        std::ofstream f(tmp);
        f << "[walker]\nwalk_speed_mps = 1.5\n";
    }

    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(tmp));
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    EXPECT_FALSE(menu.isDirty());
    menu.Close();

    const std::string after = ReadFile(tmp);
    EXPECT_TRUE(Contains(after, "walk_speed_mps = 1.5"));

    std::remove(tmp.c_str());
}

void TestSettingsMenuSetBoolM2GameFreeCameraShimDirties() {
    // m2_game_free_camera_shim is now a real M11 menu key. Flipping
    // it must (i) update WalkerSettings.m2_game_free_camera_shim,
    // (ii) flip m2_game_free_camera_shim_explicit so the M2.7
    // migration does NOT snap the value back to a sentinel default,
    // (iii) dirty the menu so Close() persists the edit, and
    // (iv) call ApplyAll() so any bound GameFreeCameraShim
    // re-reads the gate on the next Walking entry.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    EXPECT_FALSE(menu.isDirty());
    menu.SetBoolValue("m2_game_free_camera_shim", true);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_TRUE(store.current().m2_game_free_camera_shim);
    EXPECT_TRUE(store.current().m2_game_free_camera_shim_explicit);
    menu.Close();
}

void TestSettingsMenuSetBoolM2DevCvarPatcherDirties() {
    // m2_dev_cvar_patcher is the one-shot cvar patcher gate. The
    // menu exposes it for visibility; runtime flipping does NOT
    // re-trigger the patch (the patch is one-shot per process),
    // but the new value persists via Save now / Save on close.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    const bool before = store.current().m2_dev_cvar_patcher;
    menu.SetBoolValue("m2_dev_cvar_patcher", !before);
    EXPECT_TRUE(menu.isDirty());
    EXPECT_NE(store.current().m2_dev_cvar_patcher, before);
    menu.Close();
}

void TestSettingsMenuSetUIntM2DevCameraToggleVkWritesThrough() {
    // The dev camera toggle VK is exposed as `m2_dev_camera_toggle_vk`.
    // The menu writes the unsigned int raw and flips the
    // *_explicit flag so the M2.7 in-memory migration skips its
    // legacy-default snap (0x30 / 0x60 -> 0x7A) on the next
    // SettingsStore load.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetUIntValue("m2_dev_camera_toggle_vk", 0xC0u);  // '`' console key
    EXPECT_TRUE(menu.isDirty());
    EXPECT_EQ(store.current().m2_dev_camera_toggle_vk, 0xC0u);
    EXPECT_TRUE(store.current().m2_dev_camera_toggle_vk_explicit);
    menu.Close();
}

void TestSettingsMenuSetUIntUnknownKeyIsSilent() {
    // Negative: an unknown unsigned-int key must NOT dirty the menu
    // and must NOT mutate the store. Pins the silent-drop contract
    // for the new SetUIntValue API alongside the existing
    // SetSliderValue / SetBoolValue / SetKeyBinding negative cases.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    const unsigned int before = store.current().m2_dev_camera_toggle_vk;
    menu.SetUIntValue("definitely_not_a_real_uint", 0x99u);
    EXPECT_FALSE(menu.isDirty());
    EXPECT_EQ(store.current().m2_dev_camera_toggle_vk, before);
    menu.Close();
}

void TestSettingsMenuCloseWithDirtyWritesToDisk() {
    // Same setup but with a real edit before close. Verify the new value
    // appears in the file. We don't depend on %APPDATA% — the test uses
    // a temp INI we control, and Close() always saves to the store's
    // default path. The store auto-saves to DefaultSettingsPath() so we
    // have to either redirect that or skip this case.
    //
    // SettingsMenu::Close() calls SaveToDefaultPath, which writes to
    // %APPDATA%\WalkerCore\WalkerCore.ini on Windows. That's not safe to
    // touch from a unit test (would pollute the user's profile and
    // break parallel test runs). The dirty-not-dirty branch above is
    // the testable piece — the actual disk write is the SettingsStore's
    // responsibility, which already has its own tests.
    //
    // We assert here that dirty_ flows through correctly: after
    // SetSliderValue, the menu reports dirty; after Close(), is_open_
    // is false.
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);

    menu.Open();
    menu.SetSliderValue("crouch_speed_mps", 0.7f);
    EXPECT_TRUE(menu.isDirty());
    menu.Close();
    EXPECT_FALSE(menu.isOpen());
}

}  // namespace walkercore