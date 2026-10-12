// WalkerCore — ImGuiOverlay provider implementation (M11 lander).
//
// This file lands the D3D11 Present-path integration. The
// scaffolding commit shipped:
//
//   - The class API (header in include/walkercore/ImGuiOverlay.h).
//   - The ABI surface (walker_imgui_overlay_* / walker_dump_imgui_overlay).
//   - The INI gate (`[m11] imgui_overlay`).
//   - The vendored Dear ImGui v1.91 sources under `third_party/imgui/`.
//
// This commit closes the D3D11 hook integration:
//
//   - The class gains InitializeForD3D11Device / OnDeviceLost /
//     OnDeviceCreated / RenderFrameIfEnabled / Shutdown lifecycle
//     methods (header docs in ImGuiOverlay.h).
//   - D3D11Hook's HookedCreateDeviceAndSwapChain calls
//     walkercore::OnD3D11DeviceCaptured(device, context, hwnd) on
//     success, after a successful D3D11CreateDeviceAndSwapChain.
//   - D3D11Hook's HookedPresent calls
//     walkercore::RenderImGuiFrameIfEnabled() after
//     walker_try_enter_per_frame_tick() returns. The overlay
//     drives the ImGui NewFrame + draw + Render cycle.
//   - D3D11Hook::Uninstall + PluginEntry's scs_telemetry_shutdown
//     call walkercore::ShutdownImGuiOverlay() to release the ImGui
//     context.
//
// When WALKERCORE_HAVE_IMGUI is not defined (the test build
// without ImGui sources, or a vendoring gap in the field), all
// the actual ImGui calls are no-ops and the class remains a
// pure scaffolding stub — supported_ stays false and the dump
// reports "(disabled by build)" instead of crashing.
//
// M11 contract: ABI 18 -> 21 (3 new exports: frames_rendered,
// version, shutdown, on_d3d11_captured, on_d3d11_lost,
// render_frame_if_enabled).

#include "walkercore/ImGuiOverlay.h"

#include <cstdio>
#include <string>

#include "walkercore/BuildInfo.h"
#include "walkercore/CameraState.h"
#include "walkercore/FrameLoop.h"
#include "walkercore/Log.h"
#include "walkercore/PerFrame.h"
#include "walkercore/PlayerState.h"
#include "walkercore/WalkerFsm.h"
#include "walkercore/WindowSubclass.h"

#if defined(WALKERCORE_HAVE_IMGUI)
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#endif

namespace walkercore {

namespace {

// Process-wide overlay instance. Set by PluginEntry at init time;
// cleared at shutdown. The D3D11 hook reads/writes through this
// pointer. Mirrors the PxSceneHook / GameFreeCameraShim pattern.
ImGuiOverlay *g_overlay = nullptr;

}  // namespace

const char *ImGuiOverlay::ImGuiVersionString() {
#if defined(WALKERCORE_HAVE_IMGUI)
    return IMGUI_VERSION;  // "1.91.5" — defined in imgui.h
#else
    return "0.0.0";
#endif
}

ImGuiOverlay::ImGuiOverlay() {
    g_overlay = this;
}

ImGuiOverlay::~ImGuiOverlay() {
    if (g_overlay == this) {
        // Best-effort teardown so a delete from the adapter
        // (e.g. PluginEntry's scs_telemetry_shutdown) does not
        // leak the ImGui context. Failures are silent — the
        // caller is about to drop the DLL. The bound
        // FrameLoop / WalkerFsm pointers are cleared first so
        // a late RenderFrameIfEnabled (between the bound
        // provider's destruction and the overlay's destruction)
        // skips the live state section rather than
        // dereferencing freed memory.
        frame_loop_ = nullptr;
        fsm_ = nullptr;
        Shutdown();
        g_overlay = nullptr;
    }
}

void ImGuiOverlay::SetEnabled(bool enabled) {
    enabled_ = enabled;
    WALKERCORE_LOG_INFO(
        "ImGuiOverlay: enabled=%d (set [m11] imgui_overlay = true "
        "in WalkerCore.ini to keep)",
        enabled ? 1 : 0);
}

bool ImGuiOverlay::InitializeForD3D11Device(void *device, void *context,
                                            void *hwnd) {
    // Refuse to init twice. The D3D11 hook fires on every
    // D3D11CreateDeviceAndSwapChain call; only the FIRST capture
    // initializes the overlay. Subsequent calls (e.g. the game
    // recreating its swap chain) hit the device-lost / device-
    // created path below.
    if (context_initialised_) {
        WALKERCORE_LOG_WARN(
            "ImGuiOverlay: InitializeForD3D11Device called twice; "
            "ignoring duplicate call");
        return true;
    }
#if defined(WALKERCORE_HAVE_IMGUI)
    if (device == nullptr || context == nullptr) {
        init_failure_ = "null device or context";
        WALKERCORE_LOG_WARN(
            "ImGuiOverlay: cannot init — device=%p context=%p (hwnd=%p)",
            device, context, hwnd);
        return false;
    }
    // 1) ImGui context. CreateContext allocates the global state.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 2) Win32 backend. hwnd is optional; pass null to get a
    //    working ImGui context that just doesn't process
    //    window messages (the D3D11 hook is the only caller in
    //    WalkerCore, and it never feeds input — the overlay is
    //    a status read-out, not a control surface).
    if (hwnd != nullptr) {
        if (!ImGui_ImplWin32_Init(hwnd)) {
            init_failure_ = "ImGui_ImplWin32_Init failed";
            ImGui::DestroyContext();
            WALKERCORE_LOG_ERROR(
                "ImGuiOverlay: ImGui_ImplWin32_Init failed for hwnd=%p", hwnd);
            return false;
        }
        win32_initialised_ = true;
    }
    // 3) D3D11 backend. Stores the device + context pointers and
    //    creates the font texture + vertex/index buffers.
    if (!ImGui_ImplDX11_Init(static_cast<ID3D11Device *>(device),
                              static_cast<ID3D11DeviceContext *>(context))) {
        init_failure_ = "ImGui_ImplDX11_Init failed";
        if (win32_initialised_) {
            ImGui_ImplWin32_Shutdown();
            win32_initialised_ = false;
        }
        ImGui::DestroyContext();
        WALKERCORE_LOG_ERROR(
            "ImGuiOverlay: ImGui_ImplDX11_Init failed for device=%p context=%p",
            device, context);
        return false;
    }
    dx11_initialised_ = true;
    context_initialised_ = true;
    init_generation_++;
    supported_ = true;
    init_failure_.clear();
    WALKERCORE_LOG_INFO(
        "ImGuiOverlay: initialized (device=%p context=%p hwnd=%p imgui=%s)",
        device, context, hwnd, ImGuiVersionString());

    // M11.4 — install the WndProc subclass on the game hwnd so the
    // overlay's ImGui_ImplWin32_WndProcHandler is in the WndProc chain
    // AND the InputHook's raw-input sink receives WM_INPUT. The
    // subclass is a single shim; both consumers live in the same
    // module so a single install / uninstall pair covers both. The
    // install is conditional on hwnd — without a window, the WndProc
    // has nothing to subclass and the InputHook is also pointless
    // (RegisterRawInputDevices would fail with a null target). The
    // ImGui context is alive by this point, so ImGui's WndProc
    // handler is safe to call from the subclass.
    if (hwnd != nullptr) {
        WindowSubclass_Install(static_cast<HWND>(hwnd));
    }
    return true;
#else
    (void)device;
    (void)context;
    (void)hwnd;
    init_failure_ = "WALKERCORE_HAVE_IMGUI=0";
    WALKERCORE_LOG_INFO(
        "ImGuiOverlay: WALKERCORE_HAVE_IMGUI=0; staying in scaffolding state");
    return false;
#endif
}

void ImGuiOverlay::OnDeviceLost() {
#if defined(WALKERCORE_HAVE_IMGUI)
    if (dx11_initialised_) {
        // Frees the font texture + vertex/index buffers. Does NOT
        // free the backend data struct (that's Shutdown's job).
        // Call before the game destroys the device, then call
        // OnDeviceCreated after the new device is in place.
        ImGui_ImplDX11_InvalidateDeviceObjects();
        WALKERCORE_LOG_INFO("ImGuiOverlay: device lost — GPU resources released");
    }
#else
    (void)0;
#endif
}

void ImGuiOverlay::OnDeviceCreated() {
#if defined(WALKERCORE_HAVE_IMGUI)
    if (context_initialised_ && dx11_initialised_) {
        // Re-creates the font texture + vertex/index buffers. The
        // device + context pointers in the backend data struct
        // are still valid (the game's device is the same one we
        // initialized with).
        ImGui_ImplDX11_CreateDeviceObjects();
        WALKERCORE_LOG_INFO("ImGuiOverlay: device created — GPU resources restored");
    }
#else
    (void)0;
#endif
}

bool ImGuiOverlay::RenderFrameIfEnabled() {
    if (!enabled_ || !context_initialised_ || !supported_) {
        return false;
    }
#if defined(WALKERCORE_HAVE_IMGUI)
    if (!dx11_initialised_) return false;
    // NewFrame / draw / Render / RenderDrawData. We draw a single
    // status window anchored top-left with the overlay's per-frame
    // state; the M11 UI work is a follow-up commit. The pattern
    // matches the upstream examples/ example5_win32_directx11
    // exactly so future maintainers can compare line-for-line.
    ImGui_ImplDX11_NewFrame();
    if (win32_initialised_) {
        ImGui_ImplWin32_NewFrame();
    }
    ImGui::NewFrame();

    // Status window — auto-resize to content, no title bar, no
    // resize grip, no background, semi-transparent. The text
    // contents are deliberately small because most users just
    // want a "is it working?" confirmation on top of the game,
    // not a full HUD. The live state section is only drawn when
    // the bound FrameLoop / WalkerFsm are non-null — the
    // overlay without bindings shows just the version + counters
    // (the scaffolding state).
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("WalkerCore", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    // WALKERCORE_VERSION_STRING already includes the version
    // (0.0.1), the phase tag (m0), the git short SHA, and the
    // build ISO date. Surfacing the build hash in the on-screen
    // overlay closes the deploy-verification loop visually: an
    // operator who can't read game.log (or who forgot to enable
    // the SCS log sink) can still confirm "did I deploy build
    // X?" by glancing at the HUD. The WALKERCORE_VERSION_STRING
    // macro is a compile-time constant so this is a single
    // ImGui::Text call with no runtime cost.
    ImGui::Text("WalkerCore %s (imgui %s)",
                WALKERCORE_VERSION_STRING, ImGuiVersionString());
    if (frame_loop_ != nullptr) {
        // Camera eye + yaw / pitch + FOV. The eye position flips
        // during transitions and only settles when the FSM enters
        // Walking; the FOV is constant unless the user changes the
        // INI at runtime. Operators confirm "F10 produced visible
        // movement" by reading the eye position across two
        // consecutive frames.
        const CameraState &cam = frame_loop_->camera();
        ImGui::Text("cam: (%.1f, %.1f, %.1f) y=%.2f p=%.2f fov=%.1f",
                    static_cast<double>(cam.eye_position.x),
                    static_cast<double>(cam.eye_position.y),
                    static_cast<double>(cam.eye_position.z),
                    static_cast<double>(cam.yaw_radians),
                    static_cast<double>(cam.pitch_radians),
                    static_cast<double>(cam.horizontal_fov_radians));
        // Player feet + mode. has_feet is false in Driving mode
        // (the player is in the cabin, the camera eye is anchored
        // to the seat), true in Walking. The mode field
        // distinguishes Standing / Walking / Crouching / Running
        // when the user is on foot.
        const PlayerState &p = frame_loop_->player();
        if (p.has_feet) {
            ImGui::Text("feet: (%.1f, %.1f, %.1f) h=%.2f %s",
                        static_cast<double>(p.feet_position.x),
                        static_cast<double>(p.feet_position.y),
                        static_cast<double>(p.feet_position.z),
                        static_cast<double>(p.eye_height),
                        PlayerModeName(p.mode));
        } else {
            ImGui::Text("feet: (driver) %s", PlayerModeName(p.mode));
        }
    }
    if (fsm_ != nullptr) {
        // FSM state + tween progress. The transition t value
        // ticks from 0 to 1 over `transition_seconds` (default
        // 0.6s) and is the cleanest signal "is the toggle
        // tween running right now?".
        const float t = fsm_->transitionProgress();
        if (t > 0.0f && t < 1.0f) {
            ImGui::Text("fsm: %s (t=%.2f)",
                        WalkerFsmStateName(fsm_->state()),
                        static_cast<double>(t));
        } else {
            ImGui::Text("fsm: %s", WalkerFsmStateName(fsm_->state()));
        }
    }
    ImGui::Text("walks: %d in / %d out   frames: %d",
                walks_entered_, walks_exited_, frames_rendered_ + 1);

    // M11.5b — PxScene live status. The PxSceneHook (M3.5, commit
    // d353efb) captures the live PxScene instance in RCX on the
    // first PxScene::addActor call the game makes. Showing the
    // captured address + supported flag in the overlay gives the
    // deploy-side analyst immediate visual feedback on whether
    // M3.5 is wired correctly: the instance goes from 0 (no
    // addActor seen yet) to a non-zero address within the first
    // few seconds of driving (every actor the game spawns fires
    // addActor). The "supported" flag is the hook's own check that
    // the target VA is reachable inside this process; it goes 0→1
    // once the MinHook trampoline installs. Both functions are
    // already in the ABI surface (walker_pxscene_supported /
    // walker_pxscene_instance) so no new export is required.
    const int pxs_supported = walker_pxscene_supported();
    const uint64_t pxs_instance = walker_pxscene_instance();
    if (pxs_supported != 0) {
        ImGui::Text("pxs: supported inst=0x%llx",
                    static_cast<unsigned long long>(pxs_instance));
    } else {
        ImGui::Text("pxs: not supported (target VA unreachable)");
    }

    // M11.5 — first interactive widgets. Now that M11.4 (commit
    // 61ebc23) installed the WndProc subclass, the ImGui Win32
    // backend receives real mouse input from the game's WndProc
    // chain. The buttons below are the simplest verification of
    // that round-trip: clicking a button must call the corresponding
    // dump function, the button_clicks_ counter must increment,
    // and the game log must show the dump output (the user can
    // compare button_clicks_ before / after a click).
    //
    // The widgets are read-only diagnostics — they never mutate
    // game state, never trigger transitions, and never write to
    // files. The dump functions are the same walker_dump_state /
    // walker_dump_pxscene that F12 calls; the button is a
    // discoverable UI surface for analysts who would rather click
    // than reach for the keyboard.
    ImGui::Separator();
    if (ImGui::Button("Dump state now")) {
        walker_dump_state();
        ++button_clicks_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump PxScene")) {
        walker_dump_pxscene();
        ++button_clicks_;
    }
    ImGui::Text("button clicks: %d", button_clicks_);
    ImGui::End();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ++frames_rendered_;
    return true;
#else
    return false;
#endif
}

void ImGuiOverlay::Shutdown() {
    // M11.4 — uninstall the WndProc subclass FIRST, before tearing
    // down ImGui's Win32 backend. The subclass routes every input
    // message to ImGui_ImplWin32_WndProcHandler; if the ImGui
    // context is gone first, the routing would forward messages
    // to a stale context (or crash on the ImGui::GetIO() call
    // inside the handler). Uninstall is idempotent and safe to
    // call when not installed, so the unconditional call here is
    // fine even if InitializeForD3D11Device was never called.
    WindowSubclass_Uninstall(nullptr);
#if defined(WALKERCORE_HAVE_IMGUI)
    if (dx11_initialised_) {
        ImGui_ImplDX11_Shutdown();
        dx11_initialised_ = false;
    }
    if (win32_initialised_) {
        ImGui_ImplWin32_Shutdown();
        win32_initialised_ = false;
    }
    if (context_initialised_) {
        ImGui::DestroyContext();
        context_initialised_ = false;
    }
    supported_ = false;
    init_generation_++;
    WALKERCORE_LOG_INFO("ImGuiOverlay: shutdown complete (frames_rendered=%d)",
                        frames_rendered_);
#else
    supported_ = false;
#endif
}

void ImGuiOverlay::ResetForTesting() {
    if (g_overlay != nullptr) {
        g_overlay->Shutdown();
        g_overlay->walks_entered_ = 0;
        g_overlay->walks_exited_ = 0;
        g_overlay->frames_rendered_ = 0;
        g_overlay->button_clicks_ = 0;
        g_overlay->enabled_ = false;
        g_overlay->supported_ = false;
        g_overlay->frame_loop_ = nullptr;
        g_overlay->fsm_ = nullptr;
    }
}

int ImGuiOverlay::TestRenderCount() {
    return (g_overlay != nullptr) ? g_overlay->frames_rendered_ : 0;
}

// -- Free-function C API used by D3D11Hook.cpp ----------------------------

void OnD3D11DeviceCaptured(void *device, void *context, void *hwnd) {
    if (g_overlay == nullptr) return;
    g_overlay->InitializeForD3D11Device(device, context, hwnd);
}

void OnD3D11DeviceLost() {
    if (g_overlay == nullptr) return;
    g_overlay->OnDeviceLost();
}

void RenderImGuiFrameIfEnabled() {
    if (g_overlay == nullptr) return;
    g_overlay->RenderFrameIfEnabled();
}

void ShutdownImGuiOverlay() {
    if (g_overlay == nullptr) return;
    g_overlay->Shutdown();
}

}  // namespace walkercore
