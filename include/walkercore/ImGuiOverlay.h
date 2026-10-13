// WalkerCore — ImGui overlay (M11 lander).
//
// Why this exists:
//   The M11 lander is a status overlay rendered on top of the
//   game's swap chain. It uses Dear ImGui v1.91 (vendored under
//   `third_party/imgui/`) and the Win32 + D3D11 backends. The
//   overlay is INI-gated under `[m11] imgui_overlay = true` (off
//   by default — most users just want the walker, not a debug
//   HUD on top of their game).
//
// Lifecycle (this commit wires the real D3D11 path; the prior
// commit shipped the ABI surface + scaffolding):
//
//   1. PluginEntry constructs the ImGuiOverlay at
//      scs_telemetry_init time and mirrors it to the diagnostic
//      layer so walker_dump_imgui_overlay() works pre-D3D11.
//   2. D3D11Hook's HookedCreateDeviceAndSwapChain captures the
//      ID3D11Device + IDXGISwapChain + ID3D11DeviceContext, then
//      calls walkercore::OnD3D11DeviceCaptured(device, context,
//      hwnd). The overlay runs ImGui::CreateContext +
//      ImGui_ImplWin32_Init(hwnd) + ImGui_ImplDX11_Init(device,
//      context) once and flips supported_ to true.
//   3. D3D11Hook's HookedPresent calls
//      walkercore::RenderImGuiFrameIfEnabled() after
//      walker_try_enter_per_frame_tick() returns. The overlay
//      drives a NewFrame + draws a small status window + Render +
//      ImGui_ImplDX11_RenderDrawData. Per-frame counters
//      (frames_rendered_) increment.
//   4. D3D11Hook's HookedResizeBuffers does NOT call the overlay
//      on every resize — ImGui_ImplDX11_RenderDrawData re-binds
//      the render target view each frame so resizes are handled
//      transparently. The hook only fires OnDeviceLost when the
//      device pointer itself changes (a different D3D11CreateDevice
//      call from the game).
//   5. D3D11Hook::Uninstall + PluginEntry::scs_telemetry_shutdown
//      call walkercore::ShutdownImGuiOverlay() to release the
//      ImGui context + the D3D11 + Win32 backends.
//
// ABI (all add to the M11 surface; total contract now 21 exports):
//   walker_imgui_overlay_set_enabled(int)
//   walker_imgui_overlay_supported()
//   walker_imgui_overlay_frames_rendered()
//   walker_dump_imgui_overlay()
//   walker_imgui_overlay_version()
//   walker_shutdown_imgui_overlay()
//   walker_on_d3d11_device_captured(void* device, void* context, void* hwnd)
//   walker_on_d3d11_device_lost()
//   walker_render_imgui_frame_if_enabled()

#ifndef WALKERCORE_IMGUI_OVERLAY_H
#define WALKERCORE_IMGUI_OVERLAY_H

#include <cstdint>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace walkercore {

class ImGuiOverlay {
public:
    ImGuiOverlay();
    ~ImGuiOverlay();

    ImGuiOverlay(const ImGuiOverlay &) = delete;
    ImGuiOverlay &operator=(const ImGuiOverlay &) = delete;

    // INI gate. False by default — opt in.
    void SetEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    // True when ImGui context is initialised AND the D3D11 device
    // has been captured by the D3D11 hook. The overlay needs the
    // device to create its render target view.
    bool isSupported() const { return supported_; }

    // Per-frame counter. Increments on every RenderFrameIfEnabled()
    // call where the context is alive, the device is alive, and
    // the overlay is enabled. Used by walker_dump_imgui_overlay to
    // confirm "is the render path firing?".
    int frames_rendered() const { return frames_rendered_; }

    // ImGui version string the build was compiled against. Returns
    // "1.91.5" when ImGui is vendored, "0.0.0" when not. Always
    // non-NULL so the dump can log it unconditionally.
    static const char *ImGuiVersionString();

    // Pull a one-time snapshot of "is the user currently in walk
    // mode?" — used by the render path to pick which status
    // string to draw. Mirrors GameFreeCameraShim's contract.
    void OnEnterWalking() { walks_entered_++; }
    void OnExitWalking() { walks_exited_++; }
    int walks_entered() const { return walks_entered_; }
    int walks_exited() const { return walks_exited_; }

    // -- D3D11 device lifecycle --------------------------------------------
    // The D3D11 hook calls these. All three are no-ops when the
    // WALKERCORE_HAVE_IMGUI build flag is off (the test build without
    // ImGui sources, or a vendoring gap in the field).
    //
    //   InitializeForD3D11Device creates the ImGui context + D3D11 +
    //   Win32 backends. hwnd may be nullptr (e.g. headless tests) —
    //   the function still creates the ImGui context but marks the
    //   overlay as supported=false so the render path stays off.
    //   Returns true when the full init succeeded.
    bool InitializeForD3D11Device(void *device, void *context, void *hwnd);
    // Release GPU-side resources only (the font texture + vertex /
    // index buffers). Call this before the game destroys the device
    // (rare — most games reuse the same device across resizes).
    void OnDeviceLost();
    // Restore GPU-side resources after a new device has been
    // captured. Init has to be called first.
    void OnDeviceCreated();
    // Render one frame. Returns true when the call actually
    // produced output (ImGui::NewFrame → draw → Render →
    // ImGui_ImplDX11_RenderDrawData). When the overlay is disabled
    // or the device is not captured, returns false without
    // touching the render target. The render pulls the live
    // walker state from the bound FrameLoop + WalkerFsm pointers
    // (see BindFrameLoop / BindFsm) so a deploy-side analyst can
    // confirm "yes, F10 actually toggled Walking" by reading the
    // overlay on screen.
    bool RenderFrameIfEnabled();
    // Tear down the ImGui context + the D3D11 + Win32 backends.
    // Safe to call from DllMain / scs_telemetry_shutdown.
    void Shutdown();

    // -- Walker state binding ----------------------------------------------
    // Wire the live FrameLoop + WalkerFsm so the render can surface
    // the current camera eye, player feet, and FSM state. PluginEntry
    // calls both at scs_telemetry_init time (after the FrameLoop and
    // WalkerFsm are constructed). Pointers are borrowed — the overlay
    // does not own them and the diagnostics mirror in DiagnosticDump
    // is the canonical source. The render skips the corresponding
    // section when the bound pointer is null.
    void BindFrameLoop(class FrameLoop *loop) { frame_loop_ = loop; }
    void BindFsm(class WalkerFsm *fsm) { fsm_ = fsm; }
    // M11: bind the in-game settings menu so the overlay's render
    // path calls SettingsMenu::Render() once per frame. The menu
    // owns its widget layout (sliders, bools, key bindings); the
    // overlay is just the place where the draw call lands inside
    // the active ImGui::NewFrame/EndFrame block. A null binding is
    // a no-op (the overlay's status window is unaffected).
    void BindSettingsMenu(class SettingsMenu *m) { settings_menu_ = m; }
    bool frame_loop_bound() const { return frame_loop_ != nullptr; }
    bool fsm_bound() const { return fsm_ != nullptr; }
    bool settings_menu_bound() const { return settings_menu_ != nullptr; }

    // -- Test seams --------------------------------------------------------
    // Pure helpers exposed for the unit-test suite. Not part of
    // the runtime ABI; clients should never call them.
    static void ResetForTesting();
    static int TestRenderCount();

    // M11.5 — counter incremented every time the user clicks an
    // interactive widget in the overlay (currently: the "Dump state
    // now" or "Dump PxScene" buttons). The render path exposes the
    // current value in the overlay so a deploy-side analyst can
    // confirm "yes, the WndProc subclass is routing click events
    // to ImGui" by clicking the button and watching the counter
    // bump in the same frame. The counter is per-DLL-load (reset
    // by Shutdown via the ResetForTesting path).
    int button_clicks() const { return button_clicks_; }

private:
    bool enabled_ = false;
    bool supported_ = false;
    bool context_initialised_ = false;
    bool dx11_initialised_ = false;
    bool win32_initialised_ = false;
    int walks_entered_ = 0;
    int walks_exited_ = 0;
    int frames_rendered_ = 0;
    int button_clicks_ = 0;
    std::string init_failure_;

    // Bump on Shutdown so the render path can detect "context was
    // released, stop trying to draw" without dereferencing a stale
    // pointer. The D3D11 hook is not allowed to call into us after
    // the device is gone; the counter is informational only.
    int init_generation_ = 0;

    // Borrowed pointers to the walker pipeline. The render reads
    // camera + player + FSM state from these to populate the
    // overlay's live state section. Not owned; the lifetime is
    // managed by PluginEntry.
    class FrameLoop *frame_loop_ = nullptr;
    class WalkerFsm *fsm_ = nullptr;
    // M11: bound settings menu (sibling borrow, not owned).
    // SettingsMenu::Render() runs inside the overlay's ImGui frame
    // each tick when the menu is open and this pointer is non-null.
    class SettingsMenu *settings_menu_ = nullptr;
};

}  // namespace walkercore

// Free-function C API used by D3D11Hook.cpp. Names match the
// `walker_*` ABI surface in walkercore/PerFrame.h. Lives in the
// walkercore namespace so the linker doesn't see C++ mangling
// collisions; the extern "C" wrappers are in src/adapter/DiagnosticDump.cpp.
namespace walkercore {

// Called from D3D11Hook::HookedCreateDeviceAndSwapChain after a
// successful device + swap chain + context capture. hwnd is the
// game's foreground window (found via EnumWindows in D3D11Hook).
void OnD3D11DeviceCaptured(void *device, void *context, void *hwnd);

// Called from D3D11Hook::HookedCreateDeviceAndSwapChain when a
// SECOND device is created (the game replaced its D3D11 device —
// rare; only the Lost event here, the new init comes through
// OnD3D11DeviceCaptured with the new device).
void OnD3D11DeviceLost();

// Called from D3D11Hook::HookedPresent AFTER the per-frame tick.
// The function is a no-op when the overlay is disabled, the D3D11
// device has not been captured, or WALKERCORE_HAVE_IMGUI=0.
void RenderImGuiFrameIfEnabled();

// Called from D3D11Hook::Uninstall + PluginEntry::scs_telemetry_shutdown.
// Safe to call multiple times.
void ShutdownImGuiOverlay();

}  // namespace walkercore

#endif  // WALKERCORE_IMGUI_OVERLAY_H
