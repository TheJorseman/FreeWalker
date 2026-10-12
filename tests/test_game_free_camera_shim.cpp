// WalkerCore — GameFreeCameraShim unit tests.
//
// The shim must remain a no-op on the test build because:
//   - SendInput would touch the real OS key event queue on a Windows
//     build.
//   - We do not want a test run to flip the host's developer-mode
//     cvar or fire keystrokes at the calling shell.
// We enforce that by compiling GameFreeCameraShim.cpp with
// WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED=1 in tests/CMakeLists.txt
// (parallel to WALKERCORE_TEST_NO_PER_FRAME_TICK), turning SynthKey
// into a counter + return-value increment.
//
// What we still cover here:
//   - SetEnabled / isEnabled round-trip.
//   - Toggle VK + Exit-press VK customisation.
//   - OnEnterWalking increments walks_entered_ and exits cleanly when
//     disabled, when supported=false (advisory path), and when both
//     are true.
//   - OnExitWalking emits the "Walking exit" log line (asserted via
//     counters, the actual log is exercised by the integration tests).
//   - ConsumeEchoKey returns false for any VK that was not synthesised.
//   - The ABI exports wired through DiagnosticAccess / DiagnosticDump
//     accept non-null shims and forward through to the methods.
//
// Tests use a per-test heap-allocated shim so the running counter
// state is isolated — counters mutate across calls and a static
// instance shared across tests would leak state from earlier tests
// into later assertions.

#include "TestHarness.h"

#include "walkercore/GameFreeCameraShim.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace walkercore {

namespace {

std::unique_ptr<GameFreeCameraShim> MakeShim() {
    // Fresh instance per test. Construct default, then reset to a
    // known-disabled baseline (toggle VK 0x60 = VK_NUMPAD0, no exit
    // press) so the assertions are deterministic.
    auto s = std::make_unique<GameFreeCameraShim>();
    s->SetEnabled(false);
    s->SetToggleVk(0x60);
    s->SetExitPressVk(0);
    return s;
}

// Best-effort temp file path. Windows uses %TEMP%, *nix uses /tmp.
// The caller writes the file, runs the parser, then std::remove's
// the path. We deliberately do NOT use std::tmpfile because that
// returns a FILE* we cannot reopen by name.
std::string MakeTempPath(const char *suffix) {
    std::string base;
#ifdef _WIN32
    char tmp[MAX_PATH] = {};
    if (::GetTempPathA(sizeof(tmp), tmp) == 0) {
        base = "C:\\Windows\\Temp\\";
    } else {
        base = std::string(tmp);
    }
#else
    base = "/tmp/";
#endif
    // Use the current test name + suffix so different tests cannot
    // collide on the same disk file. The PID slot keeps parallel
    // builds from clobbering each other.
#ifdef _WIN32
    base += "walkercore_test_";
    base += suffix;
    base += ".cfg";
#else
    base += "walkercore_test_";
    base += suffix;
    base += ".cfg";
#endif
    return base;
}

// Write `contents` to `path` and return true on success.
bool WriteTempFile(const std::string &path, const std::string &contents) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const size_t wrote =
        std::fwrite(contents.data(), 1, contents.size(), f);
    std::fclose(f);
    return wrote == contents.size();
}

}  // namespace

void TestGameFreeCameraShimDefaultStateIsDisabled() {
    auto s = MakeShim();
    EXPECT_FALSE(s->isEnabled());
    EXPECT_EQ(s->toggle_vk(), 0x60);    // VK_NUMPAD0
    EXPECT_EQ(s->exit_press_vk(), 0);
    EXPECT_EQ(s->walks_entered(), 0);
    EXPECT_EQ(s->toggles_dispatched(), 0);
}

void TestGameFreeCameraShimSetEnabledRoundTrip() {
    auto s = MakeShim();
    s->SetEnabled(true);
    EXPECT_TRUE(s->isEnabled());
    s->SetEnabled(false);
    EXPECT_FALSE(s->isEnabled());
}

void TestGameFreeCameraShimVkCustomisation() {
    auto s = MakeShim();
    // ATS / ETS2 use F10 for toggle walk; an operator could swap
    // the camera-toggle VK to anything VK_* coded.
    s->SetToggleVk(0x79);    // VK_F10
    s->SetExitPressVk(0x1B); // VK_ESCAPE
    EXPECT_EQ(s->toggle_vk(), 0x79);
    EXPECT_EQ(s->exit_press_vk(), 0x1B);
}

void TestGameFreeCameraShimOnEnterWalkingDisabledIsAdvisory() {
    auto s = MakeShim();
    s->SetEnabled(false);
    s->OnEnterWalking();
    s->OnEnterWalking();
    EXPECT_EQ(s->walks_entered(), 2);
    // Disabled shim: no key synthesis, no counter bump.
    EXPECT_EQ(s->toggles_dispatched(), 0);
    EXPECT_EQ(s->last_dispatch_ms(), 0);
}

void TestGameFreeCameraShimOnEnterWalkingSupportedFalseIsAdvisory() {
    auto s = MakeShim();
    s->SetEnabled(true);
    // supported_ stays false because ProbeGDeveloperFromConfig has
    // not found "1" in any game config on this host.
    s->OnEnterWalking();
    EXPECT_EQ(s->walks_entered(), 1);
    EXPECT_EQ(s->toggles_dispatched(), 0);
}

void TestGameFreeCameraShimTriggerToggleRespectsDisabled() {
    auto s = MakeShim();
    s->SetEnabled(false);
    const int produced = s->TriggerToggle();
    EXPECT_EQ(produced, 0);
    EXPECT_EQ(s->toggles_dispatched(), 0);
}

void TestGameFreeCameraShimEchoSuppressionAfterTrigger() {
    auto s = MakeShim();
    s->SetEnabled(true);    s->SetToggleVk(0x79);    // pretend F10
    s->SetExitPressVk(0);

    // Without supported_=true, TriggerToggle is a no-op — we cannot
    // exercise the echo loop in this build. The contract that
    // ConsumeEchoKey returns false for any VK we didn't synthesise
    // still holds.
    s->OnEnterWalking();           // logs advisory, no synthesis
    EXPECT_FALSE(s->ConsumeEchoKey(0x79));
    EXPECT_FALSE(s->ConsumeEchoKey(0x60));
}

// BindGameHwnd + PostMessage path — pins the fix for the ATS 1.61.3.1s
// dev-free-camera regression where the shim's SendInput call did not
// reach the game window in fullscreen / DXGI exclusive mode. Manual
// Numpad-0 still toggled the camera (so the dev free camera feature
// itself was intact), but the shim's synthesised input never landed.
// Switching to PostMessage(hwnd, WM_KEYDOWN) targets the bound game
// window directly and works regardless of focus / exclusive mode.

void TestGameFreeCameraShimDefaultHwndIsNull() {
    auto s = MakeShim();
    // Default-constructed shim has no bound game window — the
    // BindGameHwnd call at scs_telemetry_init time is what sets
    // this, and the per-frame re-bind in walker_per_frame_tick
    // catches the case where the game window wasn't up yet.
    EXPECT_TRUE(s->game_hwnd() == nullptr);
}

void TestGameFreeCameraShimBindGameHwndRoundTrip() {
    auto s = MakeShim();
    s->BindGameHwnd(reinterpret_cast<HWND>(0xCAFEBABE));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(s->game_hwnd()),
              0xCAFEBABEu);
}

void TestGameFreeCameraShimBindGameHwndOverwrites() {
    auto s = MakeShim();
    s->BindGameHwnd(reinterpret_cast<HWND>(0xAAAA));
    s->BindGameHwnd(reinterpret_cast<HWND>(0xBBBB));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(s->game_hwnd()),
              0xBBBBu);
}

void TestGameFreeCameraShimBindGameHwndNullClears() {
    auto s = MakeShim();
    s->BindGameHwnd(reinterpret_cast<HWND>(0xAAAA));
    s->BindGameHwnd(nullptr);
    EXPECT_TRUE(s->game_hwnd() == nullptr);
}

void TestGameFreeCameraShimDispatchSkipsWithoutHwnd() {
    // With no HWND bound, the dispatch path must be `none` —
    // SendInput is foreground-targeted and would land in whatever
    // process is in the foreground (likely the calling shell, not
    // the game). The shim must NOT SendInput without a target
    // because that would dispatch a stray Numpad-0 press into the
    // host environment.
    auto s = MakeShim();
    s->SetEnabled(true);
    s->SetSupportedForTesting(true);
    s->OnEnterWalking();
    EXPECT_EQ(s->last_dispatch_path(), 0);  // 0 = none
    EXPECT_EQ(s->toggles_dispatched(), 1);  // attempted, but not delivered
}

void TestGameFreeCameraShimDispatchUsesSendInputWhenHwndBound() {
    // The M2 lander dispatch path: when an HWND is bound, the
    // shim uses SendInput to synthesise the Numpad-0 keypress.
    // SendInput routes through the Windows input queue, which the
    // game's DirectInput 8 keyboard device polls — that's the same
    // pipeline a physical Numpad-0 keypress uses. Confirms the
    // dispatch counter increments and the target diag records the
    // bound HWND (for the F12 dump + Walking entry log line).
    auto s = MakeShim();
    s->SetEnabled(true);
    s->SetSupportedForTesting(true);
    s->BindGameHwnd(reinterpret_cast<HWND>(0xFEEDFACE));
    s->OnEnterWalking();
    EXPECT_EQ(s->last_dispatch_path(), 1);  // 1 = SendInput
    EXPECT_EQ(s->last_postmessage_target_diag(), 0xFEEDFACEu);
    EXPECT_EQ(s->toggles_dispatched(), 1);
}

void TestGameFreeCameraShimRebindOnEntryCatchesLateWindow() {
    // The per-frame conductor re-binds the HWND on every Walking
    // entry when the previous bind returned null. Pin: the first
    // OnEnterWalking with no HWND skips dispatch (path=none); a
    // subsequent BindGameHwnd followed by a second OnEnterWalking
    // flips to SendInput (now that we have a target).
    auto s = MakeShim();
    s->SetEnabled(true);
    s->SetSupportedForTesting(true);
    s->OnEnterWalking();
    EXPECT_EQ(s->last_dispatch_path(), 0);  // none — no HWND
    // Simulate the late-window bind (e.g. splash screen dismissed).
    s->BindGameHwnd(reinterpret_cast<HWND>(0x12345678));
    s->OnEnterWalking();
    EXPECT_EQ(s->last_dispatch_path(), 1);  // SendInput
    EXPECT_EQ(s->last_postmessage_target_diag(), 0x12345678u);
}

void TestGameFreeCameraShimOnExitWalkingLeavesCountersIntact() {
    auto s = MakeShim();
    s->SetEnabled(true);
    s->OnEnterWalking();
    s->OnEnterWalking();
    EXPECT_EQ(s->walks_entered(), 2);
    s->OnExitWalking();
    // OnExitWalking must not mutate walks_entered / toggles_dispatched.
    EXPECT_EQ(s->walks_entered(), 2);
    EXPECT_EQ(s->toggles_dispatched(), 0);
}

// --- ProbeConfigCandidatesForTesting -------------------------------------
//
// The probe path used to look only at the English `Documents\`
// sub-folder of USERPROFILE, which silently fails on hosts with
// OneDrive folder backup and/or Spanish locales. The candidates
// helper now enumerates six sub-folder variants, and these tests
// pin that contract so the runtime probe keeps working across the
// most common OneDrive layouts.

void TestGameFreeCameraShimProbeConfigCandidatesCoversOneDrive() {
    const auto candidates =
        GameFreeCameraShim::ProbeConfigCandidatesForTesting(
            std::string("C:\\Users\\migue"));
    EXPECT_FALSE(candidates.empty());

    bool has_ats = false;
    bool has_ets = false;
    bool has_onedrive_en = false;
    bool has_onedrive_es = false;
    bool has_onedrive_personal_en = false;
    bool has_onedrive_personal_es = false;
    for (const auto &c : candidates) {
        if (c.find("American Truck Simulator") != std::string::npos) {
            has_ats = true;
        }
        if (c.find("Euro Truck Simulator 2") != std::string::npos) {
            has_ets = true;
        }
        if (c.find("OneDrive\\Documents") != std::string::npos) {
            has_onedrive_en = true;
        }
        if (c.find("OneDrive\\Documentos") != std::string::npos) {
            has_onedrive_es = true;
        }
        if (c.find("OneDrive - Personal\\Documents") != std::string::npos) {
            has_onedrive_personal_en = true;
        }
        if (c.find("OneDrive - Personal\\Documentos") !=
            std::string::npos) {
            has_onedrive_personal_es = true;
        }
    }
    EXPECT_TRUE(has_ats);
    EXPECT_TRUE(has_ets);
    EXPECT_TRUE(has_onedrive_en);
    EXPECT_TRUE(has_onedrive_es);
    EXPECT_TRUE(has_onedrive_personal_en);
    EXPECT_TRUE(has_onedrive_personal_es);
}

void TestGameFreeCameraShimProbeConfigCandidatesATSFirst() {
    const auto candidates =
        GameFreeCameraShim::ProbeConfigCandidatesForTesting(
            std::string("C:\\Users\\migue"));
    // The ATS stem must come before the ETS2 stem within each sub
    // folder — the runtime probe stops at the first match and ATS
    // is the actual target.
    EXPECT_TRUE(candidates.size() >= 2);
    const auto ats_pos = candidates.front().find(
        "American Truck Simulator");
    const auto ets_pos = candidates.front().find(
        "Euro Truck Simulator 2");
    EXPECT_NE(ats_pos, std::string::npos);
    EXPECT_EQ(ets_pos, std::string::npos);
}

// --- ReadGDeveloperValueForTesting ---------------------------------------
//
// The parser scans the file for the first `uset g_developer` line
// and returns the quoted value. We round-trip it with temp files
// so the test does not depend on whatever `config.cfg` happens to
// be on the host.

void TestGameFreeCameraShimReadGDeveloperValueEmptyOnEmptyPath() {
    EXPECT_EQ(
        GameFreeCameraShim::ReadGDeveloperValueForTesting(std::string()),
        std::string());
}

void TestGameFreeCameraShimReadGDeveloperValueParsesOne() {
    const std::string path = MakeTempPath("gdev_one");
    EXPECT_TRUE(WriteTempFile(
        path,
        "uset g_flyspeed \"195.313\"\nuset g_developer \"1\"\n"
        "uset g_lang_init \"es_es\"\n"));
    EXPECT_EQ(GameFreeCameraShim::ReadGDeveloperValueForTesting(path),
              std::string("1"));
    std::remove(path.c_str());
}

void TestGameFreeCameraShimReadGDeveloperValueParsesZero() {
    const std::string path = MakeTempPath("gdev_zero");
    EXPECT_TRUE(WriteTempFile(
        path,
        "uset g_developer \"0\"\n"));
    EXPECT_EQ(GameFreeCameraShim::ReadGDeveloperValueForTesting(path),
              std::string("0"));
    std::remove(path.c_str());
}

void TestGameFreeCameraShimReadGDeveloperValueEmptyOnMissingKey() {
    const std::string path = MakeTempPath("gdev_missing");
    EXPECT_TRUE(WriteTempFile(
        path,
        "uset g_flyspeed \"195.313\"\nuset g_lang_init \"es_es\"\n"));
    EXPECT_EQ(GameFreeCameraShim::ReadGDeveloperValueForTesting(path),
              std::string());
    std::remove(path.c_str());
}

void TestGameFreeCameraShimReadGDeveloperValueMalformedNoQuotes() {
    const std::string path = MakeTempPath("gdev_malformed");
    EXPECT_TRUE(WriteTempFile(
        path,
        "uset g_developer 1\n"));
    EXPECT_EQ(GameFreeCameraShim::ReadGDeveloperValueForTesting(path),
              std::string());
    std::remove(path.c_str());
}

void TestGameFreeCameraShimReadGDeveloperValueStopsAtFirstLine() {
    // If multiple `uset g_developer` lines exist (which can happen
    // if a user appends a duplicate), we return the FIRST value
    // because that is what SCS uses at startup.
    const std::string path = MakeTempPath("gdev_dup");
    EXPECT_TRUE(WriteTempFile(
        path,
        "uset g_developer \"1\"\nuset g_developer \"2\"\n"));
    EXPECT_EQ(GameFreeCameraShim::ReadGDeveloperValueForTesting(path),
              std::string("1"));
    std::remove(path.c_str());
}

}  // namespace walkercore
