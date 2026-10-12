// WalkerCore — game-window WndProc subclass implementation (M11.4).
//
// Single-hwnd state machine. The game is expected to have exactly one
// top-level window; if a future build needs to subclass multiple
// hwnds the static state would move to a small map. The walker
// itself never creates more than one window (the WndProc subclass
// lives on the game's window), so the single-hwnd assumption holds
// for the M2 / M11 landers in scope.

#include "walkercore/WindowSubclass.h"

#include "walkercore/Log.h"

#if defined(WALKERCORE_HAVE_IMGUI)
// Forward-declare the ImGui Win32 backend's WndProc handler. The
// declaration in imgui_impl_win32.h is hidden inside `#if 0` (the
// ImGui contract is "you define the WndProc call yourself" so the
// handler can reach your `this` if needed). We are not in a class,
// so the simpler `extern IMGUI_IMPL_API` form here is the canonical
// forward declaration. Calling the function when no ImGui context
// is alive is a documented no-op (the function checks the context
// pointer at the top).
#include "imgui.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                              UINT msg,
                                                              WPARAM wParam,
                                                              LPARAM lParam);
#endif

namespace walkercore {

namespace {

// Single-hwnd state. hwnd is non-null iff a subclass is currently
// installed. The original WndProc lives alongside so Uninstall can
// restore it via SetWindowLongPtrA(GWL_WNDPROC, original).
//
// raw_input_sink_fn + raw_input_sink_ctx pair is the registered
// WM_INPUT handler. Set via SetRawInputSink. The WndProc reads
// these without locking — the walker is single-threaded for input
// (all of the game's keyboard / mouse events arrive on the game
// thread, the WndProc subclass is installed and uninstalled on the
// game thread, and the sink itself runs on the game thread).
struct SubclassState {
    HWND hwnd = nullptr;
    WNDPROC original = nullptr;
    RawInputSinkFn raw_input_sink_fn = nullptr;
    void *raw_input_sink_ctx = nullptr;
};
SubclassState g_state;

// The WndProc subclass. Subclassed via SetWindowLongPtrA +
// GWL_WNDPROC, so the function signature must be LRESULT CALLBACK
// with the standard (HWND, UINT, WPARAM, LPARAM) layout.
//
// Routing order:
//   1. WM_INPUT → raw_input_sink_fn (if registered). Done first so
//      the data plane pumps even if the original WndProc swallows
//      the message (some games suppress WM_INPUT when an internal
//      camera is active — e.g. the rear-view mirror in ATS / ETS2
//      captures raw input to redirect the camera).
//   2. Every input message → ImGui_ImplWin32_WndProcHandler (when
//      ImGui is vendored). This is a no-op when no ImGui context
//      is alive, so unconditional call is safe.
//   3. Chain to the original WndProc via CallWindowProcA so the
//      game continues to see every message normally. The WndProc
//      never swallows anything.
LRESULT CALLBACK WalkerSubclassWndProc(HWND hwnd, UINT msg, WPARAM w,
                                       LPARAM l) {
    if (msg == WM_INPUT && g_state.raw_input_sink_fn != nullptr) {
        g_state.raw_input_sink_fn(g_state.raw_input_sink_ctx, l);
    }
#if defined(WALKERCORE_HAVE_IMGUI)
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l);
#endif
    if (g_state.original == nullptr) {
        // Defensive — should never fire. The subclass state
        // guarantees g_state.original is set when the subclass
        // is installed, and Uninstall clears hwnd before this
        // WndProc can run again. If it does fire (e.g. someone
        // called SetWindowLongPtrA on the game hwnd from outside
        // the walker after our subclass was installed), fall
        // through to DefWindowProcA rather than crash.
        return DefWindowProcA(hwnd, msg, w, l);
    }
    return CallWindowProcA(g_state.original, hwnd, msg, w, l);
}

}  // namespace

bool WindowSubclass_Install(HWND hwnd) {
    if (hwnd == nullptr) {
        WALKERCORE_LOG_WARN(
            "WindowSubclass_Install: refused (hwnd is null)");
        return false;
    }
    if (g_state.hwnd == hwnd) {
        // Already subclassed this exact hwnd. Idempotent.
        return false;
    }
    if (g_state.hwnd != nullptr) {
        // Different hwnd — the game replaced its top-level window
        // (rare; can happen across fullscreen / windowed mode
        // transitions). Tear down the old subclass and install on
        // the new one. The original WndProc on the old hwnd is
        // restored before we touch the new one.
        WALKERCORE_LOG_INFO(
            "WindowSubclass_Install: replacing old hwnd=%p with new "
            "hwnd=%p (window reset across fullscreen toggle?)",
            g_state.hwnd, hwnd);
        WindowSubclass_Uninstall(g_state.hwnd);
    }

    // SetWindowLongPtrA with GWLP_WNDPROC replaces the WndProc. The
    // returned value is the PREVIOUS WndProc — the one we must
    // chain to in our subclass. We restore it in Uninstall. The
    // MinGW <windows.h> exposes only the pointer-width name
    // (GWLP_WNDPROC); the legacy 32-bit-only GWL_WNDPROC isn't
    // declared on x64 toolchains and would fail to compile.
    const LONG_PTR prev = SetWindowLongPtrA(
        hwnd, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&WalkerSubclassWndProc));
    if (prev == 0) {
        const DWORD err = GetLastError();
        // ERROR_SUCCESS is 0 — that's the documented "no previous
        // WndProc" case (e.g. a fresh window with no subclass ever
        // applied), and it's a legitimate install. ERROR_INVALID_
        // WINDOW_HANDLE / ERROR_ACCESS_DENIED are real failures.
        if (err != ERROR_SUCCESS) {
            WALKERCORE_LOG_ERROR(
                "WindowSubclass_Install: SetWindowLongPtrA failed: "
                "error %lu",
                err);
            return false;
        }
    }
    g_state.hwnd = hwnd;
    g_state.original = reinterpret_cast<WNDPROC>(prev);
    WALKERCORE_LOG_INFO(
        "WindowSubclass_Install: subclassed hwnd=%p (original=%p)",
        hwnd, reinterpret_cast<void *>(g_state.original));
    return true;
}

void WindowSubclass_Uninstall(HWND hwnd) {
    if (g_state.hwnd == nullptr) {
        // Never installed. Idempotent.
        return;
    }
    if (hwnd != nullptr && g_state.hwnd != hwnd) {
        // Uninstall called with a different hwnd than the one we
        // subclassed. Don't touch the other window — just clear
        // bookkeeping if the caller asked for "all".
        if (hwnd != reinterpret_cast<HWND>(-1)) {
            WALKERCORE_LOG_WARN(
                "WindowSubclass_Uninstall: hwnd mismatch "
                "(installed=%p, asked=%p) — ignoring",
                g_state.hwnd, hwnd);
            return;
        }
    }
    // Restore the original WndProc on the subclassed window. The
    // return value is the PREVIOUS WndProc (i.e. our subclass) —
    // we don't need it, the OS discards the reference once the new
    // WndProc is in place.
    SetWindowLongPtrA(g_state.hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_state.original));
    WALKERCORE_LOG_INFO(
        "WindowSubclass_Uninstall: restored hwnd=%p original=%p",
        g_state.hwnd, reinterpret_cast<void *>(g_state.original));
    g_state.hwnd = nullptr;
    g_state.original = nullptr;
    // The sink is intentionally NOT cleared here — the InputHook
    // uninstall path handles sink clearing on its own (calling
    // SetRawInputSink(nullptr, nullptr) explicitly). Clearing it
    // here would break the case where a re-install happens with
    // the same sink already registered.
}

bool WindowSubclass_IsInstalled(HWND hwnd) {
    if (hwnd == nullptr) {
        return g_state.hwnd != nullptr;
    }
    return g_state.hwnd == hwnd;
}

void WindowSubclass_SetRawInputSink(RawInputSinkFn fn, void *ctx) {
    g_state.raw_input_sink_fn = fn;
    g_state.raw_input_sink_ctx = ctx;
    if (fn == nullptr) {
        WALKERCORE_LOG_INFO(
            "WindowSubclass_SetRawInputSink: cleared");
    } else {
        WALKERCORE_LOG_INFO(
            "WindowSubclass_SetRawInputSink: registered fn=%p ctx=%p",
            reinterpret_cast<void *>(fn), ctx);
    }
}

}  // namespace walkercore
