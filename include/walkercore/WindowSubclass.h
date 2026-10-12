// WalkerCore — game-window WndProc subclass (M11.4).
//
// Why this exists:
//   The M4 mouse-pump (InputHook::OnRawInputData / OnRawInputMessage)
//   needs WM_INPUT to actually arrive at the game window — without a
//   WndProc subclass the messages go straight to the game's default
//   handler and the mouse delta accumulator never moves. The M11
//   ImGui overlay has the same problem: ImGui_ImplWin32_WndProcHandler
//   must be in the WndProc chain for the overlay to receive input.
//
//   The WndProc subclass is a single shim that:
//     1. Routes WM_INPUT to a registered sink (the InputHook's
//        OnRawInputMessage trampoline, so the data plane pumps).
//     2. Forwards every input message to ImGui_ImplWin32_WndProcHandler
//        (no-op when no ImGui context is alive — always safe to call).
//     3. Chains to the original WndProc via CallWindowProcA so the
//        game continues to see all input normally.
//
// Lifecycle:
//   - ImGuiOverlay::InitializeForD3D11Device calls Install(hwnd) AFTER
//     ImGui's D3D11 + Win32 backends are initialised (ImGui's
//     WndProcHandler needs the context). On a device-lost event,
//     ImGuiOverlay::OnDeviceLost calls Uninstall(hwnd) so the chain
//     returns to the game's pre-WalkerCore state. On DLL unload,
//     ImGuiOverlay::Shutdown does the same.
//   - InputHook::Install registers the raw-input sink via
//     SetRawInputSink(fn, ctx) so the WndProc subclass (once
//     installed by ImGuiOverlay) pumps WM_INPUT into the data plane.
//     InputHook::Uninstall clears the sink with (nullptr, nullptr).
//
// Threading:
//   The WndProc is called on the thread that owns the window (the
//   game thread). Install / Uninstall / SetRawInputSink are also
//   called from the game thread (D3D11 hook fires on the game thread
//   and ImGuiOverlay + InputHook are owned by the same PluginEntry
//   single-threaded pipeline). No synchronisation is required.
//
// This module is internal to the DLL — there is no extern "C" ABI
// surface for WindowSubclass. The ImGuiOverlay and InputHook are the
// only callers, and both are in the same translation set.

#ifndef WALKERCORE_WINDOW_SUBCLASS_H
#define WALKERCORE_WINDOW_SUBCLASS_H

#ifdef _WIN32
#include <windows.h>
#endif

namespace walkercore {

// Sink function called for every WM_INPUT message the WndProc
// subclass receives. `ctx` is the value passed at SetRawInputSink
// registration time. `lParam` is the OS-delivered HRAWINPUT cast to
// LPARAM (the canonical WM_INPUT contract). The function should be
// tolerant of malformed HRAWINPUTs — the trampoline is fire-and-
// forget and the WndProc always chains to the original.
using RawInputSinkFn = void (*)(void *ctx, LPARAM lParam);

// Install a WndProc subclass on `hwnd`. Idempotent: a second call on
// the same hwnd is a no-op and returns false. The original WndProc
// is stashed in module-local state so Uninstall can restore it via
// SetWindowLongPtrA(GWL_WNDPROC, original). Returns true on the
// first install, false on subsequent calls or on install failure
// (e.g. hwnd is null, GetWindowLongPtrA returns 0, the subclass
// already belongs to a foreign module).
bool WindowSubclass_Install(HWND hwnd);

// Restore the original WndProc and clear the subclass bookkeeping.
// Safe to call when not installed (no-op). Safe to call from
// scs_telemetry_shutdown / DllMain / ImGuiOverlay::Shutdown.
void WindowSubclass_Uninstall(HWND hwnd);

// Returns true when the WndProc subclass is currently installed on
// `hwnd`. Used by tests and by the ImGuiOverlay to confirm "did the
// install succeed?" without re-installing.
bool WindowSubclass_IsInstalled(HWND hwnd);

// Register (or clear with (nullptr, nullptr)) the sink function
// called from the WndProc subclass on every WM_INPUT message. The
// function is called BEFORE the WndProc chains to the original
// (so the data plane pumps even if the original swallows the
// message). Pass (nullptr, nullptr) to clear.
void WindowSubclass_SetRawInputSink(RawInputSinkFn fn, void *ctx);

}  // namespace walkercore

#endif  // WALKERCORE_WINDOW_SUBCLASS_H
