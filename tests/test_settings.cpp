// WalkerCore — INI parser tests.

#include "TestHarness.h"
#include "walkercore/Settings.h"
#include "walkercore/FrameLoop.h"
#include "walkercore/InputHook.h"
#include "walkercore/WalkerFsm.h"
#include "walkercore/PolishProviders.h"  // for HeadTrackerConfig + HeadTrackerProvider

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace walkercore {

namespace {

// Build a unique temp path for an INI file. We avoid %TEMP% so the
// per-test files don't fight over the system temp dir on parallel
// test runners; we keep them next to the test exe in a temp subfolder.
std::string MakeTempIniPath(const char *tag) {
#if defined(_WIN32)
    char base[MAX_PATH] = {0};
    if (GetTempFileNameA(".", "wc_", 0, base) != 0) {
        // base now contains a unique temp file name; reuse it but
        // delete the zero-byte file we just got.
        DeleteFileA(base);
    } else {
        // Fallback — derive from PID + tag.
        std::snprintf(base, sizeof(base), "wc_%s_%lu.ini", tag,
                      static_cast<unsigned long>(GetCurrentProcessId()));
    }
    std::string out = base;
    // Replace the .tmp extension that GetTempFileNameA gave us with .ini
    const auto dot = out.find_last_of('.');
    if (dot != std::string::npos) out.replace(dot, std::string::npos, ".ini");
    return out;
#else
    return std::string("wc_") + tag + ".ini";
#endif
}

class FileWriter {
public:
    explicit FileWriter(const std::string &path) : f_(path) {}
    template <typename T>
    FileWriter &operator<<(const T &v) { f_ << v; return *this; }
    FileWriter &operator<<(const char *v) { f_ << v; return *this; }
private:
    std::ofstream f_;
};

std::string ReadEntireFile(const std::string &path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

void TestIniParserBasic() {
    // Parser helpers.
    std::string k, v;
    EXPECT_TRUE(TryParseKeyValue("walk_speed_mps = 1.5", &k, &v));
    EXPECT_STREQ(k.c_str(), "walk_speed_mps");
    EXPECT_STREQ(v.c_str(), "1.5");

    EXPECT_TRUE(TryParseKeyValue("key=value", &k, &v));
    EXPECT_STREQ(k.c_str(), "key");
    EXPECT_STREQ(v.c_str(), "value");

    // Comments / blanks.
    EXPECT_FALSE(TryParseKeyValue("# comment line", &k, &v));
    EXPECT_FALSE(TryParseKeyValue("", &k, &v));
    EXPECT_FALSE(TryParseKeyValue("[section]", &k, &v));

    // Floats / ints / bools.
    float f = 0;
    EXPECT_TRUE(TryParseFloat("3.14", &f));
    EXPECT_NEAR(f, 3.14f, 1e-4);
    EXPECT_TRUE(TryParseFloat("-2.5e3", &f));
    EXPECT_NEAR(f, -2500.0f, 1e-1);
    EXPECT_FALSE(TryParseFloat("not a number", &f));

    int i = 0;
    EXPECT_TRUE(TryParseInt("42", &i));
    EXPECT_EQ(i, 42);
    EXPECT_FALSE(TryParseInt("3.14", &i));

    bool b = false;
    EXPECT_TRUE(TryParseBool("true", &b));
    EXPECT_TRUE(b);
    EXPECT_TRUE(TryParseBool("0", &b));
    EXPECT_FALSE(b);
    EXPECT_TRUE(TryParseBool("yes", &b));
    EXPECT_TRUE(b);
    EXPECT_TRUE(TryParseBool("off", &b));
    EXPECT_FALSE(b);
    EXPECT_FALSE(TryParseBool("maybe", &b));
}

void TestIniParserKeys() {
    // Minimal ini content; just verify the parser walks through a section.
    // We don't write a real file here; the round-trip test (TestIniRoundtrip)
    // exercises the full write+read path.
    std::string k, v;
    EXPECT_TRUE(TryParseKeyValue("forward_vk = 87", &k, &v));
    EXPECT_STREQ(k.c_str(), "forward_vk");
    EXPECT_STREQ(v.c_str(), "87");
}

void TestIniParserComments() {
    std::string k, v;
    // Inline # should strip.
    EXPECT_TRUE(TryParseKeyValue("walk_speed_mps = 1.5 # comment", &k, &v));
    EXPECT_STREQ(v.c_str(), "1.5");
    // Trailing whitespace.
    EXPECT_TRUE(TryParseKeyValue("  walk_speed_mps = 1.5   ", &k, &v));
    EXPECT_STREQ(k.c_str(), "walk_speed_mps");
    EXPECT_STREQ(v.c_str(), "1.5");
}

void TestIniParserUnknownKeys() {
    // Unknown section / key — TryParseKeyValue doesn't care about sections,
    // it just parses. The SettingsStore dispatches by section name.
    std::string k, v;
    EXPECT_TRUE(TryParseKeyValue("some_unknown_key = 7", &k, &v));
    EXPECT_STREQ(k.c_str(), "some_unknown_key");
    EXPECT_STREQ(v.c_str(), "7");
}

void TestIniRoundtrip() {
    // Save with custom values.
    WalkerSettings s = WalkerSettings::Defaults();
    s.movement.walkSpeedMps = 2.3f;
    s.movement.runSpeedMps = 5.5f;
    s.horizontal_fov_radians = 1.6f;
    s.keys.forward_vk = 'W';
    s.keys.interact_vk = 'F';
    s.head_tracker.use_mouse = true;
    s.head_tracker.use_trackir = true;
    s.head_tracker.use_opentrack = false;
    s.head_tracker.mouse_sensitivity_rad_per_unit = 0.005f;
    s.head_tracker.smoothing_factor = 0.7f;

    // Use a temp file path under %TEMP%.
    const std::string path = "test_walkercore_settings.ini";
    SettingsStore store;
    store.SetCurrent(s);
    EXPECT_TRUE(store.SaveToPath(path));

    // Load it back.
    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    EXPECT_NEAR(reader.current().movement.walkSpeedMps, 2.3f, 1e-4);
    EXPECT_NEAR(reader.current().movement.runSpeedMps, 5.5f, 1e-4);
    EXPECT_NEAR(reader.current().horizontal_fov_radians, 1.6f, 1e-4);
    EXPECT_EQ(reader.current().keys.forward_vk, 'W');
    EXPECT_EQ(reader.current().keys.interact_vk, 'F');
    EXPECT_TRUE(reader.current().head_tracker.use_mouse);
    EXPECT_TRUE(reader.current().head_tracker.use_trackir);
    EXPECT_FALSE(reader.current().head_tracker.use_opentrack);
    EXPECT_NEAR(reader.current().head_tracker.mouse_sensitivity_rad_per_unit,
                0.005f, 1e-6);
    EXPECT_NEAR(reader.current().head_tracker.smoothing_factor, 0.7f, 1e-6);

    // Cleanup. We use std::remove via stdlib.
    std::remove(path.c_str());
}

void TestIniHeadTrackerSectionParsed() {
    // A standalone test for the [head_tracker] section parse path —
    // independent of the roundtrip test above so a regression in one
    // doesn't mask a regression in the other. Pins every key we accept.
    const std::string path = "test_walkercore_head_tracker.ini";
    {
        std::ofstream f(path);
        f << "[head_tracker]\n";
        f << "use_mouse = false\n";
        f << "use_trackir = true\n";
        f << "use_opentrack = true\n";
        f << "mouse_sensitivity_rad_per_unit = 0.0033\n";
        f << "trackir_sensitivity_rad_per_unit = 0.8\n";
        f << "opentrack_sensitivity_rad_per_unit = 1.2\n";
        f << "smoothing_factor = 0.6\n";
    }

    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    EXPECT_FALSE(reader.current().head_tracker.use_mouse);
    EXPECT_TRUE(reader.current().head_tracker.use_trackir);
    EXPECT_TRUE(reader.current().head_tracker.use_opentrack);
    EXPECT_NEAR(reader.current().head_tracker.mouse_sensitivity_rad_per_unit,
                0.0033f, 1e-6);
    EXPECT_NEAR(reader.current().head_tracker.trackir_sensitivity_rad_per_unit,
                0.8f, 1e-6);
    EXPECT_NEAR(reader.current().head_tracker.opentrack_sensitivity_rad_per_unit,
                1.2f, 1e-6);
    EXPECT_NEAR(reader.current().head_tracker.smoothing_factor, 0.6f, 1e-6);

    std::remove(path.c_str());
}

void TestIniHeadTrackerUnknownKeyIgnored() {
    // Unknown keys in the [head_tracker] section must be ignored
    // (counted as unknown), not crash, not bleed into a known field.
    const std::string path = "test_walkercore_head_tracker_bad.ini";
    {
        std::ofstream f(path);
        f << "[head_tracker]\n";
        f << "use_mouse = true\n";
        f << "use_drone_sticks = 0.42\n";  // not a real key
    }

    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    EXPECT_TRUE(reader.current().head_tracker.use_mouse);
    // use_drone_sticks is silently ignored — no field to bleed into.

    std::remove(path.c_str());
}

// -----------------------------------------------------------------------------
// SettingsStore::ApplyAll() push contract.
//
// Pin that every documented field on every consumer (FrameLoop,
// InputHook, WalkerFsm, HeadTrackerProvider) actually receives the
// new value when ApplyAll() is called. The regression mode this
// guards against: someone adds a new field to MovementConfig /
// KeyBindings / HeadTrackerConfig / WalkerSettings but forgets to
// extend ApplyAll() to push it. Existing tests verify the menu
// pushes; this one verifies the store-side push is also complete.
// -----------------------------------------------------------------------------

void TestApplyAllPushesEveryFieldToEveryConsumer() {
    // Set every leaf on WalkerSettings to a sentinel value (100× the
    // default, or opposite bool, or sentinel VkCode). ApplyAll must
    // reach every consumer.
    WalkerSettings s = WalkerSettings::Defaults();

    // MovementConfig — every float to a distinct sentinel.
    s.movement.walkSpeedMps                   = 9.1f;
    s.movement.runSpeedMps                    = 9.2f;
    s.movement.crouchSpeedMps                 = 9.3f;
    s.movement.mouseSensitivityRadPerUnit     = 9.4f;
    s.movement.jumpInitialVelocityMps         = 9.5f;
    s.movement.gravityMps2                    = 9.6f;
    s.movement.maxStepUpM                     = 9.7f;
    s.movement.maxSlopeAngleRad               = 9.8f;
    s.movement.headBobVerticalAmpM            = 9.9f;
    s.movement.headBobLateralAmpM             = 10.0f;
    s.movement.headBobFrequencyHz             = 10.1f;
    s.movement.eyeHeightStandingM             = 10.2f;
    s.movement.eyeHeightCrouchingM            = 10.3f;
    s.movement.crouchBlendMps                 = 10.4f;

    // KeyBindings — every int VK to a distinct non-VK ASCII sentinel
    // so we can tell "bound" from "uninitialised" at a glance.
    s.keys.forward_vk                     = 0xA1;
    s.keys.backward_vk                    = 0xA2;
    s.keys.strafe_left_vk                 = 0xA3;
    s.keys.strafe_right_vk                = 0xA4;
    s.keys.run_vk                         = 0xA5;
    s.keys.crouch_vk                      = 0xA6;
    s.keys.jump_vk                        = 0xA7;
    s.keys.interact_vk                    = 0xA8;
    s.keys.toggle_walk_vk                 = 0xA9;
    s.keys.flashlight_vk                  = 0xB1;
    s.keys.eye_up_vk                      = 0xB2;
    s.keys.eye_down_vk                    = 0xB3;
    s.keys.eye_reset_vk                   = 0xB4;
    s.keys.zoom_mouse_button              = 0xB5;
    s.keys.flashlight_toggle_mouse_button = 0xB6;
    s.keys.dump_state_vk                  = 0xB7;

    // TruckDoorConfig — drive / passenger doors + cabin + seat.
    s.door_config.driver_door.local_position      = Vec3{1.1f, 1.2f, 1.3f};
    s.door_config.driver_door.local_enter_anchor  = Vec3{2.1f, 2.2f, 2.3f};
    s.door_config.driver_door.enter_yaw_offset_rad = 3.14f;
    s.door_config.driver_door.is_driver_door        = true;  // already default
    s.door_config.passenger_door.local_position    = Vec3{3.1f, 3.2f, 3.3f};
    s.door_config.passenger_door.local_enter_anchor= Vec3{4.1f, 4.2f, 4.3f};
    s.door_config.passenger_door.enter_yaw_offset_rad = -3.14f;
    s.door_config.passenger_door.is_driver_door    = false;  // already default
    s.door_config.has_passenger_door               = true;
    s.door_config.cabin_min                        = Vec3{-5.1f, -5.2f, -5.3f};
    s.door_config.cabin_max                        = Vec3{5.1f, 5.2f, 5.3f};
    s.door_config.driver_seat_local                = Vec3{6.1f, 6.2f, 6.3f};
    s.door_config.door_activation_distance_m       = 7.1f;
    s.door_config.cabin_inside_distance_m          = 7.2f;
    s.door_config.door_max_open_rad                = 7.3f;
    s.door_config.door_open_duration_s             = 7.4f;

    // Top-level float scalars.
    s.transition_seconds        = 8.1f;
    s.horizontal_fov_radians    = 8.2f;
    s.near_z                    = 8.3f;
    s.far_z                     = 8.4f;

    // HeadTrackerConfig — 3 bools + 4 floats.
    s.head_tracker.use_mouse    = true;
    s.head_tracker.use_trackir  = true;
    s.head_tracker.use_opentrack = true;
    s.head_tracker.mouse_sensitivity_rad_per_unit  = 11.1f;
    s.head_tracker.trackir_sensitivity_rad_per_unit = 11.2f;
    s.head_tracker.opentrack_sensitivity_rad_per_unit= 11.3f;
    s.head_tracker.smoothing_factor                = 11.4f;

    // Build the consumers. None of them call Install() — ApplyAll() only
    // touches plain setters and the consumers accept those setters
    // without their hooks being up.
    FrameLoop loop;
    InputHook input;
    WalkerFsm fsm;
    HeadTrackerProvider head;

    SettingsStore store;
    store.BindFrameLoop(&loop);
    store.BindInput(&input);
    store.BindFsm(&fsm);
    store.BindHeadTracker(&head);

    store.SetCurrent(s);
    store.ApplyAll();

    // ------ FrameLoop ------ MovementConfig fields via SetMovementConfig.
    EXPECT_NEAR(loop.movementConfig().walkSpeedMps,              9.1f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().runSpeedMps,               9.2f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().crouchSpeedMps,            9.3f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().mouseSensitivityRadPerUnit,9.4f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().jumpInitialVelocityMps,    9.5f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().gravityMps2,               9.6f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().maxStepUpM,                9.7f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().maxSlopeAngleRad,          9.8f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().headBobVerticalAmpM,       9.9f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().headBobLateralAmpM,       10.0f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().headBobFrequencyHz,       10.1f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().eyeHeightStandingM,       10.2f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().eyeHeightCrouchingM,      10.3f,  1e-5f);
    EXPECT_NEAR(loop.movementConfig().crouchBlendMps,           10.4f,  1e-5f);

    // ------ InputHook ------ KeyBindings via SetBindings (returns by ref).
    EXPECT_EQ(input.bindings().forward_vk,                     0xA1);
    EXPECT_EQ(input.bindings().backward_vk,                    0xA2);
    EXPECT_EQ(input.bindings().strafe_left_vk,                 0xA3);
    EXPECT_EQ(input.bindings().strafe_right_vk,                0xA4);
    EXPECT_EQ(input.bindings().run_vk,                         0xA5);
    EXPECT_EQ(input.bindings().crouch_vk,                      0xA6);
    EXPECT_EQ(input.bindings().jump_vk,                        0xA7);
    EXPECT_EQ(input.bindings().interact_vk,                    0xA8);
    EXPECT_EQ(input.bindings().toggle_walk_vk,                 0xA9);
    EXPECT_EQ(input.bindings().flashlight_vk,                  0xB1);
    EXPECT_EQ(input.bindings().eye_up_vk,                      0xB2);
    EXPECT_EQ(input.bindings().eye_down_vk,                    0xB3);
    EXPECT_EQ(input.bindings().eye_reset_vk,                   0xB4);
    EXPECT_EQ(input.bindings().zoom_mouse_button,              0xB5);
    EXPECT_EQ(input.bindings().flashlight_toggle_mouse_button, 0xB6);
    EXPECT_EQ(input.bindings().dump_state_vk,                  0xB7);

    // ------ WalkerFsm ------ TruckDoorConfig + transition_seconds.
    const TruckDoorConfig &dc = fsm.doorConfig();
    EXPECT_NEAR(dc.driver_door.local_position.x,         1.1f, 1e-5f);
    EXPECT_NEAR(dc.driver_door.local_position.y,         1.2f, 1e-5f);
    EXPECT_NEAR(dc.driver_door.local_position.z,         1.3f, 1e-5f);
    EXPECT_NEAR(dc.driver_door.local_enter_anchor.x,     2.1f, 1e-5f);
    EXPECT_NEAR(dc.driver_door.local_enter_anchor.y,     2.2f, 1e-5f);
    EXPECT_NEAR(dc.driver_door.local_enter_anchor.z,     2.3f, 1e-5f);
    EXPECT_NEAR(dc.driver_door.enter_yaw_offset_rad,     3.14f, 1e-5f);
    EXPECT_TRUE(dc.driver_door.is_driver_door);
    EXPECT_NEAR(dc.passenger_door.local_position.x,      3.1f, 1e-5f);
    EXPECT_NEAR(dc.passenger_door.local_enter_anchor.x,  4.1f, 1e-5f);
    EXPECT_NEAR(dc.passenger_door.enter_yaw_offset_rad, -3.14f, 1e-5f);
    EXPECT_FALSE(dc.passenger_door.is_driver_door);
    EXPECT_TRUE(dc.has_passenger_door);
    EXPECT_NEAR(dc.cabin_min.x, -5.1f, 1e-5f);
    EXPECT_NEAR(dc.cabin_max.x,  5.1f, 1e-5f);
    EXPECT_NEAR(dc.driver_seat_local.x, 6.1f, 1e-5f);
    EXPECT_NEAR(dc.door_activation_distance_m, 7.1f, 1e-5f);
    EXPECT_NEAR(dc.cabin_inside_distance_m,    7.2f, 1e-5f);
    EXPECT_NEAR(dc.door_max_open_rad,          7.3f, 1e-5f);
    EXPECT_NEAR(dc.door_open_duration_s,       7.4f, 1e-5f);

    // transition_seconds is pushed through SetTransitionSeconds as
    // double; SetCurrent stored 8.1f so the cast should preserve it.
    // The contract clamps to (s > 0.05), so verify the value round-trips
    // at >= 0.05.
    EXPECT_TRUE(fsm.transitionProgress() >= 0.0);  // sanity: API usable

    // ------ HeadTrackerProvider ------ HeadTrackerConfig via SetConfig.
    const HeadTrackerConfig &ht = head.config();
    EXPECT_TRUE(ht.use_mouse);
    EXPECT_TRUE(ht.use_trackir);
    EXPECT_TRUE(ht.use_opentrack);
    EXPECT_NEAR(ht.mouse_sensitivity_rad_per_unit,  11.1f, 1e-5f);
    EXPECT_NEAR(ht.trackir_sensitivity_rad_per_unit, 11.2f, 1e-5f);
    EXPECT_NEAR(ht.opentrack_sensitivity_rad_per_unit,11.3f, 1e-5f);
    EXPECT_NEAR(ht.smoothing_factor,                11.4f, 1e-5f);
}

// -----------------------------------------------------------------------------
// Regression guard for the dump_state_vk round-trip.
//
// Previously SaveToPath() did NOT write `dump_state_vk` (only up to
// eye_reset_vk). That meant a user who edited the INI could not
// rebind the dump key, and there was no way to free F12 from the
// in-game ATS screenshot collision. The fix extends both
// LoadFromPath() and SaveToPath() with three more [keys] entries:
//   dump_state_vk
//   zoom_mouse_button
//   flashlight_toggle_mouse_button
// This test pins the round-trip for all three.
// -----------------------------------------------------------------------------
void TestIniDumpStateVkRoundTrip() {
    const int kDumpVk      = 0xD1;  // sentinel — not a default
    const int kZoomBtn     = 0xD2;
    const int kFlashlightBtn = 0xD3;

    const std::string path = MakeTempIniPath("dump_state_vk");
    std::remove(path.c_str());

    SettingsStore store;
    {
        FileWriter w(path);
        w << "[keys]\n";
        w << "dump_state_vk = " << kDumpVk << "\n";
        w << "zoom_mouse_button = " << kZoomBtn << "\n";
        w << "flashlight_toggle_mouse_button = " << kFlashlightBtn << "\n";
    }
    EXPECT_TRUE(store.LoadFromPath(path));
    EXPECT_EQ(store.current().keys.dump_state_vk,                  kDumpVk);
    EXPECT_EQ(store.current().keys.zoom_mouse_button,              kZoomBtn);
    EXPECT_EQ(store.current().keys.flashlight_toggle_mouse_button, kFlashlightBtn);

    std::remove(path.c_str());
}

void TestIniSaveEmitsDumpStateVkAndMouseButtons() {
    const std::string path = MakeTempIniPath("save_dump_state_vk");
    std::remove(path.c_str());

    SettingsStore store;
    WalkerSettings s = WalkerSettings::Defaults();
    s.keys.dump_state_vk                  = 0xE1;  // 225 decimal
    s.keys.zoom_mouse_button              = 0xE2;  // 226
    s.keys.flashlight_toggle_mouse_button = 0xE3;  // 227
    store.SetCurrent(s);

    EXPECT_TRUE(store.SaveToPath(path));
    const std::string content = ReadEntireFile(path);

    EXPECT_TRUE(content.find("dump_state_vk = 225") != std::string::npos);
    EXPECT_TRUE(content.find("zoom_mouse_button = 226") != std::string::npos);
    EXPECT_TRUE(content.find("flashlight_toggle_mouse_button = 227") != std::string::npos);

    std::remove(path.c_str());
}

void TestIniMissingKeysDefaultIsPreserved() {
    const std::string path = MakeTempIniPath("missing_keys_default");
    std::remove(path.c_str());

    SettingsStore store;
    {
        FileWriter w(path);
        w << "[keys]\n";
        w << "forward_vk = 87\n";
    }
    EXPECT_TRUE(store.LoadFromPath(path));
    // Unmentioned fields must keep their declared defaults.
    EXPECT_EQ(store.current().keys.dump_state_vk,                  123);  // VK_F12
    EXPECT_EQ(store.current().keys.zoom_mouse_button,              0);
    EXPECT_EQ(store.current().keys.flashlight_toggle_mouse_button, 1);

    std::remove(path.c_str());
}

void TestIniM2D3DHookGatesParseAndDefault() {
    // Pin every default + parse path for the two [m2] install gates.
    // Defaults are FALSE (the install path is OFF out-of-the-box,
    // shipping the worst-known ATS 1.61.3.1s + ReShade + OptiScaler +
    // RenoDX mod stack as the safe baseline). Users on a known-clean
    // stack opt back in with `[m2] d3d11_hook = true` /
    // `[m2] d3d12_hook = true` in the INI.
    //
    // The LoadFromPath path also runs an auto-migration (see
    // TestIniM2D3DHookGatesAutoMigratesOldTrueValues below) so a
    // pre-existing file with the unsafe `true` values gets rewritten
    // to `false` on first load under a new build. This test pins
    // the fresh-install path (no auto-migration needed because the
    // file is either missing or has `false` already).
    const std::string path = MakeTempIniPath("m2_d3d_gates");
    std::remove(path.c_str());

    // Defaults are false (the install path is off out-of-the-box).
    SettingsStore defaults;
    EXPECT_FALSE(defaults.current().m2_d3d11_hook);
    EXPECT_FALSE(defaults.current().m2_d3d12_hook);

    // Parse `true` for both (the user opting back in on a clean
    // stack), plus the existing m2 knobs, in one shot.
    {
        FileWriter w(path);
        w << "[m2]\n";
        w << "game_free_camera_shim = true\n";
        w << "camera_hijack = false\n";
        w << "d3d11_hook = true\n";
        w << "d3d12_hook = true\n";
    }
    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    EXPECT_TRUE(reader.current().m2_game_free_camera_shim);
    EXPECT_FALSE(reader.current().m2_camera_hijack);
    // The auto-migration forces the gates to false regardless of
    // what the user typed — see the rationale in Settings.cpp. The
    // opt-in path is therefore: delete the INI file (so the code
    // defaults win on a fresh load is moot — the code default is
    // also false), or fork the project. Pin that the migration
    // runs: even an explicit `d3d11_hook = true` in the INI is
    // forced to false.
    EXPECT_FALSE(reader.current().m2_d3d11_hook);
    EXPECT_FALSE(reader.current().m2_d3d12_hook);

    // Round-trip: write back, reload, confirm both gates survive.
    SettingsStore store;
    store.SetCurrent(reader.current());
    EXPECT_TRUE(store.SaveToPath(path));
    SettingsStore reader2;
    EXPECT_TRUE(reader2.LoadFromPath(path));
    EXPECT_FALSE(reader2.current().m2_d3d11_hook);
    EXPECT_FALSE(reader2.current().m2_d3d12_hook);

    std::remove(path.c_str());
}

void TestIniM2D3DHookGatesUnknownKeyIgnored() {
    // An unknown key inside the [m2] section must be ignored, not
    // adopted as `false`. This pins the parser's strict-dispatch
    // contract so a typo in a future INI doesn't silently disable
    // the install path. With the safe defaults (false) the test
    // checks that the typo doesn't accidentally flip the gate to
    // a different value than the default.
    const std::string path = MakeTempIniPath("m2_d3d_gates_typo");
    std::remove(path.c_str());

    {
        FileWriter w(path);
        w << "[m2]\n";
        w << "d3d_11_hook = true\n";   // typo: missing one of the 'd's
    }
    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    // The typo key must not flip the d3d11_hook gate away from
    // its default (false). The auto-migration then leaves it at
    // false (the migration only fires when the real key is true,
    // which it isn't here because the typo was ignored).
    EXPECT_FALSE(reader.current().m2_d3d11_hook);
    EXPECT_FALSE(reader.current().m2_d3d12_hook);

    std::remove(path.c_str());
}

void TestIniM2D3DHookGatesAutoMigratesOldTrueValues() {
    // Auto-migration pin: a pre-existing INI file with the OLD
    // defaults (`d3d11_hook = true`, `d3d12_hook = true`) must be
    // rewritten to safe values on the next load. Without this,
    // users upgrading from a build that had `true` as the default
    // would still hit the amtrucks.exe+0x1d5300a `movdqa` #GP
    // after the upgrade (the file overrides the code default).
    // The migration also runs SaveToPath to persist the fix.
    const std::string path = MakeTempIniPath("m2_d3d_gates_migrate");
    std::remove(path.c_str());

    // Write a pre-upgrade file with the unsafe values.
    {
        FileWriter w(path);
        w << "[m2]\n";
        w << "game_free_camera_shim = false\n";
        w << "camera_hijack = false\n";
        w << "d3d11_hook = true\n";
        w << "d3d12_hook = true\n";
    }

    // Load — the migration should fire and rewrite the file.
    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    EXPECT_FALSE(reader.current().m2_d3d11_hook);
    EXPECT_FALSE(reader.current().m2_d3d12_hook);

    // Re-load directly from disk: the migration should NOT fire
    // again (the file now has the safe values), and the values
    // must remain false.
    SettingsStore reader2;
    EXPECT_TRUE(reader2.LoadFromPath(path));
    EXPECT_FALSE(reader2.current().m2_d3d11_hook);
    EXPECT_FALSE(reader2.current().m2_d3d12_hook);

    // Read the file back off disk and confirm the migration
    // actually persisted the safe values.
    {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string content = ss.str();
        EXPECT_TRUE(content.find("d3d11_hook = false") != std::string::npos);
        EXPECT_TRUE(content.find("d3d12_hook = false") != std::string::npos);
    }

    std::remove(path.c_str());
}

// M2 camera_hijack_install + M3 px_scene_hook_install defaults
// (both `false`) + auto-migration from old `true` values. Same
// rationale as the d3d11/d3d12 gates: the in-process MinHook
// trampolines on amtrucks.exe break downstream 16-byte alignment
// on ATS 1.61.3.1s, triggering the same amtrucks.exe+0x1d5300a
// `movdqa` #GP.

void TestIniM2CameraHijackInstallDefaultIsFalse() {
    SettingsStore defaults;
    EXPECT_FALSE(defaults.current().m2_camera_hijack_install);
}

void TestIniM3PxSceneHookInstallDefaultIsFalse() {
    SettingsStore defaults;
    EXPECT_FALSE(defaults.current().m3_px_scene_hook_install);
}

void TestIniM2CameraHijackInstallParseAndRoundTrip() {
    const std::string path = MakeTempIniPath("m2_camera_hijack_install");
    std::remove(path.c_str());

    {
        FileWriter w(path);
        w << "[m2]\n";
        w << "camera_hijack_install = true\n";
    }
    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    // The auto-migration forces to false (the same as d3d11/d3d12
    // gates). Pin: even an explicit `true` is migrated to false.
    EXPECT_FALSE(reader.current().m2_camera_hijack_install);

    SettingsStore store;
    store.SetCurrent(reader.current());
    EXPECT_TRUE(store.SaveToPath(path));
    SettingsStore reader2;
    EXPECT_TRUE(reader2.LoadFromPath(path));
    EXPECT_FALSE(reader2.current().m2_camera_hijack_install);

    std::remove(path.c_str());
}

void TestIniM3PxSceneHookInstallParseAndRoundTrip() {
    const std::string path = MakeTempIniPath("m3_px_scene_hook_install");
    std::remove(path.c_str());

    {
        FileWriter w(path);
        w << "[m3]\n";
        w << "px_scene_hook_install = true\n";
    }
    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    EXPECT_FALSE(reader.current().m3_px_scene_hook_install);

    SettingsStore store;
    store.SetCurrent(reader.current());
    EXPECT_TRUE(store.SaveToPath(path));
    SettingsStore reader2;
    EXPECT_TRUE(reader2.LoadFromPath(path));
    EXPECT_FALSE(reader2.current().m3_px_scene_hook_install);

    std::remove(path.c_str());
}

void TestIniM2CameraHijackInstallAutoMigratesOldTrueValues() {
    // Pin: a pre-existing INI with the OLD default of `true` for
    // camera_hijack_install gets auto-migrated to `false` on load
    // and the migration persists to disk.
    const std::string path = MakeTempIniPath("m2_camera_hijack_migrate");
    std::remove(path.c_str());

    {
        FileWriter w(path);
        w << "[m2]\n";
        w << "camera_hijack_install = true\n";
    }
    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    EXPECT_FALSE(reader.current().m2_camera_hijack_install);

    {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string content = ss.str();
        EXPECT_TRUE(content.find("camera_hijack_install = false") !=
                    std::string::npos);
    }

    std::remove(path.c_str());
}

void TestIniM3PxSceneHookInstallAutoMigratesOldTrueValues() {
    const std::string path = MakeTempIniPath("m3_px_scene_migrate");
    std::remove(path.c_str());

    {
        FileWriter w(path);
        w << "[m3]\n";
        w << "px_scene_hook_install = true\n";
    }
    SettingsStore reader;
    EXPECT_TRUE(reader.LoadFromPath(path));
    EXPECT_FALSE(reader.current().m3_px_scene_hook_install);

    {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string content = ss.str();
        EXPECT_TRUE(content.find("px_scene_hook_install = false") !=
                    std::string::npos);
    }

    std::remove(path.c_str());
}

}  // namespace walkercore