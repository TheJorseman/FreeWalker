// WalkerCore — WindowSubclass tests (M11.4 lander coverage).
//
// The WndProc subclass is normally driven by the game's window message
// loop. In the test process there is no game, so we synthesise a
// message-only window (CreateWindowExA with HWND_MESSAGE) and route
// synthetic SendMessage calls through the subclass. The test pins:
//
//   - Install / Uninstall round-trip on a real hwnd.
//   - IsInstalled reflects the live state.
//   - Second Install on the same hwnd is a no-op (idempotent at the
//     state-machine level).
//   - Install with null hwnd is refused (returns false, no state
//     change).
//   - Uninstall when never installed is safe (idempotent).
//   - The registered raw-input sink receives every WM_INPUT message
//     with the (ctx, lParam) we set.
//   - Non-WM_INPUT messages still reach the original WndProc (we
//     install a counter-incrementing WndProc on the test window and
//     assert the counter bumps after a SendMessage(WM_PAINT) through
//     the subclass).
//   - SetRawInputSink with (nullptr, nullptr) clears the sink so a
//     later WM_INPUT is dropped (no-op).
//   - Re-installing the subclass restores the prior WM_INPUT routing
//     (sink survives across the second install).
//
// The test does not exercise the ImGui_ImplWin32_WndProcHandler
// forward — that vendor function needs an ImGui context to do
// anything useful, and the WALKERCORE_HAVE_IMGUI test build links
// ImGui but doesn't initialise a context in this fixture. The
// forward is exercised by integration (the live game window's WndProc
// chain) and the contract is "ImGui_ImplWin32_WndProcHandler is
// called for every message; the function is a no-op without a
// context" — that contract is owned by the ImGui vendor, not by the
// walker.

#include "TestHarness.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "walkercore/WindowSubclass.h"

namespace walkercore {

namespace {

// Test-scope WndProc for the message-only window. Counts every
// message that lands here (the subclass chains to this for non-
// WM_INPUT messages). The lpcs / wpcs pointers are the test's
// assertion anchor — the fixture creates the window with a
// unique class name and the WndProc pulls the counter out of
// the window's GWLP_USERDATA slot.
LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) {
        // The CreateWindowExA call passes a CreateStruct* as lParam.
        // Stash the counter in the user data slot so the WndProc can
        // find it on every subsequent message.
        const auto *cs = reinterpret_cast<CREATESTRUCTA *>(l);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcA(hwnd, msg, w, l);
    }
    auto *counter = reinterpret_cast<std::atomic<int> *>(
        GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (counter != nullptr) {
        counter->fetch_add(1, std::memory_order_relaxed);
    }
    return DefWindowProcA(hwnd, msg, w, l);
}

// Test-scope sink for WM_INPUT. Receives (ctx, lParam) where ctx is
// the sink's own counter (set via SetRawInputSink). The function
// captures the (sink, ctx) pair via raw input mock — we just bump
// the counter and stash the lParam in a captured value for the
// assertion.
struct SinkRecord {
    int call_count = 0;
    LPARAM last_lparam = 0;
};
void TestRawInputSink(void *ctx, LPARAM l_param) {
    if (ctx == nullptr) return;
    auto *rec = static_cast<SinkRecord *>(ctx);
    rec->call_count++;
    rec->last_lparam = l_param;
}

// Create a fresh message-only window with a unique class name. Each
// test gets a distinct class so registering the class doesn't leak
// state across tests. The returned hwnd has a TestWndProc that
// bumps `*counter_out` on every message.
HWND CreateTestWindow(const std::string &class_name,
                      std::atomic<int> *counter_out) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TestWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = class_name.c_str();
    if (RegisterClassExA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    return CreateWindowExA(
        0, class_name.c_str(), "WalkerCoreTest",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleA(nullptr),
        counter_out);
}

}  // namespace

// ----- Install / Uninstall / IsInstalled state machine -----

void TestWindowSubclassFreshStateIsNotInstalled() {
    // A fresh module-local state must report IsInstalled(false) before
    // any Install. Each test that installs the subclass also calls
    // Uninstall in TearDown — this probe just pins the initial state.
    EXPECT_FALSE(WindowSubclass_IsInstalled(nullptr));
    EXPECT_FALSE(WindowSubclass_IsInstalled(reinterpret_cast<HWND>(0x12345)));
}

void TestWindowSubclassInstallOnNullHwndIsRefused() {
    // Install(nullptr) is a programmer error and must return false
    // without mutating state. Refusing keeps the WndProc chain from
    // pointing at a bogus hwnd (e.g. on a headless test process
    // where FindGameWindowHandle returns null).
    const bool ok = WindowSubclass_Install(nullptr);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(WindowSubclass_IsInstalled(nullptr));
}

void TestWindowSubclassInstallUninstallRoundTrip() {
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_InstallUninstall", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    const bool installed = WindowSubclass_Install(hwnd);
    EXPECT_TRUE(installed);
    EXPECT_TRUE(WindowSubclass_IsInstalled(hwnd));
    EXPECT_TRUE(WindowSubclass_IsInstalled(nullptr));  // "any hwnd"

    WindowSubclass_Uninstall(hwnd);
    EXPECT_FALSE(WindowSubclass_IsInstalled(hwnd));
    EXPECT_FALSE(WindowSubclass_IsInstalled(nullptr));

    DestroyWindow(hwnd);
}

void TestWindowSubclassInstallIsIdempotent() {
    // Second Install on the same hwnd is a no-op returning false
    // (the state machine recognises it's already subclassed and
    // refuses to overwrite the original WndProc reference).
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_InstallIdempotent", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    EXPECT_TRUE(WindowSubclass_Install(hwnd));
    EXPECT_FALSE(WindowSubclass_Install(hwnd));
    EXPECT_TRUE(WindowSubclass_IsInstalled(hwnd));

    WindowSubclass_Uninstall(hwnd);
    DestroyWindow(hwnd);
}

void TestWindowSubclassUninstallWhenNotInstalledIsSafe() {
    // Uninstall with no live subclass is a no-op (the state machine
    // guards on hwnd == nullptr). Two consecutive calls must not
    // crash and must not report IsInstalled after.
    WindowSubclass_Uninstall(nullptr);
    EXPECT_FALSE(WindowSubclass_IsInstalled(nullptr));
    WindowSubclass_Uninstall(reinterpret_cast<HWND>(0x12345));
    EXPECT_FALSE(WindowSubclass_IsInstalled(nullptr));
}

void TestWindowSubclassUninstallWithMismatchedHwndIsIgnored() {
    // Uninstall(some_other_hwnd) while the live subclass is on
    // hwnd_1 must not touch hwnd_1. The hwnd mismatch is a
    // programmer error and the function logs + returns without
    // mutating the state. (Sent with hwnd != sentinel.)
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_UninstallMismatch", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    EXPECT_TRUE(WindowSubclass_Install(hwnd));
    const HWND other = reinterpret_cast<HWND>(0xDEADBEEF);
    WindowSubclass_Uninstall(other);
    EXPECT_TRUE(WindowSubclass_IsInstalled(hwnd));

    WindowSubclass_Uninstall(hwnd);
    DestroyWindow(hwnd);
}

void TestWindowSubclassReinstallReplacesHwnd() {
    // The state machine recognises a hwnd swap (fullscreen / windowed
    // transition) and tears down the old subclass before installing
    // on the new one. After the swap, IsInstalled(new) is true and
    // the old hwnd's WndProc has been restored to its pre-WalkerCore
    // state (we can't directly verify the WndProc was restored in
    // a unit test, but the install succeeded and the new subclass
    // routes messages to the new hwnd).
    std::atomic<int> counter_a{0};
    std::atomic<int> counter_b{0};
    HWND hwnd_a = CreateTestWindow("WCR_ReinstallA", &counter_a);
    HWND hwnd_b = CreateTestWindow("WCR_ReinstallB", &counter_b);
    if (hwnd_a == nullptr || hwnd_b == nullptr) {
        if (hwnd_a != nullptr) DestroyWindow(hwnd_a);
        if (hwnd_b != nullptr) DestroyWindow(hwnd_b);
        return;
    }

    EXPECT_TRUE(WindowSubclass_Install(hwnd_a));
    EXPECT_TRUE(WindowSubclass_Install(hwnd_b));
    EXPECT_TRUE(WindowSubclass_IsInstalled(hwnd_b));
    EXPECT_FALSE(WindowSubclass_IsInstalled(hwnd_a));

    WindowSubclass_Uninstall(hwnd_b);
    DestroyWindow(hwnd_a);
    DestroyWindow(hwnd_b);
}

// ----- WM_INPUT routing -----

void TestWindowSubclassRawInputSinkReceivesWM_INPUT() {
    // The registered sink receives (ctx, lParam) for every WM_INPUT
    // the subclass sees. The lParam we pass is a non-zero sentinel
    // (a real HRAWINPUT would point to kernel memory; we just need
    // a value the test can recognise on the receiving end).
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_RawInputSink", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    SinkRecord rec;
    WindowSubclass_SetRawInputSink(&TestRawInputSink, &rec);
    EXPECT_TRUE(WindowSubclass_Install(hwnd));

    constexpr LPARAM kFakeRawInput = 0x12345678;
    LRESULT result = SendMessageA(hwnd, WM_INPUT, 0, kFakeRawInput);
    (void)result;  // not asserted — the original WndProc is DefWindowProcA

    EXPECT_EQ(rec.call_count, 1);
    EXPECT_EQ(rec.last_lparam, kFakeRawInput);

    WindowSubclass_SetRawInputSink(nullptr, nullptr);
    WindowSubclass_Uninstall(hwnd);
    DestroyWindow(hwnd);
}

void TestWindowSubclassRawInputSinkReceivesMultipleWM_INPUT() {
    // Multiple WM_INPUTs in sequence all route to the sink. The
    // subclass doesn't deduplicate or batch — the sink is called
    // for every message verbatim. The InputHook's accumulator
    // handles the batching / drain contract.
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_RawInputMultiple", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    SinkRecord rec;
    WindowSubclass_SetRawInputSink(&TestRawInputSink, &rec);
    EXPECT_TRUE(WindowSubclass_Install(hwnd));

    for (int i = 0; i < 5; ++i) {
        SendMessageA(hwnd, WM_INPUT, 0,
                     static_cast<LPARAM>(0x1000 + i));
    }
    EXPECT_EQ(rec.call_count, 5);
    EXPECT_EQ(rec.last_lparam, 0x1004);

    WindowSubclass_SetRawInputSink(nullptr, nullptr);
    WindowSubclass_Uninstall(hwnd);
    DestroyWindow(hwnd);
}

void TestWindowSubclassClearedSinkDropsWM_INPUT() {
    // SetRawInputSink(nullptr, nullptr) must clear the sink so a
    // subsequent WM_INPUT is not dispatched. The state machine's
    // sink field is a single pointer; the no-op is the natural
    // consequence of fn == nullptr.
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_ClearedSink", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    SinkRecord rec;
    WindowSubclass_SetRawInputSink(&TestRawInputSink, &rec);
    EXPECT_TRUE(WindowSubclass_Install(hwnd));
    SendMessageA(hwnd, WM_INPUT, 0, 0xAAAA);
    EXPECT_EQ(rec.call_count, 1);

    WindowSubclass_SetRawInputSink(nullptr, nullptr);
    SendMessageA(hwnd, WM_INPUT, 0, 0xBBBB);
    EXPECT_EQ(rec.call_count, 1);  // unchanged

    WindowSubclass_Uninstall(hwnd);
    DestroyWindow(hwnd);
}

void TestWindowSubclassSinkSurvivesReinstall() {
    // Uninstalling and re-installing the subclass on the same hwnd
    // must preserve the registered sink. The state machine's sink
    // fields live outside the per-hwnd (hwnd, original) pair — the
    // pair is reset on Uninstall but the sink pointer is preserved
    // for the case where a re-install happens with the same sink
    // already registered (e.g. D3D11 device lost + re-created with
    // the same game hwnd).
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_SinkSurvives", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    SinkRecord rec;
    WindowSubclass_SetRawInputSink(&TestRawInputSink, &rec);
    EXPECT_TRUE(WindowSubclass_Install(hwnd));
    SendMessageA(hwnd, WM_INPUT, 0, 0xCAFE);
    EXPECT_EQ(rec.call_count, 1);

    WindowSubclass_Uninstall(hwnd);
    EXPECT_TRUE(WindowSubclass_Install(hwnd));
    SendMessageA(hwnd, WM_INPUT, 0, 0xBEEF);
    EXPECT_EQ(rec.call_count, 2);
    EXPECT_EQ(rec.last_lparam, 0xBEEF);

    WindowSubclass_SetRawInputSink(nullptr, nullptr);
    WindowSubclass_Uninstall(hwnd);
    DestroyWindow(hwnd);
}

// ----- Chaining to the original WndProc -----

void TestWindowSubclassChainsToOriginalForNonWM_INPUT() {
    // A non-WM_INPUT message (WM_PAINT) must reach the original
    // WndProc. The fixture's TestWndProc bumps a counter on every
    // message; after one SendMessage(WM_PAINT) through the subclass
    // the counter is 1, proving the chain to the original survived
    // the subclass install. (Note: WM_PAINT normally requires a
    // BeginPaint/EndPaint pair, but the WndProc we install ignores
    // that and just bumps the counter — the subclass chain is
    // independent of the message's paint semantics.)
    //
    // The counter is captured AFTER CreateWindowExA returns because
    // window creation itself dispatches a handful of messages
    // (WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE, ...) that pass through
    // the test WndProc and bump the counter. The first SendMessage
    // after install is what we actually want to assert.
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_ChainsToOriginal", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    const int baseline = counter.load();
    EXPECT_TRUE(WindowSubclass_Install(hwnd));
    SendMessageA(hwnd, WM_PAINT, 0, 0);
    EXPECT_EQ(counter.load(), baseline + 1);

    WindowSubclass_Uninstall(hwnd);
    DestroyWindow(hwnd);
}

void TestWindowSubclassChainsToOriginalForMultipleMessages() {
    // Multiple non-WM_INPUT messages all chain. The subclass is a
    // pure forward for non-WM_INPUT traffic — every message reaches
    // the original WndProc exactly once. Baseline is captured AFTER
    // CreateWindowExA so the assertion is on the delta, not the
    // absolute value (window creation dispatches a few setup
    // messages that also bump the counter).
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_ChainMultiple", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    const int baseline = counter.load();
    EXPECT_TRUE(WindowSubclass_Install(hwnd));
    for (int i = 0; i < 10; ++i) {
        SendMessageA(hwnd, WM_PAINT, 0, 0);
    }
    EXPECT_EQ(counter.load(), baseline + 10);

    WindowSubclass_Uninstall(hwnd);
    DestroyWindow(hwnd);
}

void TestWindowSubclassWM_INPUTAlsoChainsToOriginal() {
    // WM_INPUT is forwarded to BOTH the sink AND the original
    // WndProc. The subclass never swallows — even a raw-input
    // message that the sink consumed reaches the original so the
    // game continues to see normal traffic. Baseline captured after
    // CreateWindowExA so the WM_INPUT chain is asserted as a delta.
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_WMInputChains", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    SinkRecord rec;
    WindowSubclass_SetRawInputSink(&TestRawInputSink, &rec);
    const int baseline = counter.load();
    EXPECT_TRUE(WindowSubclass_Install(hwnd));
    SendMessageA(hwnd, WM_INPUT, 0, 0xFEED);
    EXPECT_EQ(rec.call_count, 1);
    EXPECT_EQ(counter.load(), baseline + 1);

    WindowSubclass_SetRawInputSink(nullptr, nullptr);
    WindowSubclass_Uninstall(hwnd);
    DestroyWindow(hwnd);
}

void TestWindowSubclassRestoresOriginalOnUninstall() {
    // After Uninstall, the WndProc is back to the test's original
    // (DefWindowProcA-wrapped TestWndProc). The next SendMessage
    // bumps the counter but the subclass no longer exists, so
    // there's no second chain to the original — the counter
    // increments exactly once per SendMessage. The test relies on
    // a fresh counter (post-DestroyWindow) for the post-uninstall
    // assertion; we reset to 0 explicitly so the absolute-value
    // check is meaningful.
    std::atomic<int> counter{0};
    HWND hwnd = CreateTestWindow("WCR_RestoresOriginal", &counter);
    EXPECT_TRUE(hwnd != nullptr);
    if (hwnd == nullptr) return;

    const int baseline = counter.load();
    EXPECT_TRUE(WindowSubclass_Install(hwnd));
    SendMessageA(hwnd, WM_PAINT, 0, 0);
    EXPECT_EQ(counter.load(), baseline + 1);

    WindowSubclass_Uninstall(hwnd);
    counter.store(0);
    SendMessageA(hwnd, WM_PAINT, 0, 0);
    EXPECT_EQ(counter.load(), 1);  // exactly one chain, not two

    DestroyWindow(hwnd);
}

}  // namespace walkercore
