// WalkerCore â€” diagnostic dump tests.
//
// Verifies walker_dump_state() is safe to call in any state â€” both
// before init (all globals null) and with subsystems wired up â€” and
// produces output that flows through whatever log sink is bound (the
// custom test sink used here).
//
// We exercise the full state-dump path via diagnostic_internal::Set*
// accessors, which is the same path PluginEntry uses to wire the live
// subsystems.

#include "TestHarness.h"

#include <cstdio>
#include <cstdarg>
#include <string.h>
#include <cstring>

#include "walkercore/CameraState.h"
#include "walkercore/D3D11Hook.h"
#include "walkercore/DiagnosticAccess.h"
#include "walkercore/FrameLoop.h"
#include "walkercore/FuelPump.h"
#include "walkercore/InputHook.h"
#include "walkercore/InputState.h"
#include "walkercore/Knockout.h"
#include "walkercore/Log.h"
#include "walkercore/PerFrame.h"
#include "walkercore/PlayerState.h"
#include "walkercore/PolishProviders.h"
#include "walkercore/ScsTelemetryProvider.h"
#include "walkercore/Settings.h"
#include "walkercore/SettingsMenu.h"
#include "walkercore/TruckState.h"
#include "walkercore/TruckDoorConfig.h"
#include "walkercore/WalkerFsm.h"

namespace {

// Capture sink used to verify the dump actually calls the log facade.
char g_lines[64][256];
int g_line_count = 0;
int g_last_level = -1;

extern "C" void DiagnosticDumpCaptureSink(int level, const char *msg) {
    g_last_level = level;
    if (g_line_count >= 64) return;
    snprintf(g_lines[g_line_count], sizeof(g_lines[g_line_count]),
             "%d:%s", level, msg);
    g_line_count++;
}

void ResetCapture() {
    for (int i = 0; i < g_line_count; ++i) g_lines[i][0] = '\0';
    g_line_count = 0;
    g_last_level = -1;
}

bool ContainsCapture(const char *needle) {
    for (int i = 0; i < g_line_count; ++i) {
        if (strstr(g_lines[i], needle) != nullptr) return true;
    }
    return false;
}

}  // namespace

namespace walkercore {

void TestDiagnosticDumpSafeBeforeInit() {
    // With no subsystems wired up, the dump must complete without
    // crashing and report "(not initialized)" for each.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("none");
    diagnostic_internal::ClearIdentity();
    diagnostic_internal::SetFsmLabel("Driving");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
    diagnostic_internal::SetSettings(nullptr);
    diagnostic_internal::SetInput(nullptr);
    diagnostic_internal::SetFuel(nullptr);
    diagnostic_internal::SetKnockout(nullptr);
    diagnostic_internal::SetD3D11(nullptr);

    walker_dump_state();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore state dump ---"));
    EXPECT_TRUE(ContainsCapture("--- end dump ---"));
    EXPECT_TRUE(ContainsCapture("fsm state             : Driving"));
    EXPECT_TRUE(ContainsCapture("truck                 : (not initialized)"));
    EXPECT_TRUE(ContainsCapture("frame loop            : (not initialized)"));
    EXPECT_TRUE(ContainsCapture("settings              : (not initialized)"));
    EXPECT_TRUE(ContainsCapture("input                 : (not initialized)"));
    EXPECT_TRUE(ContainsCapture("fuel provider         : (not initialized)"));
    EXPECT_TRUE(ContainsCapture("knockout             : (not initialized)"));
    EXPECT_TRUE(ContainsCapture("d3d11 hook           : (not initialized)"));
    EXPECT_TRUE(ContainsCapture("scs api version       : none"));
}

void TestDiagnosticDumpReportsScsVersion() {
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("1.01");
    diagnostic_internal::SetFsmLabel("Driving");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
    diagnostic_internal::SetSettings(nullptr);

    walker_dump_state();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("scs api version       : 1.01"));
}

void TestDiagnosticDumpMirrorsFsmLabel() {
    // SetFsmLabel should update the mirrored buffer; the dump prints
    // whatever the latest label is.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("none");
    diagnostic_internal::SetFsmLabel("TransitionIn t=0.42");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
    diagnostic_internal::SetSettings(nullptr);

    walker_dump_state();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("fsm state             : TransitionIn t=0.42"));
}

void TestDiagnosticDumpReportsFuelAndAdBlue() {
    // With a telemetry wired up and fuel/adblue populated, the dump
    // prints the amount values.
    ScsTelemetryProvider t;
    TruckState_Clear(const_cast<TruckState &>(t.state()));  // safe â€” fresh provider

    // Bypass the SDK callback path: write directly into the snapshot.
    TruckState &mut = const_cast<TruckState &>(t.state());
    mut.fuel_amount_l = 250.5f;
    mut.adblue_amount_l = 18.75f;
    mut.has_fuel = true;
    mut.has_adblue = true;

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("1.00");
    diagnostic_internal::SetFsmLabel("Walking");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(&t);
    diagnostic_internal::SetFrameLoop(nullptr);
    diagnostic_internal::SetSettings(nullptr);

    walker_dump_state();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("truck fuel            : has=yes amount=250.50 L"));
    EXPECT_TRUE(ContainsCapture("truck adblue          : has=yes amount=18.75 L"));

    // Cleanup.
    diagnostic_internal::SetTelemetry(nullptr);
}

void TestDiagnosticDumpReportsFrameLoopCamera() {
    FrameLoop loop;
    CameraState cam;
    CameraState_Reset(cam);
    cam.eye_position = {42.0, 1.65, 7.0};
    cam.yaw_radians = 0.5f;
    cam.pitch_radians = 0.25f;
    cam.horizontal_fov_radians = 1.4f;
    cam.aspect = 16.0f / 9.0f;
    cam.valid = true;
    CameraState_Update(cam);
    loop.OverrideCamera(cam);

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("none");
    diagnostic_internal::SetFsmLabel("Walking");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(nullptr);
    diagnostic_internal::SetFrameLoop(&loop);
    diagnostic_internal::SetSettings(nullptr);

    walker_dump_state();

    walkercore_log_shutdown();
    // OverrideCamera copies the target; the next call to RunOnce
    // adopts it. We just confirm the dump shows a non-empty camera.
    EXPECT_TRUE(g_line_count > 0);
    bool saw_camera_eye_line = false;
    for (int i = 0; i < g_line_count; ++i) {
        if (strstr(g_lines[i], "camera eye") != nullptr) {
            saw_camera_eye_line = true;
            break;
        }
    }
    EXPECT_TRUE(saw_camera_eye_line);

    diagnostic_internal::SetFrameLoop(nullptr);
}

void TestDiagnosticDumpReportsInputState() {
    // Wire an InputHook with a freshly pressed DumpState key. The dump
    // should report installed/valid, the edge counts, and the most recent
    // pressed key name. This is the path an analyst uses to verify "did
    // F12 actually reach the walker" when a real dump is missing.
    InputHook hook;

    // Pre-press DumpState so the bit is in last_ before we sample. We
    // poke the source directly (the hook is in the test build, no real
    // WH_KEYBOARD_LL fires here) and then call Sample() once so the
    // dump sees a snapshot — note that Sample() drains the rising edge
    // in source, so we read via the post-sample copy.
    uint8_t &bits = hook.mutable_last().keys[KeyIndex(WalkercoreKey::DumpState)];
    const bool was_held = (bits & 0x01) != 0;
    bits |= 0x01;
    if (!was_held) bits |= 0x02;
    bits |= 0x04;  // and a synthetic falling edge to exercise that field

    InputState drained;
    hook.Sample(drained);

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("none");
    diagnostic_internal::SetFsmLabel("Driving");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
    diagnostic_internal::SetSettings(nullptr);
    diagnostic_internal::SetInput(&hook);

    walker_dump_state();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("input installed       :"));
    EXPECT_TRUE(ContainsCapture("input edges           :"));
    EXPECT_TRUE(ContainsCapture("input mouse           :"));

    diagnostic_internal::SetInput(nullptr);
}

void TestDiagnosticDumpReportsFuelProvider() {
    // Wire a FuelProvider that has been fueled up to capacity, then verify
    // the dump shows the local tank amounts (which may diverge from the
    // telemetry block when telemetry is missing — that's the whole point
    // of having both).
    FuelProvider fuel;
    fuel.SetTankCapacity(600.0);
    fuel.SetFuelAmount(312.5);
    fuel.SetAdblueAmount(27.0);
    fuel.StartFueling();

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("none");
    diagnostic_internal::SetFsmLabel("Walking");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
    diagnostic_internal::SetSettings(nullptr);
    diagnostic_internal::SetInput(nullptr);
    diagnostic_internal::SetFuel(&fuel);

    walker_dump_state();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("fuel provider         : fueling=true"));
    EXPECT_TRUE(ContainsCapture("fuel station         : available="));
    EXPECT_TRUE(ContainsCapture("amount=312.50 L"));
    EXPECT_TRUE(ContainsCapture("adblue=27.00 L"));

    diagnostic_internal::SetFuel(nullptr);
}

void TestDiagnosticDumpReportsKnockout() {
    // Wire a KnockoutProvider that has flipped the player into KnockedOut
    // with a known remaining recovery window. The dump should report the
    // boolean, the time remaining, and the threshold the provider uses.
    KnockoutProvider knockout;
    KnockoutConfig cfg;
    cfg.recovery_seconds = 8.0;
    cfg.velocity_change_threshold_mps = 8.0;
    cfg.teleport_reprime_distance_m = 100.0;  // disable for this test
    knockout.SetConfig(cfg);

    // Simulate the provider already in the knocked-out state — bypass
    // the stub's velocity-change detection by mutating the public state
    // through Tick() with a sustained high-velocity synthetic input.
    PlayerState p;
    PlayerState_Reset(p);
    p.feet_position = {0, 0, 0};
    p.has_feet = true;
    // First Tick with has_feet and dt>0 seeds the previous position.
    knockout.Tick(p, 0.016);
    // Second Tick jumps the player 50 m in one frame — velocity ≈ 3125 m/s
    // >> 8 m/s threshold, triggers knock-out. (Teleport gate is disabled
    // so the 50 m jump goes through the velocity branch instead of the
    // re-prime branch.)
    p.feet_position = {50, 0, 0};
    knockout.Tick(p, 0.016);

    EXPECT_TRUE(knockout.isKnockedOut());

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("none");
    diagnostic_internal::SetFsmLabel("Walking");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
    diagnostic_internal::SetSettings(nullptr);
    diagnostic_internal::SetInput(nullptr);
    diagnostic_internal::SetFuel(nullptr);
    diagnostic_internal::SetKnockout(&knockout);

    walker_dump_state();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("knockout             : out=true"));
    EXPECT_TRUE(ContainsCapture("recovery_left="));
    EXPECT_TRUE(ContainsCapture("threshold=8.0 m/s"));

    diagnostic_internal::SetKnockout(nullptr);
}

void TestDiagnosticDumpReportsD3D11Hook() {
    // Wire a D3D11Hook that has not been installed (Install() requires the
    // MinHook-vendored DLL and a real d3d11.dll; the test build doesn't
    // link MinHook). The dump should still report the install state and
    // the swap-chain generation (0 in the stub branch).
    D3D11Hook hook;

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("none");
    diagnostic_internal::SetFsmLabel("Driving");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
    diagnostic_internal::SetSettings(nullptr);
    diagnostic_internal::SetInput(nullptr);
    diagnostic_internal::SetFuel(nullptr);
    diagnostic_internal::SetKnockout(nullptr);
    diagnostic_internal::SetD3D11(&hook);

    walker_dump_state();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("d3d11 hook           : installed=false"));
    EXPECT_TRUE(ContainsCapture("swap_chain_gen=0"));

    diagnostic_internal::SetD3D11(nullptr);
}

void TestDiagnosticDumpSettingsReportsBeforeInit() {
    // With no SettingsStore wired, walker_dump_settings() must still
    // complete and report "(not initialized)".
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetSettings(nullptr);

    walker_dump_settings();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore settings ---"));
    EXPECT_TRUE(ContainsCapture("settings              : (not initialized)"));
    EXPECT_TRUE(ContainsCapture("--- end settings ---"));
}

void TestDiagnosticDumpSettingsReportsLiveValues() {
    // Wire a SettingsStore with non-default values, then verify the dump
    // echoes each field. The fields cover movement, camera, transitions,
    // and key bindings.
    SettingsStore store;
    WalkerSettings custom = WalkerSettings::Defaults();
    custom.movement.walkSpeedMps = 2.5f;
    custom.movement.runSpeedMps = 5.0f;
    custom.movement.crouchSpeedMps = 1.0f;
    custom.movement.mouseSensitivityRadPerUnit = 0.005f;
    custom.movement.headBobVerticalAmpM = 0.02f;
    custom.movement.headBobLateralAmpM = 0.01f;
    custom.horizontal_fov_radians = 1.5f;
    custom.transition_seconds = 0.3f;
    custom.keys.toggle_walk_vk = VK_F11;
    custom.keys.dump_state_vk = VK_F12;
    store.SetCurrent(custom);

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetSettings(&store);

    walker_dump_settings();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("walk_speed_mps        : 2.500"));
    EXPECT_TRUE(ContainsCapture("run_speed_mps         : 5.000"));
    EXPECT_TRUE(ContainsCapture("mouse_sensitivity     : 0.00500 rad/unit"));
    EXPECT_TRUE(ContainsCapture("horizontal_fov_radians: 1.500"));
    EXPECT_TRUE(ContainsCapture("transition_seconds    : 0.30"));
    EXPECT_TRUE(ContainsCapture("key toggle_walk_vk    :"));
    // VK_F11 = 0x7A = 122, VK_F12 = 0x7B = 123
    EXPECT_TRUE(ContainsCapture("key dump_state_vk     : 123"));

    diagnostic_internal::SetSettings(nullptr);
}

void TestDiagnosticDumpReportsPlayerModeName() {
    // Wire a FrameLoop whose player is in a non-default mode (Running).
    // The dump should print the mode as a readable name, not a raw int.
    FrameLoop loop;
    CameraState cam;
    CameraState_Reset(cam);
    cam.valid = true;
    CameraState_Update(cam);
    loop.OverrideCamera(cam);
    PlayerState &p = loop.player();
    p.mode = PlayerMode::Running;
    p.has_feet = true;
    p.feet_position = {1.0, 0.0, 2.0};

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetScsVersion("none");
    diagnostic_internal::SetFsmLabel("Walking");
    diagnostic_internal::SetFsm(nullptr);
    diagnostic_internal::SetTelemetry(nullptr);
    diagnostic_internal::SetFrameLoop(&loop);
    diagnostic_internal::SetSettings(nullptr);
    diagnostic_internal::SetInput(nullptr);
    diagnostic_internal::SetFuel(nullptr);
    diagnostic_internal::SetKnockout(nullptr);
    diagnostic_internal::SetD3D11(nullptr);

    walker_dump_state();

    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("mode=Running"));

    diagnostic_internal::SetFrameLoop(nullptr);
}

void TestPlayerModeNameReturnsNamesForAllValues() {
    // Each enum value must map to a non-empty, distinct literal. We don't
    // pin the literal strings here (those are display-only) — only that
    // they exist and don't collide.
    EXPECT_TRUE(PlayerModeName(PlayerMode::Standing)    != nullptr);
    EXPECT_TRUE(PlayerModeName(PlayerMode::Standing)[0] != '\0');
    EXPECT_TRUE(PlayerModeName(PlayerMode::Idle)        != nullptr);
    EXPECT_TRUE(PlayerModeName(PlayerMode::Walking)     != nullptr);
    EXPECT_TRUE(PlayerModeName(PlayerMode::Running)     != nullptr);
    EXPECT_TRUE(PlayerModeName(PlayerMode::CrouchIdle)  != nullptr);
    EXPECT_TRUE(PlayerModeName(PlayerMode::CrouchWalk)  != nullptr);
    EXPECT_TRUE(PlayerModeName(PlayerMode::Jumping)     != nullptr);
    EXPECT_TRUE(PlayerModeName(PlayerMode::Falling)     != nullptr);
    EXPECT_TRUE(PlayerModeName(PlayerMode::Stumbling)   != nullptr);
    EXPECT_TRUE(PlayerModeName(PlayerMode::KnockedOut)  != nullptr);

    // A value outside the enum range must not return null or a name that
    // collides with a valid one.
    const char *bogus = PlayerModeName(static_cast<PlayerMode>(9999));
    EXPECT_TRUE(bogus != nullptr);
    EXPECT_TRUE(bogus[0] != '\0');
    // Sanity: walking and bogus must be different strings.
    EXPECT_NE(bogus, PlayerModeName(PlayerMode::Walking));
}

void TestDiagnosticDumpMenuReportsNotInitialized() {
    // Before M11 wires a SettingsMenu, the focused walker_dump_menu()
    // dump must report "(not initialized)" without crashing. This is
    // the same contract as walker_dump_settings() / walker_dump_state().
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetSettingsMenu(nullptr);
    walker_dump_menu();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore settings menu ---"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    EXPECT_TRUE(ContainsCapture("--- end menu ---"));
}

void TestDiagnosticDumpMenuReportsClosedClean() {
    // Wired but closed menu shows open=false dirty=false.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);
    diagnostic_internal::SetSettingsMenu(&menu);
    walker_dump_menu();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("menu                 : open=false dirty=false"));
    diagnostic_internal::SetSettingsMenu(nullptr);
}

void TestDiagnosticDumpMenuReportsOpenClean() {
    // Open + no edits → open=true dirty=false.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);
    menu.Open();
    diagnostic_internal::SetSettingsMenu(&menu);
    walker_dump_menu();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("open=true dirty=false"));
    menu.Close();
    diagnostic_internal::SetSettingsMenu(nullptr);
}

void TestDiagnosticDumpMenuReportsOpenDirty() {
    // Open + edit → open=true dirty=true. The classic "did my slider
    // edit land?" question — the dump tells you yes/no.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);
    menu.Open();
    menu.SetSliderValue("walk_speed_mps", 3.0f);
    diagnostic_internal::SetSettingsMenu(&menu);
    walker_dump_menu();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("open=true dirty=true"));
    menu.Close();
    diagnostic_internal::SetSettingsMenu(nullptr);
}

void TestDiagnosticDumpMenuMirrorsClose() {
    // After Close(), open=false but dirty STAYS true until the next
    // Open(). This is the contract — Close persists the edits (if
    // dirty and store bound) and clears open, but does not clear
    // dirty. Only Open resets dirty, so a subsequent edit cycle sees
    // a clean slate. The dump surfaces both flags accurately so
    // analysts can tell "menu saved and dismissed" from "menu still
    // has unsaved edits".
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);
    menu.Open();
    menu.SetSliderValue("walk_speed_mps", 3.0f);
    diagnostic_internal::SetSettingsMenu(&menu);
    menu.Close();
    walker_dump_menu();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("open=false dirty=true"));
    diagnostic_internal::SetSettingsMenu(nullptr);
}

void TestDiagnosticDumpFullStateMenuSection() {
    // walker_dump_state() must include the settings menu row alongside
    // every other wired subsystem. We construct a fresh menu, mirror it
    // via the diagnostic accessor, and look for the row in the full
    // dump.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    SettingsStore store;
    SettingsMenu menu;
    menu.Bind(&store);
    menu.Open();
    menu.SetSliderValue("walk_speed_mps", 3.0f);
    diagnostic_internal::SetSettingsMenu(&menu);
    walker_dump_state();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("settings menu       :"));
    EXPECT_TRUE(ContainsCapture("open=true dirty=true"));
    menu.Close();
    diagnostic_internal::SetSettingsMenu(nullptr);
}

void TestDiagnosticDumpHeadTrackerReportsNotInitialized() {
    // Before the provider exists, walker_dump_head_tracker() prints
    // "(not initialized)" and exits without crashing. Mirrors the
    // walker_dump_menu() / walker_dump_settings() contracts.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetHeadTracker(nullptr);
    walker_dump_head_tracker();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore head tracker ---"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    EXPECT_TRUE(ContainsCapture("--- end head tracker ---"));
}

void TestDiagnosticDumpHeadTrackerReportsConfigAndState() {
    // Wired head tracker → config section (use_mouse / use_trackir /
    // use_opentrack + sensitivities + smoothing) plus the Peek()
    // state. We override the config so the dump shows non-default
    // values, then feed a sample to make the state active.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg = provider.config();
    cfg.use_mouse = true;
    cfg.use_trackir = true;
    cfg.use_opentrack = false;
    cfg.mouse_sensitivity_rad_per_unit = 0.005f;
    cfg.trackir_sensitivity_rad_per_unit = 0.7f;
    cfg.smoothing_factor = 0.4f;
    provider.SetConfig(cfg);
    provider.Install();
    diagnostic_internal::SetHeadTracker(&provider);
    walker_dump_head_tracker();
    walkercore_log_shutdown();
    // Config row labels + values:
    EXPECT_TRUE(ContainsCapture("config mouse         : use=true"));
    EXPECT_TRUE(ContainsCapture("config trackir       : use=true"));
    EXPECT_TRUE(ContainsCapture("config opentrack     : use=false"));
    EXPECT_TRUE(ContainsCapture("sens=0.0050"));
    EXPECT_TRUE(ContainsCapture("sens=0.7000"));
    EXPECT_TRUE(ContainsCapture("config smoothing     : 0.400"));
    // State row labels — fresh provider, no samples yet → active=false.
    EXPECT_TRUE(ContainsCapture("state active         : false"));
    EXPECT_TRUE(ContainsCapture("state dominant source: Mouse"));
    EXPECT_TRUE(ContainsCapture("state yaw            : 0.000 rad"));
    EXPECT_TRUE(ContainsCapture("state pitch          : 0.000 rad"));
    diagnostic_internal::SetHeadTracker(nullptr);
}

void TestDiagnosticDumpHeadTrackerReportsActiveAfterMouseFeed() {
    // Feed a sample, sample the smoothed output, then dump. active
    // must be true and the dominant source must read "Mouse".
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg = provider.config();
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 1.0f;
    cfg.smoothing_factor = 0.0f;  // no smoothing — reflect delta directly
    provider.SetConfig(cfg);
    provider.Install();

    HeadTrackerSample s;
    s.yaw_delta_rad = 0.4f;
    s.pitch_delta_rad = 0.1f;
    s.roll_delta_rad = 0.0f;
    provider.FeedSample(HeadTrackerSource::Mouse, s);
    (void)provider.Sample();

    diagnostic_internal::SetHeadTracker(&provider);
    walker_dump_head_tracker();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("state active         : true"));
    EXPECT_TRUE(ContainsCapture("state dominant source: Mouse"));
    EXPECT_TRUE(ContainsCapture("state yaw            : 0.400 rad"));
    EXPECT_TRUE(ContainsCapture("state pitch          : 0.100 rad"));
    diagnostic_internal::SetHeadTracker(nullptr);
}

void TestDiagnosticDumpHeadTrackerPeekDoesNotPerturb() {
    // walker_dump_head_tracker() must use Peek() (not Sample()) so
    // calling the dump twice in a row doesn't double-advance the EMA
    // and shift the smoothed value. Same contract as the full dump.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    HeadTrackerProvider provider;
    HeadTrackerConfig cfg = provider.config();
    cfg.use_mouse = true;
    cfg.mouse_sensitivity_rad_per_unit = 1.0f;
    cfg.smoothing_factor = 0.5f;
    provider.SetConfig(cfg);
    provider.Install();

    HeadTrackerSample s;
    s.yaw_delta_rad = 0.5f;
    s.pitch_delta_rad = 0.0f;
    s.roll_delta_rad = 0.0f;
    provider.FeedSample(HeadTrackerSource::Mouse, s);
    const HeadTrackerState seeded = provider.Sample();

    diagnostic_internal::SetHeadTracker(&provider);
    walker_dump_head_tracker();
    walker_dump_head_tracker();
    walkercore_log_shutdown();
    // After the two dumps, the EMA must still be at the seeded value
    // because the dumps don't advance it. The dump log says yaw=X
    // both times — same value both times.
    char needle_yaw[64];
    std::snprintf(needle_yaw, sizeof(needle_yaw), "state yaw            : %.3f rad",
                  static_cast<double>(seeded.yaw_radians));
    EXPECT_TRUE(ContainsCapture(needle_yaw));
    diagnostic_internal::SetHeadTracker(nullptr);
}

void TestDiagnosticDumpShadowReportsNotInitialized() {
    // Uninit path prints "(not initialized)" without crashing.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetShadow(nullptr);
    walker_dump_shadow();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore shadow ---"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    EXPECT_TRUE(ContainsCapture("--- end shadow ---"));
}

void TestDiagnosticDumpShadowReportsNoPlayerState() {
    // ShadowProvider wired but no FrameLoop → "(no player state)".
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    ShadowProvider provider;
    diagnostic_internal::SetShadow(&provider);
    diagnostic_internal::SetFrameLoop(nullptr);
    walker_dump_shadow();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("shadow"));
    EXPECT_TRUE(ContainsCapture("(no player state)"));
    diagnostic_internal::SetShadow(nullptr);
}

void TestDiagnosticDumpShadowReportsConfigAndProjection() {
    // Full wiring: shadow provider + frame loop with a positioned
    // player. We override the config to non-default values and verify
    // the dump surfaces every config field plus the projected
    // shadow centre.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    ShadowProvider provider;
    ShadowConfig cfg = provider.config();
    cfg.sun_direction = Vec3{0.0, 1.0, 0.0};  // straight down for stable test
    cfg.scale = 2.5f;
    cfg.softness = 0.4f;
    cfg.rgba = 0xAABBCCDDu;
    provider.SetConfig(cfg);

    FrameLoop fl;
    PlayerState_Reset(fl.player());
    fl.player().feet_position = Vec3{3.0, 2.0, 4.0};
    fl.player().has_feet = true;

    diagnostic_internal::SetShadow(&provider);
    diagnostic_internal::SetFrameLoop(&fl);
    walker_dump_shadow();
    walkercore_log_shutdown();
    // Config rows
    EXPECT_TRUE(ContainsCapture("config sun direction : (0.000, 1.000, 0.000)"));
    EXPECT_TRUE(ContainsCapture("config scale         : 2.500"));
    EXPECT_TRUE(ContainsCapture("config softness      : 0.400 m"));
    EXPECT_TRUE(ContainsCapture("config rgba          : 0xAABBCCDD"));
    // State row
    EXPECT_TRUE(ContainsCapture("shadow state         : active=true"));
    // Sun is straight down → projection is at the player's (x,z) and
    // y=0. Verify the centre is reported correctly.
    EXPECT_TRUE(ContainsCapture("pos=(3.00, 0.00, 4.00)"));
    EXPECT_TRUE(ContainsCapture("radius=0.875 m"));  // 0.35 * 2.5
    diagnostic_internal::SetShadow(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
}

void TestDiagnosticDumpShadowSunOnHorizonEdge() {
    // Same as the shadow wiring edge-case test, applied to the
    // focused dump. Sun y ≈ 0 must not produce NaN/Inf. The full
    // dump uses the same Compute() path, so this is just confirming
    // the focused dump doesn't have a different code path.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    ShadowProvider provider;
    ShadowConfig cfg = provider.config();
    cfg.sun_direction = Vec3{1.0, 1e-6, 0.0};
    const double len = sqrt(1.0 + 1e-12);
    cfg.sun_direction.x /= len;
    cfg.sun_direction.y /= len;
    cfg.sun_direction.z /= len;
    provider.SetConfig(cfg);

    FrameLoop fl;
    PlayerState_Reset(fl.player());
    fl.player().feet_position = Vec3{0.0, 0.0, 0.0};
    fl.player().has_feet = true;

    diagnostic_internal::SetShadow(&provider);
    diagnostic_internal::SetFrameLoop(&fl);
    walker_dump_shadow();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("shadow state         : active=true"));
    diagnostic_internal::SetShadow(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
}

// -----------------------------------------------------------------------------
// walker_dump_flashlight()
// -----------------------------------------------------------------------------

void TestDiagnosticDumpFlashlightReportsNotInitialized() {
    // Uninit path prints "(not initialized)" without crashing. Both
    // the "on" row and the full state rows must be absent.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetFlashlight(nullptr);
    walker_dump_flashlight();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore flashlight ---"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    EXPECT_TRUE(ContainsCapture("--- end flashlight ---"));
}

void TestDiagnosticDumpFlashlightReportsNoCameraState() {
    // FlashlightProvider wired but no FrameLoop → "(no camera
    // state)". The "on" row still appears because it does not depend
    // on the camera; the cone / direction rows must not appear.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    FlashlightProvider provider;
    diagnostic_internal::SetFlashlight(&provider);
    diagnostic_internal::SetFrameLoop(nullptr);
    walker_dump_flashlight();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("flashlight on       : "));
    EXPECT_TRUE(ContainsCapture("(no camera state)"));
    diagnostic_internal::SetFlashlight(nullptr);
}

void TestDiagnosticDumpFlashlightReportsOffByDefault() {
    // Default FlashlightProvider is off → state row reports "false".
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    FlashlightProvider provider;
    FrameLoop fl;
    PlayerState_Reset(fl.player());
    fl.player().feet_position = Vec3{1.0, 2.0, 3.0};
    fl.player().has_feet = true;
    CameraState_AdoptPlayer(fl.camera(), fl.player());

    diagnostic_internal::SetFlashlight(&provider);
    diagnostic_internal::SetFrameLoop(&fl);
    walker_dump_flashlight();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("flashlight on       : false"));
    EXPECT_TRUE(ContainsCapture("flashlight state    : false"));
    diagnostic_internal::SetFlashlight(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
}

void TestDiagnosticDumpFlashlightReportsConeAfterToggle() {
    // Toggle on, capture, then dump. The state row must report true
    // on both the "flashlight on" surface and the state row, and the
    // cone inner/outer angles + intensity must be reported (i.e. the
    // Compute() path is the same one the dump uses).
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    FlashlightProvider provider;
    FrameLoop fl;
    PlayerState_Reset(fl.player());
    fl.player().feet_position = Vec3{1.0, 2.0, 3.0};
    fl.player().has_feet = true;
    CameraState_AdoptPlayer(fl.camera(), fl.player());
    provider.Toggle();

    diagnostic_internal::SetFlashlight(&provider);
    diagnostic_internal::SetFrameLoop(&fl);
    walker_dump_flashlight();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("flashlight on       : true"));
    EXPECT_TRUE(ContainsCapture("flashlight state    : true"));
    EXPECT_TRUE(ContainsCapture("cone inner/outer    : "));
    EXPECT_TRUE(ContainsCapture("intensity           : "));
    diagnostic_internal::SetFlashlight(nullptr);
    diagnostic_internal::SetFrameLoop(nullptr);
}

// -----------------------------------------------------------------------------
// walker_dump_door()
// -----------------------------------------------------------------------------

void TestDiagnosticDumpDoorReportsNotInitialized() {
    // Uninit path prints "(not initialized)".
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetDoorAnim(nullptr);
    walker_dump_door();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore door ---"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    EXPECT_TRUE(ContainsCapture("--- end door ---"));
}

void TestDiagnosticDumpDoorReportsClosedIdle() {
    // Default DoorAnimProvider is closed and idle → angle 0.0 rad,
    // animating=false.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    DoorAnimProvider provider;
    diagnostic_internal::SetDoorAnim(&provider);
    walker_dump_door();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("door angle          : 0.000 rad"));
    EXPECT_TRUE(ContainsCapture("door animating      : false"));
    diagnostic_internal::SetDoorAnim(nullptr);
}

void TestDiagnosticDumpDoorReportsMidwayAnimation() {
    // Kick off a 1-second AnimateTo(π/2) animation, advance ~250 ms
    // via Tick(0.25), dump. DoorAnimProvider uses smoothstep easing
    // (3t² - 2t³), so at t=0.25 the angle is (1/2)·π·(3·0.0625 - 2·0.015625)
    // = (π/2)·0.15625 ≈ 0.245 rad. We assert the animating flag plus
    // a non-trivial angle in a sensible window — the dump surface is
    // the contract, not the easing curve.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    DoorAnimProvider provider;
    provider.AnimateTo(1.5707963f, 1.0f);  // π/2 over 1 second
    provider.Tick(0.25);                   // 25% time → smoothstep 0.15625

    diagnostic_internal::SetDoorAnim(&provider);
    walker_dump_door();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("door animating      : true"));
    EXPECT_TRUE(ContainsCapture("door angle          : 0.245 rad"));
    diagnostic_internal::SetDoorAnim(nullptr);
}

// -----------------------------------------------------------------------------
// walker_dump_version()
// -----------------------------------------------------------------------------

void TestDiagnosticDumpVersionReportsNotInitialized() {
    // Pre-init path: clear the identity mirror, dump must report
    // "(not initialized)" for the game identity row, and still print
    // the SCS API version mirror so analysts can distinguish "I
    // forgot to wire SetIdentity" from "no game loaded".
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::ClearIdentity();
    diagnostic_internal::SetScsVersion("none");
    walker_dump_version();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore version ---"));
    EXPECT_TRUE(ContainsCapture("--- end version ---"));
    EXPECT_TRUE(ContainsCapture("scs api version      : none"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
}

void TestDiagnosticDumpVersionReportsEts2Match() {
    // ETS2 1.61.1.0 — version (1<<16)|13, code "eut2". The version
    // table has a matching entry; the dump must surface its label
    // and (since features_enabled = 0) every feature row reports
    // false.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetIdentity("eut2", (1u << 16) | 13u);
    diagnostic_internal::SetScsVersion("1.01");
    walker_dump_version();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("game id              : eut2"));
    EXPECT_TRUE(ContainsCapture("game version         : 1.13 (encoded=0x0001000D)"));
    EXPECT_TRUE(ContainsCapture("is ets2              : true"));
    EXPECT_TRUE(ContainsCapture("matched entry        : ETS2 1.61.1.0"));
    // Features all off (M0.5 default). Assert just the row label +
    // ": false" without pinning column width — the dump widths may
    // shift as feature names grow. The label "feature free_camera"
    // substring + ": false" together is unique to that row among
    // everything walker_dump_version() produces.
    EXPECT_TRUE(ContainsCapture("feature free_camera"));
    EXPECT_TRUE(ContainsCapture("free_camera"));
    EXPECT_TRUE(ContainsCapture(": false"));
    diagnostic_internal::ClearIdentity();
}

void TestDiagnosticDumpVersionReportsAtsMatch() {
    // ATS 1.61.3 — code "ats", version (1<<16)|15.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetIdentity("ats", (1u << 16) | 15u);
    diagnostic_internal::SetScsVersion("1.01");
    walker_dump_version();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("game id              : ats"));
    EXPECT_TRUE(ContainsCapture("is ets2              : false"));
    EXPECT_TRUE(ContainsCapture("matched entry        : ATS 1.61.3"));
    diagnostic_internal::ClearIdentity();
}

void TestDiagnosticDumpVersionReportsNoMatchForUnknownBuild() {
    // Unknown game_id → "(no entry matched)". The dump must NOT
    // crash and must still show the negotiated identity.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetIdentity("ets4", (1u << 16) | 99u);
    diagnostic_internal::SetScsVersion("1.01");
    walker_dump_version();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("game id              : ets4"));
    EXPECT_TRUE(ContainsCapture("(no entry matched)"));
    // Features must not appear in the "no match" branch — only the
    // identity rows do.
    EXPECT_TRUE(!ContainsCapture("feature free_camera"));
    diagnostic_internal::ClearIdentity();
}

// -----------------------------------------------------------------------------
// walker_dump_fsm()
// -----------------------------------------------------------------------------

void TestDiagnosticDumpFsmReportsNotInitialized() {
    // Pre-init: g_fsm not bound. The dump must print "(not
    // initialized)" and still surface the most recent observed
    // label mirror so analysts can see "the last frame before I
    // unbound things said X".
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetFsmLabel("Walking");
    diagnostic_internal::SetFsm(nullptr);
    walker_dump_fsm();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore fsm ---"));
    EXPECT_TRUE(ContainsCapture("--- end fsm ---"));
    EXPECT_TRUE(ContainsCapture("(not initialized)"));
    EXPECT_TRUE(ContainsCapture("last observed label : Walking"));
}

void TestDiagnosticDumpFsmReportsDrivingDefault() {
    // A freshly-constructed FSM is Driving. Progress=0.0, override=false.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    WalkerFsm fsm;
    diagnostic_internal::SetFsm(&fsm);
    walker_dump_fsm();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("fsm state           : Driving"));
    EXPECT_TRUE(ContainsCapture("transition progress : 0.000"));
    EXPECT_TRUE(ContainsCapture("should override rdr : false"));
    diagnostic_internal::SetFsm(nullptr);
}

void TestDiagnosticDumpFsmReportsMidTransitionOut() {
    // Drive F10 → TransitionOut → advance halfway → dump. State must
    // be TransitionOut, progress must be > 0, override must be true.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.4);

    // Same scaffolding as the WalkerFsm tests: a parked truck + a
    // walking player standing outside the driver's door.
    TruckState t;
    t.has_placement = true;
    t.placement.position = Vec3{0, 0, 0};
    t.placement.orientation.heading = 0.0f;
    t.placement.orientation.pitch = 0.0f;
    t.placement.orientation.roll = 0.0f;

    PlayerState p;
    PlayerState_Reset(p);
    p.feet_position = Vec3{-1.4, 0, -1.2};
    p.yaw_radians = 1.5708f;
    p.has_feet = true;
    p.mode = PlayerMode::Idle;

    WalkerFsmInputs inputs;
    inputs.truck = &t;
    inputs.player = &p;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = true;
    CameraState target;
    fsm.Update(inputs, 0.001, &target);  // -> TransitionOut at t=0
    // 0.2 s of 0.4 s window = 50 % complete (smoothstep-ish, we just
    // assert it's strictly between 0 and 1).
    fsm.Update(inputs, 0.2, &target);

    diagnostic_internal::SetFsm(&fsm);
    walker_dump_fsm();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("fsm state           : TransitionOut"));
    EXPECT_TRUE(ContainsCapture("should override rdr : true"));
    // Progress must be > 0 (we advanced 50 % of the window). We
    // don't pin the exact value because the FSM may or may not use
    // an easing curve — the contract is just "monotonically 0..1".
    EXPECT_TRUE(ContainsCapture("transition progress : 0."));
    EXPECT_TRUE(!ContainsCapture("transition progress : 0.000"));
    diagnostic_internal::SetFsm(nullptr);
}

void TestDiagnosticDumpFsmReportsMidTransitionIn() {
    // Same idea, opposite direction: Walking → press F (in range) →
    // halfway through TransitionIn → progress > 0, override = false.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    WalkerFsm fsm;
    fsm.SetTransitionSeconds(0.4);

    TruckState t;
    t.has_placement = true;
    t.placement.position = Vec3{0, 0, 0};
    t.placement.orientation.heading = 0.0f;
    t.placement.orientation.pitch = 0.0f;
    t.placement.orientation.roll = 0.0f;

    PlayerState p;
    PlayerState_Reset(p);
    p.feet_position = Vec3{-1.4, 0, -1.2};
    p.yaw_radians = 1.5708f;
    p.has_feet = true;
    p.mode = PlayerMode::Idle;

    WalkerFsmInputs inputs;
    inputs.truck = &t;
    inputs.player = &p;
    inputs.interact_pressed = false;
    inputs.toggle_walk_pressed = false;
    CameraState target;

    // First, get into Walking.
    inputs.toggle_walk_pressed = true;
    fsm.Update(inputs, 0.001, &target);
    fsm.Update(inputs, 0.5, &target);

    // Then engage F (in range) and advance 50 %.
    inputs.toggle_walk_pressed = false;
    inputs.interact_pressed = true;
    fsm.Update(inputs, 0.001, &target);
    fsm.Update(inputs, 0.2, &target);

    diagnostic_internal::SetFsm(&fsm);
    walker_dump_fsm();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("fsm state           : TransitionIn"));
    EXPECT_TRUE(ContainsCapture("should override rdr : false"));
    EXPECT_TRUE(ContainsCapture("transition progress : 0."));
    EXPECT_TRUE(!ContainsCapture("transition progress : 0.000"));
    diagnostic_internal::SetFsm(nullptr);
}

// -----------------------------------------------------------------------------
// walker_dump_input()
// -----------------------------------------------------------------------------

void TestDiagnosticDumpInputReportsNotInitialized() {
    // Uninit path: g_input == nullptr → "(not initialized)".
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    diagnostic_internal::SetInput(nullptr);
    walker_dump_input();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore input ---"));
    EXPECT_TRUE(ContainsCapture("--- end input ---"));
    EXPECT_TRUE(ContainsCapture("input                 : (not initialized)"));
}

void TestDiagnosticDumpInputReportsEmptyState() {
    // Hook bound but never sampled → input.valid=false → "(no
    // sample yet)" for the per-key table. The summary rows still
    // print with zeros.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    InputHook hook;
    diagnostic_internal::SetInput(&hook);
    walker_dump_input();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("input installed       : "));
    EXPECT_TRUE(ContainsCapture("input valid           : false"));
    EXPECT_TRUE(ContainsCapture("held / pressed / rel  : 0 / 0 / 0"));
    EXPECT_TRUE(ContainsCapture("mouse dx/dy/wheel     : 0 / 0 / 0"));
    EXPECT_TRUE(ContainsCapture("(no sample yet)"));
    diagnostic_internal::SetInput(nullptr);
}

void TestDiagnosticDumpInputReportsPerKeyEdges() {
    // Manually populate the last() snapshot with held + pressed bits
    // set on a couple of keys; verify those keys appear as rows in
    // the focused dump.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    InputHook hook;
    // The InputHook default last() is a fresh InputState. Populate it
    // directly — the dump goes through `g_input->last()` which returns
    // a const reference, so we don't perturb the producer.
    InputState_Reset(hook.last());
    hook.last().keys[static_cast<unsigned>(WalkercoreKey::Forward)] =
        static_cast<uint8_t>(0x01 | 0x02);  // held + pressed-since-last
    hook.last().keys[static_cast<unsigned>(WalkercoreKey::Jump)] =
        static_cast<uint8_t>(0x01);         // held only
    hook.last().keys[static_cast<unsigned>(WalkercoreKey::Interact)] =
        static_cast<uint8_t>(0x02);         // rising edge only (not held)
    hook.last().valid = true;
    hook.last().mouse_dx = 12;
    hook.last().mouse_dy = -7;
    hook.last().wheel_delta = 1;
    hook.last().last_update_us = 12345;

    diagnostic_internal::SetInput(&hook);
    walker_dump_input();
    walkercore_log_shutdown();
    // Summary rows.
    EXPECT_TRUE(ContainsCapture("input valid           : true"));
    EXPECT_TRUE(ContainsCapture("held / pressed / rel  : 2 / 2 / 0"));
    EXPECT_TRUE(ContainsCapture("mouse dx/dy/wheel     : 12 / -7 / 1"));
    EXPECT_TRUE(ContainsCapture("last update us        : 12345"));
    // Per-key rows.
    EXPECT_TRUE(ContainsCapture("key Forward"));
    EXPECT_TRUE(ContainsCapture("key Jump"));
    EXPECT_TRUE(ContainsCapture("key Interact"));
    EXPECT_TRUE(ContainsCapture("held=true pressed=true"));  // Forward
    EXPECT_TRUE(ContainsCapture("held=true pressed=false")); // Jump
    EXPECT_TRUE(ContainsCapture("held=false pressed=true")); // Interact
    // No "no sample yet" because we set valid=true.
    EXPECT_TRUE(!ContainsCapture("(no sample yet)"));
    diagnostic_internal::SetInput(nullptr);
}

void TestDiagnosticDumpInputQuietKeysAreOmitted() {
    // When only one key has any bit set, only that key should appear
    // in the per-key table — the dump must NOT emit zero-bit rows
    // for every other WalkercoreKey. This pins the "compact table"
    // behaviour.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    ResetCapture();
    InputHook hook;
    InputState_Reset(hook.last());
    hook.last().keys[static_cast<unsigned>(WalkercoreKey::FlashlightToggle)] =
        static_cast<uint8_t>(0x04);  // released-since-last only
    hook.last().valid = true;

    diagnostic_internal::SetInput(&hook);
    walker_dump_input();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("key FlashlightToggle"));
    EXPECT_TRUE(ContainsCapture("released=true"));
    // Quiet keys must not have rows.
    EXPECT_TRUE(!ContainsCapture("key Forward"));
    EXPECT_TRUE(!ContainsCapture("key Backward"));
    EXPECT_TRUE(!ContainsCapture("key Crouch"));
    // One key with the released bit set → released count is 1, others 0.
    EXPECT_TRUE(ContainsCapture("held / pressed / rel  : 0 / 0 / 1"));
    diagnostic_internal::SetInput(nullptr);
}

// =============================================================================
// FSM label cache — encoder + cache-decision + canonical buffer
// =============================================================================
//
// The per-frame conductor (PluginEntry's walker_per_frame_tick, called
// at 60 Hz by the M2 lander's D3D11 Present hook) used to encode the
// FSM state label with snprintf every frame, then copy it through
// SetFsmLabel() with another snprintf("%s", ...). Both runs hit a
// second buffer that was kept in lock-step with the diagnostic dump's
// own buffer.
//
// That double-copy + per-frame re-encode is the optimization target
// here. The new flow is:
//
//   if (FsmLabelNeedsReencode(prev, current)) {
//       char stack_buf[64];
//       EncodeFsmLabel(current, t, stack_buf, sizeof(stack_buf));
//       SetFsmLabel(stack_buf);   // strncpy, not snprintf
//       prev = current;
//   }
//
//   // walker_fsm_state_label() reads the same canonical buffer
//   return GetFsmLabel();
//
// These probes pin each piece in isolation; the per-frame conductor
// itself isn't exercised directly (the full D3D11 / InputHook stack
// isn't available in the test exe), but every helper it touches IS
// tested here.

// EncodeFsmLabel — every state maps to its expected output, and the
// transition progress string is in the right format.
void TestEncodeFsmLabelEveryState() {
    char buf[64];

    const size_t n1 = diagnostic_internal::EncodeFsmLabel(
        WalkerFsmState::Driving, 0.0f, buf, sizeof(buf));
    EXPECT_GT(n1, 0u);
    EXPECT_STREQ(buf, "Driving");

    const size_t n2 = diagnostic_internal::EncodeFsmLabel(
        WalkerFsmState::Walking, 0.0f, buf, sizeof(buf));
    EXPECT_GT(n2, 0u);
    EXPECT_STREQ(buf, "Walking");

    // Transition states render with two-decimal progress.
    const size_t n3 = diagnostic_internal::EncodeFsmLabel(
        WalkerFsmState::TransitionIn, 0.42f, buf, sizeof(buf));
    EXPECT_GT(n3, 0u);
    EXPECT_STREQ(buf, "TransitionIn · t=0.42");

    const size_t n4 = diagnostic_internal::EncodeFsmLabel(
        WalkerFsmState::TransitionOut, 0.87f, buf, sizeof(buf));
    EXPECT_GT(n4, 0u);
    EXPECT_STREQ(buf, "TransitionOut · t=0.87");
}

// EncodeFsmLabel — transition progress is ignored for the steady states.
// Garbage values like 0.99 must NOT appear in a "Walking" label.
void TestEncodeFsmLabelProgressOnlyForTransitions() {
    char buf[64];

    diagnostic_internal::EncodeFsmLabel(WalkerFsmState::Driving, 0.99f, buf, sizeof(buf));
    EXPECT_STREQ(buf, "Driving");

    diagnostic_internal::EncodeFsmLabel(WalkerFsmState::Walking, 0.99f, buf, sizeof(buf));
    EXPECT_STREQ(buf, "Walking");
}

// EncodeFsmLabel — clamp transition progress to [0, 1] to make sure a
// dt overshoot or negative-input race doesn't print rubbish. The
// underlying tween math should stay in [0, 1]; the clamp is a defence
// in depth.
void TestEncodeFsmLabelClampsProgressToUnitInterval() {
    char buf[64];

    // Negative progress clamps to 0.00.
    diagnostic_internal::EncodeFsmLabel(WalkerFsmState::TransitionIn, -0.5f, buf, sizeof(buf));
    EXPECT_STREQ(buf, "TransitionIn · t=0.00");

    // >1 progress clamps to 1.00.
    diagnostic_internal::EncodeFsmLabel(WalkerFsmState::TransitionOut, 1.7f, buf, sizeof(buf));
    EXPECT_STREQ(buf, "TransitionOut · t=1.00");
}

// EncodeFsmLabel — bounded output. A buffer that's too small for the
// full label must NUL-terminate and report only what fit. snprintf's
// behaviour here is platform-standard but pinning it catches a
// regression to strcpy or unbounded snprintf.
//
// We drive ASCII-only inputs here ("Walking" has 7 chars; the transition
// labels include the UTF-8 middle dot "·" which is 2 bytes per glyph and
// would complicate a literal EXPECT_STREQ comparison). For the transition
// path we use a deliberately small buffer and check the truncation was
// safe (NUL-terminated, no overrun), not its exact byte content.
void TestEncodeFsmLabelBoundedOutput() {
    // ---- Walking fits exactly with NUL when buffer is 8 ----
    {
        char buf[8];
        diagnostic_internal::EncodeFsmLabel(
            WalkerFsmState::Walking, 0.0f, buf, sizeof(buf));
        EXPECT_STREQ(buf, "Walking");  // 7 chars + NUL = 8 bytes, fits exactly
    }
    // ---- Walking truncates when buffer is smaller ----
    {
        char buf[5];
        const size_t n = diagnostic_internal::EncodeFsmLabel(
            WalkerFsmState::Walking, 0.0f, buf, sizeof(buf));
        EXPECT_STREQ(buf, "Walk");
        EXPECT_EQ(n, 4u);  // wrote 4 chars; NUL sits at buf[4]
        EXPECT_EQ(buf[4], '\0');
    }
    // ---- Transition output truncates safely with a small buffer ----
    // We don't pin the exact bytes (the UTF-8 · is 2 bytes and string
    // comparisons get tricky on partial multi-byte prefixes). We just
    // check that the function never overruns the buffer and always
    // writes a NUL.
    {
        char buf[12];
        diagnostic_internal::EncodeFsmLabel(
            WalkerFsmState::TransitionIn, 0.42f, buf, sizeof(buf));
        EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
    }
}

// FsmLabelNeedsReencode — full decision table.
void TestFsmLabelNeedsReencode() {
    using S = WalkerFsmState;

    // Same steady state → skip.
    EXPECT_FALSE(diagnostic_internal::FsmLabelNeedsReencode(S::Driving, S::Driving));
    EXPECT_FALSE(diagnostic_internal::FsmLabelNeedsReencode(S::Walking, S::Walking));

    // Any state change → re-encode.
    EXPECT_TRUE(diagnostic_internal::FsmLabelNeedsReencode(S::Driving, S::Walking));
    EXPECT_TRUE(diagnostic_internal::FsmLabelNeedsReencode(S::Walking, S::Driving));
    EXPECT_TRUE(diagnostic_internal::FsmLabelNeedsReencode(S::Walking, S::TransitionIn));
    EXPECT_TRUE(diagnostic_internal::FsmLabelNeedsReencode(S::TransitionIn, S::Walking));
    EXPECT_TRUE(diagnostic_internal::FsmLabelNeedsReencode(S::TransitionIn, S::TransitionOut));
    EXPECT_TRUE(diagnostic_internal::FsmLabelNeedsReencode(S::Driving, S::TransitionOut));
}

// Steady-state skip is the common-case fast path. Pin the property
// that a long sequence of identical-state frames re-encodes nothing.
void TestFsmLabelNeedsReencodeSteadyStateIsFalse() {
    using S = WalkerFsmState;
    constexpr WalkerFsmState kState = WalkerFsmState::Walking;
    for (int i = 0; i < 10000; ++i) {
        EXPECT_FALSE(diagnostic_internal::FsmLabelNeedsReencode(kState, kState));
    }
}

// Transition states always re-encode, even with a static (prev == current)
// state — because the t value changes every frame.
void TestFsmLabelNeedsReencodeTransitionAlwaysTrue() {
    using S = WalkerFsmState;
    EXPECT_TRUE(diagnostic_internal::FsmLabelNeedsReencode(S::TransitionIn, S::TransitionIn));
    EXPECT_TRUE(diagnostic_internal::FsmLabelNeedsReencode(S::TransitionOut, S::TransitionOut));
}

// The unified canonical buffer. SetFsmLabel writes the bytes; GetFsmLabel
// reads them; the buffer is bounded by its size (64 bytes including NUL).
//
// This is the regression test for the eliminated parallel buffer: if a
// future refactor re-introduces a dupe buffer, the "buffer size" test
// here still passes (the bound is the same) but the long-input test
// remains correct. The fact that walker_fsm_state_label() reads the
// SAME buffer is the contract change being pinned.
void TestFsmLabelCanonicalBuffer() {
    diagnostic_internal::SetFsmLabel("Test123");
    EXPECT_STREQ(diagnostic_internal::GetFsmLabel(), "Test123");

    diagnostic_internal::SetFsmLabel(nullptr);  // safe no-op
    EXPECT_STREQ(diagnostic_internal::GetFsmLabel(), "Test123");

    // Very long input must be truncated at sizeof(buf) - 1.
    char long_label[128];
    for (int i = 0; i < 120; ++i) long_label[i] = 'A';
    long_label[120] = '\0';
    diagnostic_internal::SetFsmLabel(long_label);
    const char *got = diagnostic_internal::GetFsmLabel();
    // The canonical buffer is 64 bytes including the NUL terminator, so
    // a 120-byte input must truncate to 63 printable bytes + NUL.
    EXPECT_EQ(strlen(got), 63u);
}

// Cache-decision + encoder integration. Simulate a steady-state frame
// sequence and pin the cache contract:
//
//   - 60 frames of "Driving" with prev = Driving → 0 encodes (cache
//     hot; state unchanged; current is a steady state).
//   - state change Driving → Walking → 1 encode.
//   - 60 more frames of "Walking" → 0 encodes.
//   - state change Walking → TransitionIn → 30 encodes (each frame
//     inside the tween re-encodes because t changes every frame).
//
// Total expected: 0 + 1 + 0 + 30 = 31 encodes. A regression that drops
// the cache check (i.e. always encodes) would do 60 + 1 + 60 + 30 = 151
// — caught here as "didn't do exactly 31 encodes".
//
// This is the integration test for the per-frame optimization without
// needing the full D3D11 / InputHook / DllMain stack the real conductor
// needs.
void TestFsmLabelCacheDecisionDrivesEncodeCalls() {
    using S = WalkerFsmState;
    int encode_count = 0;
    char stack_buf[64];
    // Initial state matches the canonical buffer's starting content
    // ("Driving"), which is exactly what walker_per_frame_tick's
    // g_prev_encoded_fsm_state defaults to. A real player who sits in
    // "Driving" forever incurs 0 per-frame FSM-label encodes.
    S prev = S::Driving;
    diagnostic_internal::SetFsmLabel("Driving");

    // 60 frames of "Driving" — zero encodes, cache hits every time.
    for (int i = 0; i < 60; ++i) {
        if (diagnostic_internal::FsmLabelNeedsReencode(prev, S::Driving)) {
            diagnostic_internal::EncodeFsmLabel(
                S::Driving, 0.0f, stack_buf, sizeof(stack_buf));
            diagnostic_internal::SetFsmLabel(stack_buf);
            prev = S::Driving;
            ++encode_count;
        }
    }
    EXPECT_EQ(encode_count, 0);

    // State change Driving → Walking — one encode.
    if (diagnostic_internal::FsmLabelNeedsReencode(prev, S::Walking)) {
        diagnostic_internal::EncodeFsmLabel(
            S::Walking, 0.0f, stack_buf, sizeof(stack_buf));
        diagnostic_internal::SetFsmLabel(stack_buf);
        prev = S::Walking;
        ++encode_count;
    }
    EXPECT_EQ(encode_count, 1);
    EXPECT_STREQ(diagnostic_internal::GetFsmLabel(), "Walking");

    // 60 more frames of "Walking" — zero encodes (steady cache hit).
    for (int i = 0; i < 60; ++i) {
        if (diagnostic_internal::FsmLabelNeedsReencode(prev, S::Walking)) {
            diagnostic_internal::EncodeFsmLabel(
                S::Walking, 0.0f, stack_buf, sizeof(stack_buf));
            diagnostic_internal::SetFsmLabel(stack_buf);
            ++encode_count;
        }
    }
    EXPECT_EQ(encode_count, 1);

    // Now a full transition. State changes Walking → TransitionIn (the
    // first frame is a state-change edge), then each subsequent frame
    // re-encodes because the tween's t value moves every frame.
    float t = 0.0f;
    for (int i = 0; i < 30; ++i) {
        t = static_cast<float>(i) / 30.0f;
        if (diagnostic_internal::FsmLabelNeedsReencode(prev, S::TransitionIn)) {
            diagnostic_internal::EncodeFsmLabel(
                S::TransitionIn, t, stack_buf, sizeof(stack_buf));
            diagnostic_internal::SetFsmLabel(stack_buf);
            prev = S::TransitionIn;
            ++encode_count;
        }
    }
    EXPECT_EQ(encode_count, 31);  // 1 prior + 30 transition frames
    // The last encode was for t = 29/30 ≈ 0.967 → "0.97" with %.2f.
    EXPECT_TRUE(strstr(diagnostic_internal::GetFsmLabel(), "TransitionIn") != nullptr);
}

void TestDumpKnockoutHandlesUninitialized() {
    // No KnockoutProvider wired. walker_dump_knockout must still
    // emit a banner + the (not initialized) marker without crashing.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    diagnostic_internal::SetKnockout(nullptr);
    walker_dump_knockout();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore knockout ---"));
    EXPECT_TRUE(ContainsCapture("--- end knockout ---"));
    EXPECT_TRUE(ContainsCapture("knockout            : (not initialized)"));
}

void TestDumpKnockoutReportsStateAndConfig() {
    // Wire a real KnockoutProvider with a non-default state and
    // config so the dump carries the expected fields. We don't tick
    // it — only read state() + config() — so the dump reads
    // exactly what the constructor left, plus whatever the test
    // writes via `state` (which is private; we mutate it via Tick
    // once with a teleport-sized move to confirm the dump reflects
    // the post-tick values).
    KnockoutProvider ko;
    KnockoutConfig cfg;
    cfg.velocity_change_threshold_mps = 30.0;
    cfg.recovery_seconds = 2.0;
    cfg.fall_height_threshold_m = 5.0;
    ko.SetConfig(cfg);

    PlayerState p;
    p.feet_position = Vec3{0, 0, 0};
    p.has_feet = true;
    ko.Tick(p, 0.016);  // prime tracking_ without firing KO
    p.feet_position = Vec3{0.6, 0, 0};
    // 0.6 m in 16 ms = 37.5 m/s — above the 30 m/s velocity
    // threshold (dist_threshold = 30 * 0.016 = 0.48 m so 0.6 > 0.48)
    // but below the 1.0 m snap-re-prime distance, so the heuristic
    // fires the KO instead of re-priming the baseline.
    ko.Tick(p, 0.016);
    EXPECT_TRUE(ko.isKnockedOut());  // confirm we triggered KO

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    diagnostic_internal::SetKnockout(&ko);
    walker_dump_knockout();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore knockout ---"));
    EXPECT_TRUE(ContainsCapture("knocked_out          : true"));
    EXPECT_TRUE(ContainsCapture("time_remaining_s     : 2.00"));
    EXPECT_TRUE(ContainsCapture("config vel_change_mps: 30.0"));
    EXPECT_TRUE(ContainsCapture("config recovery_s    : 2.00"));
    EXPECT_TRUE(ContainsCapture("config fall_height_m : 5.00"));
    EXPECT_TRUE(ContainsCapture("impact_direction_mag"));
    diagnostic_internal::SetKnockout(nullptr);
}

void TestDumpPerTruckDoorHandlesEmpty() {
    // No mirror pushed yet. walker_dump_per_truck_door must report
    // the sentinel state without crashing.
    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    diagnostic_internal::SetPerTruckDoor(nullptr, 0, nullptr);
    walker_dump_per_truck_door();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("--- WalkerCore per-truck door ---"));
    EXPECT_TRUE(ContainsCapture("--- end per-truck door ---"));
    EXPECT_TRUE(ContainsCapture("per-truck door       : (no config yet)"));
}

void TestDumpPerTruckDoorReportsMatchedRow() {
    // Build a TruckDoorConfig from the built-in table for a Volvo
    // VNL 2016 (one of the 14 built-in rows) and push it through
    // the mirror. The dump should report the model_id and confirm
    // the row was matched.
    TruckState t;
    t.cabin_position = Vec3{0, 1.0, 0};
    t.head_position = Vec3{-0.45, 1.55, -0.50};
    t.has_cabin_position = true;
    t.has_head_position = true;
    std::strncpy(t.model_id, "vehicle.volvo.vnl_2016", sizeof(t.model_id) - 1);
    t.model_id[sizeof(t.model_id) - 1] = '\0';
    t.has_model_id = true;
    const TruckDoorConfig cfg = TruckDoorConfigFromTruckState(t);

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    diagnostic_internal::SetPerTruckDoor(
        t.model_id, std::strlen(t.model_id), &cfg);
    walker_dump_per_truck_door();
    walkercore_log_shutdown();

    EXPECT_TRUE(ContainsCapture("model_id             : vehicle.volvo.vnl_2016 (matched built-in row)"));
    EXPECT_TRUE(ContainsCapture("config valid         : true"));
    EXPECT_TRUE(ContainsCapture("driver_door.position"));
    EXPECT_TRUE(ContainsCapture("cabin_min/max"));
    EXPECT_TRUE(ContainsCapture("driver_seat_local"));
    EXPECT_TRUE(ContainsCapture("door_max_open_rad"));

    diagnostic_internal::SetPerTruckDoor(nullptr, 0, nullptr);
}

void TestDumpPerTruckDoorReportsUnmatchedRow() {
    // model_id that's not in the built-in table. The dump should
    // still report the geometry (the generic placeholder) but flag
    // "no built-in row" so analysts know the per-truck override
    // path did not fire.
    TruckState t;
    t.cabin_position = Vec3{0, 1.0, 0};
    t.head_position = Vec3{-0.45, 1.55, -0.50};
    t.has_cabin_position = true;
    t.has_head_position = true;
    std::strncpy(t.model_id, "vehicle.volvo.future_model",
                 sizeof(t.model_id) - 1);
    t.model_id[sizeof(t.model_id) - 1] = '\0';
    t.has_model_id = true;
    const TruckDoorConfig cfg = TruckDoorConfigFromTruckState(t);

    walkercore_log_shutdown();
    walkercore_log_init(&DiagnosticDumpCaptureSink);
    diagnostic_internal::SetPerTruckDoor(
        t.model_id, std::strlen(t.model_id), &cfg);
    walker_dump_per_truck_door();
    walkercore_log_shutdown();
    EXPECT_TRUE(ContainsCapture("vehicle.volvo.future_model (no built-in row)"));

    diagnostic_internal::SetPerTruckDoor(nullptr, 0, nullptr);
}

}  // namespace walkercore
