// WalkerCore — diagnostic state dump (M1.5).
//
// walker_dump_state() writes a multi-line plain-ASCII summary of every
// subsystem to the SCS log sink. The function is exported from the DLL
// so an F12 key handler in the input hook, or a third-party tool, can
// request the snapshot without knowing the layout of the global state.
//
// The implementation lives in the adapter layer (not domain) because it
// reads process-wide globals defined by PluginEntry.cpp. The header
// `walkercore/PerFrame.h` is the SDK-free public surface — this file
// does not need the SDK because it only consumes POD structs.
//
// Plain ASCII output: every sink (game log, OutputDebugStringW, stderr)
// renders it legibly.

#include "walkercore/PerFrame.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "walkercore/BuildInfo.h"
#include "walkercore/CameraState.h"
#include "walkercore/D3D11Hook.h"
#include "walkercore/D3D12Hook.h"
#include "walkercore/DevCameraCvarPatcher.h"
#include "walkercore/DiagnosticAccess.h"
#include "walkercore/FrameLoop.h"
#include "walkercore/GameVersion.h"
#include "walkercore/FuelPump.h"
#include "walkercore/GameFreeCameraShim.h"
#include "walkercore/CameraHijack.h"
#include "walkercore/PxSceneHook.h"
#include "walkercore/ImGuiOverlay.h"
#include "walkercore/InputHook.h"
#include "walkercore/InputState.h"
#include "walkercore/Knockout.h"
#include "walkercore/Log.h"
#include "walkercore/PlayerState.h"
#include "walkercore/PhysXAdapter.h"
#include "walkercore/PerTruckDoorTable.h"
#include "walkercore/TruckDoorConfig.h"
#include "walkercore/TrailerCoupling.h"
#include "walkercore/PolishProviders.h"
#include "walkercore/ScsTelemetryProvider.h"
#include "walkercore/Settings.h"
#include "walkercore/SettingsMenu.h"
#include "walkercore/VersionTable.h"
#include "walkercore/WalkerFsm.h"

// Globals defined here. PluginEntry.cpp wires them via the
// diagnostic_internal::Set* accessors declared in DiagnosticAccess.h.
namespace walkercore {
namespace {

const char *g_negotiated_scs_version = "none";
// The canonical FSM state label buffer. Both walker_dump_state() (via
// GetFsmLabel()) and the exported ABI walker_fsm_state_label() read
// from this same buffer; the per-frame conductor (PluginEntry's
// walker_per_frame_tick) writes through SetFsmLabel() with a steady-
// state skip when the FSM state hasn't changed. Previously there were
// two parallel buffers (one in PluginEntry for the export, one here for
// the dump) and the per-frame conductor copied between them via
// snprintf("%s", ...). The unification is part of the per-frame
// hot-path optimisation pass.
char g_fsm_label_buf[64] = "Driving";

// Mirrored GameIdentity, populated by SetIdentity() right after the
// adapter constructs it from the SDK init params. Default empty +
// version=0 → before scs_telemetry_init this dump shows "(not
// initialized)" so analysts can tell "I haven't loaded a game yet"
// from "the build didn't match".
char g_identity_game_id[32] = "";
uint32_t g_identity_game_version = 0;
bool g_identity_has = false;

const WalkerFsm *g_fsm = nullptr;
const ScsTelemetryProvider *g_telemetry = nullptr;
const FrameLoop *g_frame_loop = nullptr;
const SettingsStore *g_settings = nullptr;
const SettingsMenu *g_settings_menu = nullptr;
const InputHook *g_input = nullptr;
const FuelProvider *g_fuel = nullptr;
const KnockoutProvider *g_knockout = nullptr;
const D3D11Hook *g_d3d11_hook = nullptr;
const D3D12Hook *g_d3d12_hook = nullptr;
const FlashlightProvider *g_flashlight = nullptr;
const DoorAnimProvider *g_door_anim = nullptr;
const HeadTrackerProvider *g_head_tracker = nullptr;
const ShadowProvider *g_shadow = nullptr;
const PhysXAdapter *g_physx = nullptr;
const CouplingProvider *g_coupling = nullptr;
// M2 Game-Free-Camera shim (MVP for the GPU render push). Wired by
// PluginEntry at scs_telemetry_init time; null until then. Read by
// walker_dump_game_free_camera_shim() to surface the configured
// toggle / exit VKs, walks_entered / toggles_dispatched counters,
// supported / enabled flags so a deploy-side analyst can confirm the
// shim fired on F10.
GameFreeCameraShim *g_game_free_camera_shim = nullptr;
// M2 dev-camera cvar patcher. Wired by PluginEntry at
// scs_telemetry_init time; null until then. Read by
// walker_dump_dev_camera_cvar_patcher() to surface the patch
// attempt counters, the rebased cvar name + value VAs, and the
// before/after cvar value bytes so the analyst can confirm the
// patch succeeded (or follow the traceback when it skipped).
DevCameraCvarPatcher *g_dev_cvar_patcher = nullptr;
// M2 long-term camera hijack (dispatcher MinHook). Wired by
// PluginEntry at scs_telemetry_init time when the [m2] camera_hijack
// gate is enabled; null until then. Read by walker_dump_camera_hijack()
// to surface the captured image_base + dispatcher VA, install attempt
// + dispatcher call counters, and any install_error captured during
// the MinHook install so a deploy-side analyst can confirm the hook
// actually fired.
CameraHijack *g_camera_hijack = nullptr;
// M3.5 PxScene runtime hook mirror. Wired by PluginEntry at
// scs_telemetry_init time; null until then. Read by
// walker_dump_pxscene() to surface the supported flag, captured
// instance pointer, and addActor call count so a deploy-side
// analyst can confirm the runtime hook fired.
PxSceneHook *g_px_scene_hook = nullptr;
// M11 ImGui overlay mirror. Wired by PluginEntry at
// scs_telemetry_init time; null until then. Read by
// walker_dump_imgui_overlay() to surface the enabled / supported
// flags + the walks_entered / walks_exited counters.
ImGuiOverlay *g_imgui_overlay = nullptr;
// M5 per-truck door config mirror. The model_id and resolved
// TruckDoorConfig are pushed from PluginEntry alongside the FSM
// SetDoorConfig call. The mirror defaults to "(no config yet)" until
// the first push arrives.
constexpr size_t kMaxModelIdLen = 64;
char g_per_truck_model_id[kMaxModelIdLen] = "";
TruckDoorConfig g_per_truck_door{};
bool g_per_truck_door_set = false;

}  // namespace

namespace diagnostic_internal {

void SetScsVersion(const char *v) { g_negotiated_scs_version = v; }
void SetFsmLabel(const char *l) {
    if (l == nullptr) return;
    // Plain-text copy, not format string. strncpy beats snprintf("%s",
    // l) by several times on these short labels and the bounded-write
    // behaviour (NUL termination via the manual terminator below)
    // matches what we need. The per-frame conductor calls us at 60 Hz
    // (M2 lander's D3D11 Present hook); even small wins here are
    // multiplicative.
    const size_t cap = sizeof(g_fsm_label_buf);
    std::strncpy(g_fsm_label_buf, l, cap - 1);
    g_fsm_label_buf[cap - 1] = '\0';
}

size_t EncodeFsmLabel(WalkerFsmState state,
                      float transition_progress,
                      char *out_buf,
                      size_t out_size) {
    if (out_buf == nullptr || out_size == 0) return 0;
    const char *name = "Driving";
    switch (state) {
        case WalkerFsmState::Driving:       name = "Driving";       break;
        case WalkerFsmState::Walking:       name = "Walking";       break;
        case WalkerFsmState::TransitionIn:  name = "TransitionIn";  break;
        case WalkerFsmState::TransitionOut: name = "TransitionOut"; break;
    }
    int n = 0;
    if (state == WalkerFsmState::TransitionIn ||
        state == WalkerFsmState::TransitionOut) {
        // transition_progress is only meaningful during tweens; clamp the
        // displayed value to [0, 1] so a tween that's overshot (rare,
        // can happen if dt > tween_remaining) doesn't print negative or
        // >1.00 floats in the dump.
        if (transition_progress < 0.0f) transition_progress = 0.0f;
        if (transition_progress > 1.0f) transition_progress = 1.0f;
        n = std::snprintf(out_buf, out_size, "%s · t=%.2f",
                          name, static_cast<double>(transition_progress));
    } else {
        n = std::snprintf(out_buf, out_size, "%s", name);
    }
    if (n < 0) {
        out_buf[0] = '\0';
        return 0;
    }
    // snprintf's return value is the would-be-written length, not bounded
    // by out_size. We return only what's actually in the buffer
    // (excluding NUL) — that's what callers care about for "did the
    // buffer fit?".
    if (static_cast<size_t>(n) >= out_size) return out_size - 1;
    return static_cast<size_t>(n);
}

bool FsmLabelNeedsReencode(WalkerFsmState prev, WalkerFsmState now) {
    if (prev != now) return true;
    // Steady-state cache HIT only when the state is one of the two
    // terminal states. Anything in a transition must re-encode every
    // frame because the t value is changing.
    return now == WalkerFsmState::TransitionIn ||
           now == WalkerFsmState::TransitionOut;
}
void SetIdentity(const char *game_id, uint32_t game_version) {
    // Copy up to the buffer size minus terminator, then null-terminate.
    // Empty game_id is acceptable and means "(no game)" — the dump just
    // reports "(not initialized)" in that case via g_identity_has.
    g_identity_game_id[0] = '\0';
    if (game_id != nullptr) {
        std::snprintf(g_identity_game_id, sizeof(g_identity_game_id),
                      "%s", game_id);
    }
    g_identity_game_version = game_version;
    g_identity_has = true;
}
void ClearIdentity() {
    g_identity_game_id[0] = '\0';
    g_identity_game_version = 0;
    g_identity_has = false;
}
const char *GetFsmLabel() { return g_fsm_label_buf; }
void SetFsm(const WalkerFsm *f) { g_fsm = f; }
void SetTelemetry(const ScsTelemetryProvider *t) { g_telemetry = t; }
void SetFrameLoop(const FrameLoop *f) { g_frame_loop = f; }
void SetSettings(const SettingsStore *s) { g_settings = s; }
void SetSettingsMenu(const SettingsMenu *m) { g_settings_menu = m; }
void SetInput(const InputHook *i) { g_input = i; }
void SetFuel(const FuelProvider *f) { g_fuel = f; }
void SetKnockout(const KnockoutProvider *k) { g_knockout = k; }
void SetD3D11(const D3D11Hook *d) { g_d3d11_hook = d; }
void SetD3D12(const D3D12Hook *d) { g_d3d12_hook = d; }
void SetFlashlight(const FlashlightProvider *f) { g_flashlight = f; }
void SetDoorAnim(const DoorAnimProvider *d) { g_door_anim = d; }
void SetHeadTracker(const HeadTrackerProvider *h) { g_head_tracker = h; }
void SetShadow(const ShadowProvider *s) { g_shadow = s; }
void SetPhysX(const PhysXAdapter *p) { g_physx = p; }
void SetCoupling(const CouplingProvider *p) { g_coupling = p; }
void SetGameFreeCameraShim(const GameFreeCameraShim *g) {
    g_game_free_camera_shim = const_cast<GameFreeCameraShim *>(g);
}
void SetDevCameraCvarPatcher(const DevCameraCvarPatcher *p) {
    g_dev_cvar_patcher = const_cast<DevCameraCvarPatcher *>(p);
}
void SetCameraHijack(const CameraHijack *h) {
    g_camera_hijack = const_cast<CameraHijack *>(h);
}
void SetPxSceneHook(const PxSceneHook *h) {
    g_px_scene_hook = const_cast<PxSceneHook *>(h);
}
void SetImGuiOverlay(const ImGuiOverlay *o) {
    g_imgui_overlay = const_cast<ImGuiOverlay *>(o);
}
void SetPerTruckDoor(const char *model_id, size_t model_id_len,
                     const TruckDoorConfig *cfg) {
    g_per_truck_model_id[0] = '\0';
    if (model_id != nullptr && model_id_len > 0) {
        // Bounded copy into the mirror's fixed buffer. model_id_len
        // is the caller's measured length (no NUL search); we copy
        // min(model_id_len, kMaxModelIdLen - 1) bytes and always
        // NUL-terminate.
        const size_t cap = sizeof(g_per_truck_model_id);
        const size_t copy = (model_id_len < cap - 1)
                                ? model_id_len
                                : cap - 1;
        std::memcpy(g_per_truck_model_id, model_id, copy);
        g_per_truck_model_id[copy] = '\0';
    }
    if (cfg != nullptr) {
        g_per_truck_door = *cfg;
        g_per_truck_door_set = true;
    } else {
        g_per_truck_door = TruckDoorConfig{};
        g_per_truck_door_set = false;
    }
}

}  // namespace diagnostic_internal
}  // namespace walkercore

extern "C" __declspec(dllexport) void walker_dump_state(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };
    auto has_str = [](bool has) -> const char * { return has ? "yes" : "no"; };

    WALKERCORE_LOG_INFO("--- WalkerCore state dump ---");
    WALKERCORE_LOG_INFO("scs api version       : %s", g_negotiated_scs_version);

    // The FSM label is written to the mirror buffer by PluginEntry each frame,
    // independently of whether the FSM pointer is currently wired up. The
    // mirror defaults to "Driving" before any update has run, so the label is
    // always a meaningful snapshot of the most recent observed state.
    WALKERCORE_LOG_INFO("fsm state             : %s",
                        diagnostic_internal::GetFsmLabel());

    if (g_telemetry != nullptr) {
        const TruckState &t = g_telemetry->state();
        WALKERCORE_LOG_INFO("truck placement       : has=%s pos=(%.2f, %.2f, %.2f) "
                            "heading=%.3f pitch=%.3f roll=%.3f",
                            has_str(t.has_placement),
                            t.placement.position.x, t.placement.position.y,
                            t.placement.position.z,
                            static_cast<double>(t.placement.orientation.heading),
                            static_cast<double>(t.placement.orientation.pitch),
                            static_cast<double>(t.placement.orientation.roll));
        WALKERCORE_LOG_INFO("truck speed           : has=%s scalar=%.2f m/s",
                            has_str(t.has_speed), t.speed_mps);
        WALKERCORE_LOG_INFO("truck parking brake   : has=%s value=%s",
                            has_str(t.has_parking_brake),
                            bool_str(t.parking_brake));
        WALKERCORE_LOG_INFO("truck trailer attached: has=%s value=%s",
                            has_str(t.has_trailer_attached),
                            bool_str(t.trailer_attached));
        WALKERCORE_LOG_INFO("truck fuel            : has=%s amount=%.2f L",
                            has_str(t.has_fuel),
                            static_cast<double>(t.fuel_amount_l));
        WALKERCORE_LOG_INFO("truck adblue          : has=%s amount=%.2f L",
                            has_str(t.has_adblue),
                            static_cast<double>(t.adblue_amount_l));
    } else {
        WALKERCORE_LOG_INFO("truck                 : (not initialized)");
    }

    if (g_frame_loop != nullptr) {
        const CameraState &c = g_frame_loop->camera();
        WALKERCORE_LOG_INFO("camera eye            : (%.2f, %.2f, %.2f) "
                            "yaw=%.3f pitch=%.3f h_fov=%.3f aspect=%.3f valid=%s",
                            c.eye_position.x, c.eye_position.y, c.eye_position.z,
                            static_cast<double>(c.yaw_radians),
                            static_cast<double>(c.pitch_radians),
                            static_cast<double>(c.horizontal_fov_radians),
                            static_cast<double>(c.aspect),
                            bool_str(c.valid));
        const PlayerState &p = g_frame_loop->player();
        WALKERCORE_LOG_INFO("player feet           : has=%s pos=(%.2f, %.2f, %.2f) "
                            "eye_height=%.3f mode=%s",
                            has_str(p.has_feet),
                            p.feet_position.x, p.feet_position.y, p.feet_position.z,
                            static_cast<double>(p.eye_height),
                            PlayerModeName(p.mode));
    } else {
        WALKERCORE_LOG_INFO("frame loop            : (not initialized)");
    }

    if (g_settings != nullptr) {
        const WalkerSettings s = g_settings->current();
        WALKERCORE_LOG_INFO("settings walk/run/crouch mps : %.2f / %.2f / %.2f",
                            static_cast<double>(s.movement.walkSpeedMps),
                            static_cast<double>(s.movement.runSpeedMps),
                            static_cast<double>(s.movement.crouchSpeedMps));
        WALKERCORE_LOG_INFO("settings mouse sens   : %.4f rad/unit",
                            static_cast<double>(s.movement.mouseSensitivityRadPerUnit));
        WALKERCORE_LOG_INFO("settings head bob amp : v=%.3f m l=%.3f m",
                            static_cast<double>(s.movement.headBobVerticalAmpM),
                            static_cast<double>(s.movement.headBobLateralAmpM));
        WALKERCORE_LOG_INFO("settings horizontal_fov: %.3f rad",
                            static_cast<double>(s.horizontal_fov_radians));
        WALKERCORE_LOG_INFO("settings transition   : %.2f s",
                            static_cast<double>(s.transition_seconds));
    } else {
        WALKERCORE_LOG_INFO("settings              : (not initialized)");
    }

    if (g_input != nullptr) {
        // Snapshot the latest input. We use last() via the public accessor
        // for input sample; the rising-edge bits are visible exactly once
        // (cleared in source on next Sample) so the dump may capture them
        // mid-frame.
        const InputState &in = g_input->last();
        int held_count = 0;
        int pressed_count = 0;
        int released_count = 0;
        const char *last_pressed_name = "(none)";
        for (unsigned i = 0; i < static_cast<unsigned>(WalkercoreKey::LastKey); ++i) {
            const uint8_t b = in.keys[i];
            if (b & 0x01) ++held_count;
            if (b & 0x02) {
                ++pressed_count;
                last_pressed_name = WalkercoreKeyName(static_cast<WalkercoreKey>(i));
            }
            if (b & 0x04) ++released_count;
        }
        WALKERCORE_LOG_INFO("input installed       : %s valid=%s last_update_us=%llu",
                            bool_str(g_input->isInstalled()),
                            bool_str(in.valid),
                            static_cast<unsigned long long>(in.last_update_us));
        WALKERCORE_LOG_INFO("input edges           : held=%d pressed=%d released=%d "
                            "last_pressed=%s",
                            held_count, pressed_count, released_count,
                            last_pressed_name);
        WALKERCORE_LOG_INFO("input mouse           : dx=%d dy=%d wheel=%d",
                            in.mouse_dx, in.mouse_dy, in.wheel_delta);
    } else {
        WALKERCORE_LOG_INFO("input                 : (not initialized)");
    }

    if (g_fuel != nullptr) {
        // The dump distinguishes the telemetry-sourced amount (top, in
        // the truck block) from the local provider's tracked amount
        // (here). When telemetry is present, both should agree; when
        // telemetry is absent, the local amount is the only source of
        // truth and is what the UI should display.
        const FuelStationState &st = g_fuel->station();
        WALKERCORE_LOG_INFO("fuel provider         : fueling=%s tank_cap=%.1f L "
                            "amount=%.2f L adblue=%.2f L",
                            bool_str(g_fuel->isFueling()),
                            g_fuel->tank_capacity_l(),
                            g_fuel->fuel_amount_l(),
                            g_fuel->adblue_amount_l());
        WALKERCORE_LOG_INFO("fuel station         : available=%s dist=%.2f m "
                            "(radius=%.2f m) pos=(%.2f, %.2f, %.2f)",
                            bool_str(st.available),
                            static_cast<double>(st.distance_m),
                            static_cast<double>(st.trigger_radius_m),
                            st.world_position.x, st.world_position.y,
                            st.world_position.z);
    } else {
        WALKERCORE_LOG_INFO("fuel provider         : (not initialized)");
    }

    if (g_knockout != nullptr) {
        const KnockoutState &k = g_knockout->state();
        WALKERCORE_LOG_INFO("knockout             : out=%s recovery_left=%.2f s "
                            "impact_dir=(%.2f, %.2f, %.2f) threshold=%.1f m/s",
                            bool_str(k.knocked_out),
                            k.time_remaining_s,
                            k.impact_direction.x, k.impact_direction.y,
                            k.impact_direction.z,
                            g_knockout->config().velocity_change_threshold_mps);
    } else {
        WALKERCORE_LOG_INFO("knockout             : (not initialized)");
    }

    if (g_d3d11_hook != nullptr) {
        // Swap-chain generation is a free function (the hooked COM
        // callbacks don't have an `this` pointer). Reading it gives the
        // current value, regardless of how many times the game has
        // recreated its device.
        WALKERCORE_LOG_INFO("d3d11 hook           : installed=%s "
                            "swap_chain_gen=%llu",
                            bool_str(g_d3d11_hook->isInstalled()),
                            static_cast<unsigned long long>(
                                walkercore::CurrentSwapChainGeneration()));
    } else {
        WALKERCORE_LOG_INFO("d3d11 hook           : (not initialized)");
    }

    if (g_d3d12_hook != nullptr) {
        WALKERCORE_LOG_INFO("d3d12 hook           : installed=%s "
                            "swap_chain_gen=%llu",
                            bool_str(g_d3d12_hook->isInstalled()),
                            static_cast<unsigned long long>(
                                walkercore::CurrentD3D12SwapChainGeneration()));
    } else {
        WALKERCORE_LOG_INFO("d3d12 hook           : (not initialized)");
    }

    if (g_flashlight != nullptr) {
        // On / off and intensity track each other. Origin / direction
        // are sourced from the camera and only meaningful while on;
        // we report (0,0,0)/(0,0,-1) when off as a stable placeholder
        // rather than burning a branch in the dump consumer.
        const bool on = g_flashlight->isOn();
        const float intensity = on ? 1.0f : 0.0f;
        WALKERCORE_LOG_INFO("flashlight            : on=%s intensity=%.2f",
                            bool_str(on), static_cast<double>(intensity));
    } else {
        WALKERCORE_LOG_INFO("flashlight            : (not initialized)");
    }

    if (g_door_anim != nullptr) {
        // Current angle in radians + whether the tween is still in flight.
        // Renderers that want a door mesh in lockstep with the transition
        // tween read angle_radians directly.
        const walkercore::DoorAnimState ds = g_door_anim->state();
        WALKERCORE_LOG_INFO("door anim             : angle=%.3f rad animating=%s",
                            static_cast<double>(ds.angle_radians),
                            bool_str(ds.animating));
    } else {
        WALKERCORE_LOG_INFO("door anim             : (not initialized)");
    }

    if (g_head_tracker != nullptr) {
        // Snapshot of the smoothed head-tracker output. active=false
        // means no source produced a sample since install (TrackIR /
        // opentrack are stubbed and off by default). The yaw/pitch
        // values are the cumulative offsets that would be applied to
        // the camera as an additive offset by an M7 renderer. Peek()
        // reads without advancing the EMA so the dump doesn't perturb
        // the producer's smoothing state.
        const HeadTrackerState hs = g_head_tracker->Peek();
        WALKERCORE_LOG_INFO("head tracker         : active=%s source=%s "
                            "yaw=%.3f rad pitch=%.3f rad",
                            bool_str(hs.active),
                            HeadTrackerSourceName(hs.dominant_source),
                            static_cast<double>(hs.yaw_radians),
                            static_cast<double>(hs.pitch_radians));
    } else {
        WALKERCORE_LOG_INFO("head tracker         : (not initialized)");
    }

    if (g_shadow != nullptr) {
        // ShadowProvider is a pure function over PlayerState. Compute
        // the snapshot now from the latest FrameLoop player state so
        // the dump reflects where the player actually is. When the
        // FrameLoop isn't wired (early dump), we report "(no player)"
        // because Compute requires a real PlayerState.
        if (g_frame_loop != nullptr) {
            const PlayerState &p = g_frame_loop->player();
            const PlayerShadowState ss = g_shadow->Compute(p);
            WALKERCORE_LOG_INFO(
                "shadow              : active=%s pos=(%.2f, %.2f, %.2f) "
                "radius=%.2f m",
                bool_str(ss.active),
                ss.worldCentre.x, ss.worldCentre.y, ss.worldCentre.z,
                static_cast<double>(ss.radius));
        } else {
            WALKERCORE_LOG_INFO("shadow              : (no player state)");
        }
    } else {
        WALKERCORE_LOG_INFO("shadow              : (not initialized)");
    }

    if (g_physx != nullptr) {
        // Mirror the install-time catalog (the same 7-VA list the
        // adapter logs on Install). We split the line in half so the
        // dump stays legible without forcing a single very-long row:
        //   physx adapter       : installed=true catalog=7 runtime=live
        //   physx catalog       : release=0x141AE07B6 addActor=0x141AE0B62
        //                          simulate=0x141AE4D80 shiftOrigin=0x141AE6C28
        //                          sweep=0x141B06F10 raycast=0x141AE1610
        //                          overlap=0x141AE0F70
        // The runtime column tells the analyst whether the in-memory
        // scan actually ran (live) or whether we're in a unit-test /
        // non-game host (no live process). The focused
        // walker_dump_physx() dump adds per-method resolved_slot.
        const PhysXAdapter::RuntimeCache &cache = g_physx->runtime_cache();
        const bool live = cache.have_scan && cache.have_resolve;
        WALKERCORE_LOG_INFO(
            "physx adapter       : installed=%s catalog=%d runtime=%s",
            bool_str(g_physx->isInstalled()),
            kPhysXKnownMethodCount,
            live ? "live" : "(no live process)");
        char catalog_buf[768];
        size_t off = 0;
        for (int m = 0; m < kPhysXKnownMethodCount; ++m) {
            const int n = std::snprintf(
                catalog_buf + off,
                (off < sizeof(catalog_buf)) ? sizeof(catalog_buf) - off : 0,
                "%s%s=0x%llX",
                (m == 0 ? "" : " "),
                kPhysXKnownMethods[m].name,
                static_cast<unsigned long long>(kPhysXKnownMethods[m].va));
            if (n <= 0 ||
                static_cast<size_t>(n) >= sizeof(catalog_buf) - off) {
                break;
            }
            off += static_cast<size_t>(n);
        }
        // 28 chars/line at 12-pt; 768 / 28 ≈ 27 names per row. Split
        // every ~4 entries (≈ 220 chars) so the dump reads as a
        // multi-row block instead of a single very long line.
        // With the 8-entry catalog (release, addActor, simulate,
        // shiftOrigin, sweep, raycast, overlap, removeActor) this
        // produces [1..4] and [5..8] rows.
        WALKERCORE_LOG_INFO("physx catalog[1..4]  : %.*s",
                            220, catalog_buf);
        if (off > 220) {
            WALKERCORE_LOG_INFO("physx catalog[5..8]  : %.*s",
                                static_cast<int>(off - 220),
                                catalog_buf + 220);
        }
    } else {
        WALKERCORE_LOG_INFO("physx adapter       : (not initialized)");
    }

    if (g_settings_menu != nullptr) {
        // Menu runtime state — open flag, dirty flag. Renderers and
        // analysts want to know "is the menu visible right now?" and
        // "does it have unsaved edits?" without inspecting the
        // SettingsMenu object directly.
        WALKERCORE_LOG_INFO("settings menu       : open=%s dirty=%s",
                            bool_str(g_settings_menu->isOpen()),
                            bool_str(g_settings_menu->isDirty()));
    } else {
        WALKERCORE_LOG_INFO("settings menu       : (not initialized)");
    }

    // M3.5 PxScene runtime hook — surface the captured PxScene
    // pointer inside the full state dump so the user can confirm
    // "did addActor fire and where is the live instance?" without
    // having to press the focused walker_dump_pxscene key. The
    // captured value is informational; it never gates other landers.
    if (g_px_scene_hook != nullptr) {
        WALKERCORE_LOG_INFO(
            "px scene hook       : installed=%s target_reachable=%s "
            "instance=0x%llX addActor_calls=%d",
            bool_str(g_px_scene_hook->isInstalled()),
            bool_str(g_px_scene_hook->target_reachable()),
            static_cast<unsigned long long>(g_px_scene_hook->captured_instance()),
            g_px_scene_hook->add_actor_calls());
    } else {
        WALKERCORE_LOG_INFO("px scene hook       : (not initialized)");
    }

    VersionTable::DumpToLog();

    WALKERCORE_LOG_INFO("--- end dump ---");
}

extern "C" __declspec(dllexport) void walker_dump_settings(void) {
    using namespace walkercore;

    WALKERCORE_LOG_INFO("--- WalkerCore settings ---");
    if (g_settings == nullptr) {
        WALKERCORE_LOG_INFO("settings              : (not initialized)");
    } else {
        const WalkerSettings &s = g_settings->current();
        const MovementConfig &m = s.movement;
        const KeyBindings &k = s.keys;

        // Movement / locomotion
        WALKERCORE_LOG_INFO("walk_speed_mps        : %.3f", static_cast<double>(m.walkSpeedMps));
        WALKERCORE_LOG_INFO("run_speed_mps         : %.3f", static_cast<double>(m.runSpeedMps));
        WALKERCORE_LOG_INFO("crouch_speed_mps      : %.3f", static_cast<double>(m.crouchSpeedMps));
        WALKERCORE_LOG_INFO("mouse_sensitivity     : %.5f rad/unit",
                            static_cast<double>(m.mouseSensitivityRadPerUnit));
        WALKERCORE_LOG_INFO("head_bob_vertical_m   : %.3f",
                            static_cast<double>(m.headBobVerticalAmpM));
        WALKERCORE_LOG_INFO("head_bob_lateral_m    : %.3f",
                            static_cast<double>(m.headBobLateralAmpM));
        WALKERCORE_LOG_INFO("head_bob_frequency_hz : %.1f",
                            static_cast<double>(m.headBobFrequencyHz));
        WALKERCORE_LOG_INFO("eye_height_standing_m : %.2f",
                            static_cast<double>(m.eyeHeightStandingM));
        WALKERCORE_LOG_INFO("eye_height_crouching_m: %.2f",
                            static_cast<double>(m.eyeHeightCrouchingM));

        // Camera + transitions
        WALKERCORE_LOG_INFO("horizontal_fov_radians: %.3f",
                            static_cast<double>(s.horizontal_fov_radians));
        WALKERCORE_LOG_INFO("transition_seconds    : %.2f",
                            static_cast<double>(s.transition_seconds));
        WALKERCORE_LOG_INFO("near_z                : %.2f", static_cast<double>(s.near_z));
        WALKERCORE_LOG_INFO("far_z                 : %.1f", static_cast<double>(s.far_z));

        // Key bindings — VK codes in decimal so OutputDebugStringW renders
        // them legibly across locales. Analysts who need the symbolic name
        // can decode via Win32 VK_* tables.
        WALKERCORE_LOG_INFO("key forward_vk        : %d", k.forward_vk);
        WALKERCORE_LOG_INFO("key backward_vk       : %d", k.backward_vk);
        WALKERCORE_LOG_INFO("key strafe_left_vk    : %d", k.strafe_left_vk);
        WALKERCORE_LOG_INFO("key strafe_right_vk   : %d", k.strafe_right_vk);
        WALKERCORE_LOG_INFO("key run_vk            : %d", k.run_vk);
        WALKERCORE_LOG_INFO("key crouch_vk         : %d", k.crouch_vk);
        WALKERCORE_LOG_INFO("key jump_vk           : %d", k.jump_vk);
        WALKERCORE_LOG_INFO("key interact_vk       : %d", k.interact_vk);
        WALKERCORE_LOG_INFO("key toggle_walk_vk    : %d", k.toggle_walk_vk);
        WALKERCORE_LOG_INFO("key flashlight_vk     : %d", k.flashlight_vk);
        WALKERCORE_LOG_INFO("key eye_up_vk         : %d", k.eye_up_vk);
        WALKERCORE_LOG_INFO("key eye_down_vk       : %d", k.eye_down_vk);
        WALKERCORE_LOG_INFO("key eye_reset_vk      : %d", k.eye_reset_vk);
        WALKERCORE_LOG_INFO("key dump_state_vk     : %d", k.dump_state_vk);
    }
    WALKERCORE_LOG_INFO("--- end settings ---");
}

extern "C" __declspec(dllexport) void walker_dump_menu(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore settings menu ---");
    if (g_settings_menu == nullptr) {
        WALKERCORE_LOG_INFO("menu                 : (not initialized)");
    } else {
        // Surface the two runtime flags that matter when something
        // looks broken: is the menu visible right now (open) and
        // does it have unsaved edits (dirty). Renderers that want to
        // confirm "is the Ctrl+F10 handler firing?" can compare
        // consecutive walker_dump_menu() outputs.
        WALKERCORE_LOG_INFO("menu                 : open=%s dirty=%s",
                            bool_str(g_settings_menu->isOpen()),
                            bool_str(g_settings_menu->isDirty()));
    }
    WALKERCORE_LOG_INFO("--- end menu ---");
}

extern "C" __declspec(dllexport) void walker_dump_head_tracker(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore head tracker ---");
    if (g_head_tracker == nullptr) {
        WALKERCORE_LOG_INFO("head tracker         : (not initialized)");
    } else {
        // Surface the live config so analysts can verify their INI /
        // menu overrides (use_mouse / use_trackir / use_opentrack +
        // per-source sensitivities + smoothing factor) actually reached
        // the provider. Then dump the latest smoothed output via Peek()
        // so we don't perturb the EMA.
        const HeadTrackerConfig &cfg = g_head_tracker->config();
        WALKERCORE_LOG_INFO("config mouse         : use=%s sens=%.4f rad/unit",
                            bool_str(cfg.use_mouse),
                            static_cast<double>(cfg.mouse_sensitivity_rad_per_unit));
        WALKERCORE_LOG_INFO("config trackir       : use=%s sens=%.4f rad/unit",
                            bool_str(cfg.use_trackir),
                            static_cast<double>(cfg.trackir_sensitivity_rad_per_unit));
        WALKERCORE_LOG_INFO("config opentrack     : use=%s sens=%.4f rad/unit",
                            bool_str(cfg.use_opentrack),
                            static_cast<double>(cfg.opentrack_sensitivity_rad_per_unit));
        WALKERCORE_LOG_INFO("config smoothing     : %.3f",
                            static_cast<double>(cfg.smoothing_factor));

        const HeadTrackerState hs = g_head_tracker->Peek();
        WALKERCORE_LOG_INFO("state active         : %s",
                            bool_str(hs.active));
        WALKERCORE_LOG_INFO("state dominant source: %s",
                            HeadTrackerSourceName(hs.dominant_source));
        WALKERCORE_LOG_INFO("state yaw            : %.3f rad",
                            static_cast<double>(hs.yaw_radians));
        WALKERCORE_LOG_INFO("state pitch          : %.3f rad",
                            static_cast<double>(hs.pitch_radians));
    }
    WALKERCORE_LOG_INFO("--- end head tracker ---");
}

extern "C" __declspec(dllexport) void walker_dump_shadow(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore shadow ---");
    if (g_shadow == nullptr) {
        WALKERCORE_LOG_INFO("shadow              : (not initialized)");
    } else {
        // Surface the live config (sun direction, scale, softness,
        // colour) so analysts can verify overrides took effect. Then
        // compute the projected shadow from the current FrameLoop
        // player state — same code path the full dump uses.
        const ShadowConfig &cfg = g_shadow->config();
        WALKERCORE_LOG_INFO("config sun direction : (%.3f, %.3f, %.3f)",
                            static_cast<double>(cfg.sun_direction.x),
                            static_cast<double>(cfg.sun_direction.y),
                            static_cast<double>(cfg.sun_direction.z));
        WALKERCORE_LOG_INFO("config scale         : %.3f",
                            static_cast<double>(cfg.scale));
        WALKERCORE_LOG_INFO("config softness      : %.3f m",
                            static_cast<double>(cfg.softness));
        WALKERCORE_LOG_INFO("config rgba          : 0x%08X",
                            static_cast<unsigned>(cfg.rgba));

        if (g_frame_loop == nullptr) {
            WALKERCORE_LOG_INFO("shadow state         : (no player state)");
        } else {
            const PlayerState &p = g_frame_loop->player();
            const PlayerShadowState ss = g_shadow->Compute(p);
            WALKERCORE_LOG_INFO("shadow state         : active=%s "
                                "pos=(%.2f, %.2f, %.2f) radius=%.3f m",
                                bool_str(ss.active),
                                ss.worldCentre.x, ss.worldCentre.y,
                                ss.worldCentre.z,
                                static_cast<double>(ss.radius));
        }
    }
    WALKERCORE_LOG_INFO("--- end shadow ---");
}

extern "C" __declspec(dllexport) void walker_dump_flashlight(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore flashlight ---");
    if (g_flashlight == nullptr) {
        WALKERCORE_LOG_INFO("flashlight          : (not initialized)");
    } else {
        // Surface the on/off flag (the user's G or RMB toggle) then
        // recompute the world-space state from the live camera so
        // analysts can see where the cone would point if a real
        // point-light was injected. We do not perturb the provider
        // state — FlashlightProvider::Compute is const.
        WALKERCORE_LOG_INFO("flashlight on       : %s",
                            bool_str(g_flashlight->isOn()));
        if (g_frame_loop == nullptr) {
            WALKERCORE_LOG_INFO("flashlight state    : (no camera state)");
        } else {
            const CameraState &c = g_frame_loop->camera();
            const FlashlightState fs = g_flashlight->Compute(c);
            WALKERCORE_LOG_INFO("flashlight state    : %s",
                                bool_str(fs.on));
            WALKERCORE_LOG_INFO("origin              : (%.3f, %.3f, %.3f)",
                                fs.origin.x, fs.origin.y, fs.origin.z);
            WALKERCORE_LOG_INFO("direction           : (%.3f, %.3f, %.3f)",
                                fs.direction.x, fs.direction.y,
                                fs.direction.z);
            WALKERCORE_LOG_INFO("cone inner/outer    : %.3f / %.3f rad",
                                static_cast<double>(fs.inner_cone_rad),
                                static_cast<double>(fs.outer_cone_rad));
            WALKERCORE_LOG_INFO("intensity           : %.3f",
                                static_cast<double>(fs.intensity));
            WALKERCORE_LOG_INFO("colour              : 0x%08X",
                                static_cast<unsigned>(fs.rgba));
        }
    }
    WALKERCORE_LOG_INFO("--- end flashlight ---");
}

extern "C" __declspec(dllexport) void walker_dump_door(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore door ---");
    if (g_door_anim == nullptr) {
        WALKERCORE_LOG_INFO("door anim           : (not initialized)");
    } else {
        // Surface the current angle, the animating flag, and the
        // currently-attached target (if any). When the door is closed
        // and idle the angles stay at 0; when the FSM fires AnimateTo
        // during a Driving↔Walking transition we move toward ±π/2.
        const DoorAnimState ds = g_door_anim->state();
        WALKERCORE_LOG_INFO("door angle          : %.3f rad",
                            static_cast<double>(ds.angle_radians));
        WALKERCORE_LOG_INFO("door animating      : %s",
                            bool_str(ds.animating));
    }
    WALKERCORE_LOG_INFO("--- end door ---");
}

extern "C" __declspec(dllexport) void walker_dump_version(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore version ---");
    if (!g_identity_has) {
        // Pre-init: scs_telemetry_init hasn't run yet. Distinguish
        // "I forgot to wire SetIdentity" from "no game" by reading
        // the SCS API version mirror too.
        WALKERCORE_LOG_INFO("scs api version      : %s",
                            g_negotiated_scs_version);
        WALKERCORE_LOG_INFO("game identity        : (not initialized)");
    } else {
        // Reconstruct the GameIdentity so we can rerun the
        // VersionTable::Lookup() here (mirror-only state — no other
        // plug-in code depends on it).
        const GameIdentity id = GameIdentity::FromFields(
            g_identity_game_id, g_identity_game_version);

        WALKERCORE_LOG_INFO("scs api version      : %s",
                            g_negotiated_scs_version);
        WALKERCORE_LOG_INFO("game id              : %s", id.game_id);
        WALKERCORE_LOG_INFO("game version         : %u.%u (encoded=0x%08X)",
                            id.major, id.minor,
                            static_cast<unsigned>(id.game_version));
        WALKERCORE_LOG_INFO("is ets2              : %s",
                            bool_str(id.is_ets2));

        const VersionEntry *entry = VersionTable::Lookup(id);
        if (entry != nullptr) {
            WALKERCORE_LOG_INFO("matched entry        : %s "
                                "[v=%u.%u..%u.%u] features=0x%08X",
                                entry->label,
                                (entry->version_min >> 16) & 0xffffu,
                                entry->version_min & 0xffffu,
                                (entry->version_max >> 16) & 0xffffu,
                                entry->version_max & 0xffffu,
                                entry->features_enabled);
            // Surface each feature bit on its own line so analysts can
            // grep "feature walk" / "feature collision" without
            // decoding the bitmask. FeatureName() is the canonical
            // name lookup and is exhaustively covered by the
            // -Wswitch-enum + TestFeatureNameReturnsNameForEveryValue
            // pairing — adding a new Feature bit forces the dump to
            // also report it.
            const Feature kAll[] = {
                FeatureFreeCamera,
                FeatureCollision,
                FeatureWalk,
                FeatureEnterExit,
                FeatureConfigReload,
                FeatureHeadTracker,
                FeatureShadow,
                FeatureFlashlight,
                FeatureDoorAnimation,
                FeatureFuelPump,
                FeatureAdBlue,
                FeatureTrailerCoupling,
                FeatureKnockOut,
            };
            for (const Feature f : kAll) {
                WALKERCORE_LOG_INFO("%-24s: %s",
                                    FeatureName(f),
                                    bool_str(VersionTable::FeatureIsOn(
                                        *entry, f)));
            }
        } else {
            WALKERCORE_LOG_INFO("matched entry        : (no entry matched)");
        }
    }
    WALKERCORE_LOG_INFO("--- end version ---");
}

extern "C" __declspec(dllexport) void walker_dump_fsm(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore fsm ---");
    if (g_fsm == nullptr) {
        // Pre-init / post-shutdown: dump only the mirrored label so
        // analysts see something familiar. SetFsmLabel is updated by
        // PluginEntry each frame, so this is a meaningful snapshot
        // even before the FSM pointer is bound.
        WALKERCORE_LOG_INFO("fsm                 : (not initialized)");
        WALKERCORE_LOG_INFO("last observed label : %s",
                            diagnostic_internal::GetFsmLabel());
    } else {
        // Live values come from the FSM directly so this dump
        // reports what the renderer would see this very frame.
        // WalkerFsmStateName() is the canonical name lookup and is
        // exhaustively covered by the -Wswitch-enum +
        // TestWalkerFsmStateNameReturnsNameForEveryValue pairing —
        // adding a new WalkerFsmState forces the dump to also report
        // it.
        WALKERCORE_LOG_INFO("fsm state           : %s",
                            WalkerFsmStateName(g_fsm->state()));
        WALKERCORE_LOG_INFO("transition progress : %.3f",
                            static_cast<double>(g_fsm->transitionProgress()));
        WALKERCORE_LOG_INFO("should override rdr : %s",
                            bool_str(g_fsm->ShouldRenderOverride()));
    }
    WALKERCORE_LOG_INFO("--- end fsm ---");
}

extern "C" __declspec(dllexport) void walker_dump_input(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore input ---");
    if (g_input == nullptr) {
        WALKERCORE_LOG_INFO("input                 : (not initialized)");
        WALKERCORE_LOG_INFO("--- end input ---");
        return;
    }

    const InputState &in = g_input->last();
    // Counts of per-bit states for the summary row.
    int held_count = 0;
    int pressed_count = 0;
    int released_count = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(WalkercoreKey::LastKey); ++i) {
        const uint8_t b = in.keys[i];
        if (b & 0x01) ++held_count;
        if (b & 0x02) ++pressed_count;
        if (b & 0x04) ++released_count;
    }

    WALKERCORE_LOG_INFO("input installed       : %s",
                        bool_str(g_input->isInstalled()));
    WALKERCORE_LOG_INFO("input valid           : %s", bool_str(in.valid));
    WALKERCORE_LOG_INFO("last update us        : %llu",
                        static_cast<unsigned long long>(in.last_update_us));
    WALKERCORE_LOG_INFO("held / pressed / rel  : %d / %d / %d",
                        held_count, pressed_count, released_count);
    WALKERCORE_LOG_INFO("mouse dx/dy/wheel     : %d / %d / %d",
                        in.mouse_dx, in.mouse_dy, in.wheel_delta);

    // Per-key rows: only emit keys that have any bit set. The full
    // dump scans every key so we add an opt-in row here for greppable
    // per-key detail (e.g. "key Forward: held=true pressed=false")
    // without burying the dump under 17 lines of "all false".
    if (!in.valid) {
        WALKERCORE_LOG_INFO("per-key table         : (no sample yet)");
    } else {
        for (unsigned i = 0; i < static_cast<unsigned>(WalkercoreKey::LastKey); ++i) {
            const uint8_t b = in.keys[i];
            if (b == 0) continue;  // skip quiet keys
            WALKERCORE_LOG_INFO("key %-17s    : held=%s pressed=%s released=%s",
                                WalkercoreKeyName(static_cast<WalkercoreKey>(i)),
                                bool_str((b & 0x01) != 0),
                                bool_str((b & 0x02) != 0),
                                bool_str((b & 0x04) != 0));
        }
    }
    WALKERCORE_LOG_INFO("--- end input ---");
}

extern "C" __declspec(dllexport) void walker_dump_physx(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore physx ---");
    if (g_physx == nullptr) {
        WALKERCORE_LOG_INFO("physx adapter       : (not initialized)");
        WALKERCORE_LOG_INFO("--- end physx ---");
        return;
    }

    // Mirror the install-time configuration so analysts can confirm
    // "did the INI reach the adapter?" (defaults: 0.35 m capsule radius,
    // 1.85 m capsule height, 1.0 m max sweep, 0.87 rad slope limit).
    const PhysXAdapterConfig &cfg = g_physx->config_for_dump();
    WALKERCORE_LOG_INFO("physx installed       : %s",
                        bool_str(g_physx->isInstalled()));
    WALKERCORE_LOG_INFO("capsule radius        : %.3f m",
                        static_cast<double>(cfg.capsule_radius));
    WALKERCORE_LOG_INFO("capsule height        : %.3f m",
                        static_cast<double>(cfg.capsule_height));
    WALKERCORE_LOG_INFO("max sweep distance    : %.3f m",
                        static_cast<double>(cfg.max_sweep_distance));
    WALKERCORE_LOG_INFO("max slope angle       : %.3f rad",
                        static_cast<double>(cfg.max_slope_angle));
    WALKERCORE_LOG_INFO("disable collisions    : %s",
                        bool_str(cfg.disable_collisions));

    // Catalog + slot dispatch. The runtime column is sourced from
    // the install-time cache populated by PhysXAdapter_RuntimeScan:
    // when amtrucks.exe is loaded and the in-memory PxScene vtable
    // is reachable, each row carries the actual resolved slot from
    // ResolveKnownMethods. When the scan could not run (unit tests,
    // non-game hosts), the runtime column stays at -1 and we emit a
    // "no live process" summary so analysts know what is happening.
    const PhysXAdapter::RuntimeCache &cache = g_physx->runtime_cache();
    const bool live = cache.have_scan && cache.have_resolve;
    static constexpr int kExpectedSlot[kPhysXKnownMethodCount] = {
        2,    // release      → slot 2  on ATS 1.61.3.1s
        11,   // addActor     → slot 11
        57,   // simulate     → slot 57
        117,  // shiftOrigin  → slot 117
        -1,   // sweep: lives in 5-slot run @ RVA 0x2507C08, slot 4
        15,   // raycast      → slot 15
        13,   // overlap      → slot 13
        14,   // removeActor  → slot 14
    };
    const int kResolvedSlot[kPhysXKnownMethodCount] = {
        cache.resolve.release_slot,
        cache.resolve.add_actor_slot,
        cache.resolve.simulate_slot,
        cache.resolve.shift_origin_slot,
        cache.resolve.sweep_slot,
        cache.resolve.raycast_slot,
        cache.resolve.overlap_slot,
        cache.resolve.remove_actor_slot,
    };
    WALKERCORE_LOG_INFO(
        "physx runtime         : %s strategy=%s vtable_off=0x%llX run=%d "
        "string_probes=%d",
        live ? "live" : "(no live process)",
        cache.scan.strategy,
        static_cast<unsigned long long>(cache.scan.vtable.vtable_base),
        cache.scan.vtable.accepted_run,
        cache.scan.string_probes_hit);
    WALKERCORE_LOG_INFO("catalog size          : %d", kPhysXKnownMethodCount);
    for (int m = 0; m < kPhysXKnownMethodCount; ++m) {
        WALKERCORE_LOG_INFO(
            "physx[%d] %-12s : va=0x%llX expected_slot=%d resolved_slot=%d",
            m, kPhysXKnownMethods[m].name,
            static_cast<unsigned long long>(kPhysXKnownMethods[m].va),
            kExpectedSlot[m],
            kResolvedSlot[m]);
    }
    WALKERCORE_LOG_INFO(
        "physx resolve         : tolerance=%llu B min_run=%d "
        "(matches the 0x25020A0 vtable for ATS 1.61.3.1s)",
        static_cast<unsigned long long>(kPhysXResolveSlotTolerance),
        kMinPhysXVtableRun);

    WALKERCORE_LOG_INFO("--- end physx ---");
}

extern "C" __declspec(dllexport) void walker_dump_knockout(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore knockout ---");
    if (g_knockout == nullptr) {
        WALKERCORE_LOG_INFO("knockout            : (not initialized)");
        WALKERCORE_LOG_INFO("--- end knockout ---");
        return;
    }

    // Live config + state. The config fields are knobs the user can
    // change via M6 INI; the state fields are what the heuristic
    // actually computed this frame. Listing both side by side lets
    // analysts answer "did the INI edit take effect?" and "is the
    // current knock-out real or spurious?" without scrolling through
    // walker_dump_state().
    const KnockoutConfig &cfg = g_knockout->config();
    const KnockoutState &st = g_knockout->state();
    WALKERCORE_LOG_INFO("knocked_out          : %s",
                        bool_str(st.knocked_out));
    WALKERCORE_LOG_INFO("time_remaining_s     : %.2f",
                        st.time_remaining_s);
    WALKERCORE_LOG_INFO("impact_direction     : (%.3f, %.3f, %.3f)",
                        st.impact_direction.x, st.impact_direction.y,
                        st.impact_direction.z);
    WALKERCORE_LOG_INFO("config vel_change_mps: %.1f",
                        static_cast<double>(cfg.velocity_change_threshold_mps));
    WALKERCORE_LOG_INFO("config snap_reprime_m : %.2f",
                        static_cast<double>(cfg.teleport_reprime_distance_m));
    WALKERCORE_LOG_INFO("config recovery_s    : %.2f",
                        static_cast<double>(cfg.recovery_seconds));
    WALKERCORE_LOG_INFO("config fall_height_m : %.2f",
                        static_cast<double>(cfg.fall_height_threshold_m));

    // Useful derived diagnostic: |impact_direction|. A normalised
    // normal has |v| ≈ 1.0; anything wildly off (e.g. 0 because the
    // heuristic fired without normalising, or > 1 because the code
    // forgot to divide by |dist|) is suspicious. We surface this so
    // the F12 dump can spot a sloppy heuristic without rerunning it.
    const double mag = std::sqrt(
        st.impact_direction.x * st.impact_direction.x +
        st.impact_direction.y * st.impact_direction.y +
        st.impact_direction.z * st.impact_direction.z);
    WALKERCORE_LOG_INFO("impact_direction_mag : %.3f (expect ≈ 1.0 when normalised)",
                        mag);

    WALKERCORE_LOG_INFO("--- end knockout ---");
}

extern "C" __declspec(dllexport) void walker_dump_per_truck_door(void) {
    using namespace walkercore;

    WALKERCORE_LOG_INFO("--- WalkerCore per-truck door ---");
    if (!g_per_truck_door_set) {
        WALKERCORE_LOG_INFO("per-truck door       : (no config yet)");
        WALKERCORE_LOG_INFO("--- end per-truck door ---");
        return;
    }

    // Look up the row in the built-in table so the dump can show
    // "matched row: yes/no" alongside the resolved geometry. When
    // model_id is empty (no telemetry yet) we still emit a row
    // saying the table was bypassed.
    bool matched = false;
    if (g_per_truck_model_id[0] != '\0') {
        matched =
            PerTruckDoorTable::BuiltIn().Find(g_per_truck_model_id) != nullptr;
    }

    WALKERCORE_LOG_INFO("model_id             : %s%s",
                        g_per_truck_model_id,
                        matched ? " (matched built-in row)" : " (no built-in row)");
    WALKERCORE_LOG_INFO("config valid         : %s",
                        g_per_truck_door.valid ? "true" : "false");
    WALKERCORE_LOG_INFO("has_passenger_door   : %s",
                        g_per_truck_door.has_passenger_door ? "true" : "false");
    WALKERCORE_LOG_INFO(
        "driver_door.position : (%.2f, %.2f, %.2f)",
        g_per_truck_door.driver_door.local_position.x,
        g_per_truck_door.driver_door.local_position.y,
        g_per_truck_door.driver_door.local_position.z);
    WALKERCORE_LOG_INFO(
        "driver_door.anchor   : (%.2f, %.2f, %.2f)",
        g_per_truck_door.driver_door.local_enter_anchor.x,
        g_per_truck_door.driver_door.local_enter_anchor.y,
        g_per_truck_door.driver_door.local_enter_anchor.z);
    WALKERCORE_LOG_INFO("driver_door.yaw_off  : %.3f rad",
                        static_cast<double>(
                            g_per_truck_door.driver_door.enter_yaw_offset_rad));
    WALKERCORE_LOG_INFO(
        "cabin_min/max        : (%.2f, %.2f, %.2f) .. (%.2f, %.2f, %.2f)",
        g_per_truck_door.cabin_min.x, g_per_truck_door.cabin_min.y,
        g_per_truck_door.cabin_min.z, g_per_truck_door.cabin_max.x,
        g_per_truck_door.cabin_max.y, g_per_truck_door.cabin_max.z);
    WALKERCORE_LOG_INFO(
        "driver_seat_local    : (%.2f, %.2f, %.2f)",
        g_per_truck_door.driver_seat_local.x,
        g_per_truck_door.driver_seat_local.y,
        g_per_truck_door.driver_seat_local.z);
    WALKERCORE_LOG_INFO("door_max_open_rad    : %.3f rad (%.1f deg)",
                        static_cast<double>(g_per_truck_door.door_max_open_rad),
                        static_cast<double>(g_per_truck_door.door_max_open_rad) *
                            (180.0 / 3.14159265358979323846));
    WALKERCORE_LOG_INFO("door_open_duration_s : %.2f",
                        static_cast<double>(g_per_truck_door.door_open_duration_s));
    WALKERCORE_LOG_INFO("door_activation_dist : %.2f m",
                        static_cast<double>(
                            g_per_truck_door.door_activation_distance_m));
    WALKERCORE_LOG_INFO("cabin_inside_dist    : %.2f m",
                        static_cast<double>(g_per_truck_door.cabin_inside_distance_m));

    WALKERCORE_LOG_INFO("--- end per-truck door ---");
}

extern "C" __declspec(dllexport) void walker_dump_coupling(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore coupling ---");
    if (g_coupling == nullptr) {
        WALKERCORE_LOG_INFO("coupling             : (not initialized)");
        WALKERCORE_LOG_INFO("--- end coupling ---");
        return;
    }

    const CouplingState st = g_coupling->state();
    WALKERCORE_LOG_INFO("active              : %s",
                        bool_str(g_coupling->isActive()));
    WALKERCORE_LOG_INFO("step                : %s (%u)",
                        CouplingStepName(st.step),
                        static_cast<unsigned>(st.step));
    WALKERCORE_LOG_INFO("progress            : %.3f",
                        static_cast<double>(st.progress));
    WALKERCORE_LOG_INFO(
        "waypoint_local      : (%.2f, %.2f, %.2f)",
        st.waypoint_local.x, st.waypoint_local.y, st.waypoint_local.z);
    WALKERCORE_LOG_INFO("waypoint_radius_m   : %.2f",
                        static_cast<double>(st.waypoint_radius_m));

    WALKERCORE_LOG_INFO("--- end coupling ---");
}

extern "C" __declspec(dllexport) void walker_dump_game_free_camera_shim(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore game-free-camera shim ---");
    if (g_game_free_camera_shim == nullptr) {
        WALKERCORE_LOG_INFO("shim                : (not initialized)");
        WALKERCORE_LOG_INFO("--- end game-free-camera shim ---");
        return;
    }
    const GameFreeCameraShim *s = g_game_free_camera_shim;
    WALKERCORE_LOG_INFO("enabled             : %s",
                        bool_str(s->isEnabled()));
    WALKERCORE_LOG_INFO("supported           : %s",
                        bool_str(s->isSupported()));
    WALKERCORE_LOG_INFO("walks_entered       : %d",
                        s->walks_entered());
    WALKERCORE_LOG_INFO("toggles_dispatched  : %d",
                        s->toggles_dispatched());
    WALKERCORE_LOG_INFO("last_dispatch_ms    : %d",
                        s->last_dispatch_ms());
    WALKERCORE_LOG_INFO("toggle_vk (hex)     : 0x%X",
                        s->toggle_vk());
    WALKERCORE_LOG_INFO("exit_press_vk (hex) : 0x%X",
                        s->exit_press_vk());
    // M2 lander SendInput dispatch — surfaces which path the shim
    // used on its most recent toggle, and the bound game HWND that
    // SendInput was synthesised against. Without these, a regression
    // where the shim silently skips dispatch (no bound HWND, or a
    // future mod stack where SendInput is foreground-locked) is
    // invisible to the deploy-side analyst reading the F12 dump.
    WALKERCORE_LOG_INFO("last_dispatch_path  : %s",
                        s->last_dispatch_path() == 1
                            ? "SendInput"
                            : "none");
    WALKERCORE_LOG_INFO("postmessage_target  : 0x%X",
                        s->last_postmessage_target_diag());
    // M2.5: surface the controls.sii auto-detect state. The user
    // can press F12 to confirm the right key was picked up
    // (auto-detected-key vs the default VK_NUMPAD0), the controls.sii
    // path that was probed, and whether the INI overrode the
    // auto-detect via [m2] dev_camera_toggle_vk. These three lines
    // are the cross-reference surface that resolves the
    // "the shim fired but my Numpad-0 didn't toggle the dev camera"
    // support question in one screen.
    WALKERCORE_LOG_INFO("ini_overrides_vk    : %s",
                        bool_str(s->ini_overrides_toggle_vk()));
    WALKERCORE_LOG_INFO("auto_detected_key   : %s",
                        s->auto_detected_key_name().empty()
                            ? "<none in controls.sii>"
                            : s->auto_detected_key_name().c_str());
    WALKERCORE_LOG_INFO("controls_sii_path   : %s",
                        s->auto_detected_controls_sii_path().empty()
                            ? "<not found>"
                            : s->auto_detected_controls_sii_path().c_str());
    WALKERCORE_LOG_INFO("--- end game-free-camera shim ---");
}

extern "C" __declspec(dllexport) void walker_dump_camera_hijack(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore camera hijack (M2 long-term) ---");
    if (g_camera_hijack == nullptr) {
        WALKERCORE_LOG_INFO("camera_hijack       : (not initialized — gate closed)");
        WALKERCORE_LOG_INFO("--- end camera hijack ---");
        return;
    }
    const CameraHijack *h = g_camera_hijack;
    WALKERCORE_LOG_INFO("enabled             : %s", bool_str(h->isEnabled()));
    WALKERCORE_LOG_INFO("installed           : %s", bool_str(h->IsInstalled()));
    WALKERCORE_LOG_INFO("install_attempts    : %d", h->install_attempts());
    WALKERCORE_LOG_INFO("dispatch_calls      : %d", h->dispatch_calls());
    WALKERCORE_LOG_INFO("last_dispatch_ms    : %d", h->last_dispatch_ms());
    WALKERCORE_LOG_INFO("image_base (hex)    : 0x%llX",
                        static_cast<unsigned long long>(h->image_base()));
    WALKERCORE_LOG_INFO("dispatcher_va (hex) : 0x%llX",
                        static_cast<unsigned long long>(h->dispatcher_va()));
    if (h->install_error()) {
        WALKERCORE_LOG_INFO("install_error       : %s", h->install_error_msg());
    }
    WALKERCORE_LOG_INFO("v2 (lazy singleton walk + vtable[16]):");
    WALKERCORE_LOG_INFO("vtable16_installed  : %s", bool_str(h->vtable16_installed()));
    WALKERCORE_LOG_INFO("vtable16_calls      : %d", h->vtable16_calls());
    WALKERCORE_LOG_INFO("singleton_global_va : 0x%llX",
                        static_cast<unsigned long long>(h->singleton_global_va()));
    WALKERCORE_LOG_INFO("cam_state_struct_va : 0x%llX",
                        static_cast<unsigned long long>(h->cam_state_struct_va()));
    WALKERCORE_LOG_INFO("vtable_base_va      : 0x%llX",
                        static_cast<unsigned long long>(h->vtable_base_va()));
    WALKERCORE_LOG_INFO("vtable16_va         : 0x%llX",
                        static_cast<unsigned long long>(h->vtable16_va()));
    WALKERCORE_LOG_INFO("--- end camera hijack ---");
}

extern "C" __declspec(dllexport) int walker_camera_hijack_set_enabled(int enabled) {
    using namespace walkercore;
    if (g_camera_hijack == nullptr) return 0;
    g_camera_hijack->SetEnabled(enabled != 0);
    return 1;
}

extern "C" __declspec(dllexport) int walker_camera_hijack_supported(void) {
    using namespace walkercore;
    return g_camera_hijack != nullptr && g_camera_hijack->IsInstalled() ? 1 : 0;
}

extern "C" __declspec(dllexport) int walker_camera_hijack_install(void) {
    using namespace walkercore;
    if (g_camera_hijack == nullptr) return 0;
    return g_camera_hijack->Install() ? 1 : 0;
}

extern "C" __declspec(dllexport) void walker_camera_hijack_uninstall(void) {
    using namespace walkercore;
    if (g_camera_hijack == nullptr) return;
    g_camera_hijack->Uninstall();
}

extern "C" __declspec(dllexport) int
walker_game_free_camera_shim_supported(void) {
    using namespace walkercore;
    return g_game_free_camera_shim != nullptr &&
           g_game_free_camera_shim->isSupported() ? 1 : 0;
}

extern "C" __declspec(dllexport) void
walker_game_free_camera_shim_on_enter_walking(void) {
    using namespace walkercore;
    if (g_game_free_camera_shim == nullptr) return;
    g_game_free_camera_shim->OnEnterWalking();
}

extern "C" __declspec(dllexport) void
walker_game_free_camera_shim_on_exit_walking(void) {
    using namespace walkercore;
    if (g_game_free_camera_shim == nullptr) return;
    g_game_free_camera_shim->OnExitWalking();
}

extern "C" __declspec(dllexport) int
walker_game_free_camera_shim_toggle_now(void) {
    using namespace walkercore;
    if (g_game_free_camera_shim == nullptr) return 0;
    return g_game_free_camera_shim->TriggerToggle();
}

extern "C" __declspec(dllexport) void
walker_game_free_camera_shim_set_enabled(int enabled) {
    using namespace walkercore;
    if (g_game_free_camera_shim == nullptr) return;
    g_game_free_camera_shim->SetEnabled(enabled != 0);
}

// --- M2 dev-camera cvar patcher ----------------------------------------

extern "C" __declspec(dllexport) int
walker_dev_camera_cvar_patcher_supported(void) {
    using namespace walkercore;
    if (g_dev_cvar_patcher == nullptr) return 0;
    return g_dev_cvar_patcher->last_result().cvar_name_va != 0 ? 1 : 0;
}

extern "C" __declspec(dllexport) int
walker_dev_camera_cvar_patcher_run_once(void) {
    using namespace walkercore;
    if (g_dev_cvar_patcher == nullptr) return 0;
    const DevCameraCvarPatchResult r = g_dev_cvar_patcher->PatchOnce();
    return r.patched ? 2 : (r.skipped ? 1 : 0);
}

extern "C" __declspec(dllexport) void
walker_dump_dev_camera_cvar_patcher(void) {
    using namespace walkercore;
    WALKERCORE_LOG_INFO("--- WalkerCore dev-camera cvar patcher ---");
    if (g_dev_cvar_patcher == nullptr) {
        WALKERCORE_LOG_INFO("patcher             : (not initialized)");
        WALKERCORE_LOG_INFO("--- end dev-camera cvar patcher ---");
        return;
    }
    const DevCameraCvarPatcher *p = g_dev_cvar_patcher;
    const DevCameraCvarPatchResult &r = p->last_result();
    WALKERCORE_LOG_INFO("enabled             : %s",
                        p->isEnabled() ? "true" : "false");
    WALKERCORE_LOG_INFO("runs                : %d", p->runs());
    WALKERCORE_LOG_INFO("patches             : %d", p->patches());
    WALKERCORE_LOG_INFO("skips_already       : %d", p->skips_already_patched());
    WALKERCORE_LOG_INFO("skips_disabled      : %d", p->skips_disabled());
    WALKERCORE_LOG_INFO("skips_no_module     : %d", p->skips_no_module());
    WALKERCORE_LOG_INFO("skips_sig_mismatch  : %d", p->skips_signature_mismatch());
    WALKERCORE_LOG_INFO("skips_value_unread  : %d", p->skips_value_unreadable());
    WALKERCORE_LOG_INFO("skips_protect_fail  : %d", p->skips_protect_failed());
    WALKERCORE_LOG_INFO(
        "cvar name VA        : 0x%llX (RVA 0x%llX)",
        static_cast<unsigned long long>(r.cvar_name_va),
        static_cast<unsigned long long>(p->name_rva()));
    WALKERCORE_LOG_INFO(
        "cvar value VA       : 0x%llX",
        static_cast<unsigned long long>(r.cvar_value_va));
    WALKERCORE_LOG_INFO("cvar value before   : '%.24s'", r.cvar_value_before);
    WALKERCORE_LOG_INFO("cvar value after    : '%.24s'", r.cvar_value_after);
    WALKERCORE_LOG_INFO(
        "last outcome        : %s%s",
        r.patched ? "patched" : (r.skipped ? "skipped" : "no-op"),
        r.skipped ? " (see counters above)" : "");
    WALKERCORE_LOG_INFO("--- end dev-camera cvar patcher ---");
}

// --- M3.5 PxScene runtime hook ----------------------------------------

extern "C" __declspec(dllexport) int walker_pxscene_supported(void) {
    using namespace walkercore;
    if (g_px_scene_hook == nullptr) {
        return 0;
    }
    return g_px_scene_hook->target_reachable() ? 1 : 0;
}

extern "C" __declspec(dllexport) uint64_t walker_pxscene_instance(void) {
    using namespace walkercore;
    if (g_px_scene_hook == nullptr) {
        return 0;
    }
    return g_px_scene_hook->captured_instance();
}

extern "C" __declspec(dllexport) void walker_dump_pxscene(void) {
    using namespace walkercore;

    WALKERCORE_LOG_INFO("--- WalkerCore PxScene hook ---");
    if (g_px_scene_hook == nullptr) {
        WALKERCORE_LOG_INFO("hook                : (not initialized)");
        WALKERCORE_LOG_INFO("--- end PxScene hook ---");
        return;
    }
    const PxSceneHook *h = g_px_scene_hook;
    WALKERCORE_LOG_INFO("installed           : %s",
                        h->isInstalled() ? "true" : "false");
    WALKERCORE_LOG_INFO("target reachable    : %s",
                        h->target_reachable() ? "true" : "false");
    WALKERCORE_LOG_INFO(
        "target VA           : 0x%llX (RVA 0x%llX)",
        static_cast<unsigned long long>(PxSceneHook::kAddActorVa),
        static_cast<unsigned long long>(
            PxSceneHook::kAddActorVa - PxSceneHook::kAmtrucksImageBase));
    WALKERCORE_LOG_INFO(
        "captured instance   : 0x%llX",
        static_cast<unsigned long long>(h->captured_instance()));
    WALKERCORE_LOG_INFO("addActor calls      : %d", h->add_actor_calls());
    WALKERCORE_LOG_INFO("--- end PxScene hook ---");
}

// --- M11 ImGui overlay -------------------------------------------------

extern "C" __declspec(dllexport) int walker_imgui_overlay_supported(void) {
    using namespace walkercore;
    if (g_imgui_overlay == nullptr) {
        return 0;
    }
    return g_imgui_overlay->isSupported() ? 1 : 0;
}

extern "C" __declspec(dllexport) void
walker_imgui_overlay_set_enabled(int enabled) {
    using namespace walkercore;
    if (g_imgui_overlay == nullptr) return;
    g_imgui_overlay->SetEnabled(enabled != 0);
}

extern "C" __declspec(dllexport) void walker_dump_imgui_overlay(void) {
    using namespace walkercore;

    auto bool_str = [](bool b) -> const char * { return b ? "true" : "false"; };

    WALKERCORE_LOG_INFO("--- WalkerCore ImGui overlay ---");
    if (g_imgui_overlay == nullptr) {
        WALKERCORE_LOG_INFO("overlay             : (not initialized)");
        WALKERCORE_LOG_INFO("--- end ImGui overlay ---");
        return;
    }
    const ImGuiOverlay *o = g_imgui_overlay;
    WALKERCORE_LOG_INFO("enabled             : %s",
                        bool_str(o->isEnabled()));
    WALKERCORE_LOG_INFO("supported           : %s",
                        bool_str(o->isSupported()));
    WALKERCORE_LOG_INFO("walks_entered       : %d", o->walks_entered());
    WALKERCORE_LOG_INFO("walks_exited        : %d", o->walks_exited());
    WALKERCORE_LOG_INFO("imgui version       : %s", o->ImGuiVersionString());
    WALKERCORE_LOG_INFO("frames_rendered     : %d", o->frames_rendered());
    WALKERCORE_LOG_INFO("--- end ImGui overlay ---");
}

extern "C" __declspec(dllexport) const char *walker_imgui_overlay_version(void) {
    return walkercore::ImGuiOverlay::ImGuiVersionString();
}

extern "C" __declspec(dllexport) unsigned long long
walker_imgui_overlay_frames_rendered(void) {
    return static_cast<unsigned long long>(
        walkercore::ImGuiOverlay::TestRenderCount());
}

extern "C" __declspec(dllexport) void walker_shutdown_imgui_overlay(void) {
    walkercore::ShutdownImGuiOverlay();
}

extern "C" __declspec(dllexport) int walker_on_d3d11_device_captured(
    void *device, void *context, void *hwnd) {
    using namespace walkercore;
    OnD3D11DeviceCaptured(device, context, hwnd);
    return (g_imgui_overlay != nullptr && g_imgui_overlay->isSupported()) ? 1 : 0;
}

extern "C" __declspec(dllexport) void walker_on_d3d11_device_lost(void) {
    walkercore::OnD3D11DeviceLost();
}

extern "C" __declspec(dllexport) void walker_render_imgui_frame_if_enabled(void) {
    walkercore::RenderImGuiFrameIfEnabled();
}

// Build metadata + ABI surface summary. Pulls the values from
// the generated BuildInfo.h (version, phase, build hash, build
// date) plus the runtime ImGui version + the number of
// __declspec(dllexport) exports we ship. The test build pins
// the export count to a literal, so a regression that adds or
// removes a 37th export breaks the ABI test before it breaks
// the field; this dump surfaces the same number to deploy-side
// analysts so the deployed build can be matched against the
// commit it claims to be.
extern "C" __declspec(dllexport) void walker_dump_build_info(void) {
    using namespace walkercore;

    // The ABI export count is the same set test_abi_contract
    // walks. We duplicate the list here to keep the dump
    // self-contained — the test build and the DLL build both
    // compile DiagnosticDump.cpp with the same kExports[]
    // constant (modulo MSVC vs MinGW __declspec). When the
    // counts diverge the ABI test catches it first; this
    // function reports the running build's number.
    constexpr const char *kExportedNames[] = {
        "DllMain", "scs_telemetry_init", "scs_telemetry_shutdown",
        "walkercore_main", "walker_dump_state", "walker_dump_settings",
        "walker_dump_menu", "walker_dump_head_tracker",
        "walker_dump_shadow", "walker_dump_flashlight",
        "walker_dump_door", "walker_dump_fsm", "walker_dump_version",
        "walker_dump_input", "walker_dump_physx", "walker_dump_knockout",
        "walker_dump_per_truck_door", "walker_dump_coupling",
        "walker_dump_game_free_camera_shim",
        "walker_game_free_camera_shim_supported",
        "walker_game_free_camera_shim_on_enter_walking",
        "walker_game_free_camera_shim_on_exit_walking",
        "walker_game_free_camera_shim_toggle_now",
        "walker_game_free_camera_shim_set_enabled",
        "walker_dump_dev_camera_cvar_patcher",
        "walker_dev_camera_cvar_patcher_supported",
        "walker_dev_camera_cvar_patcher_run_once",
        "walker_pxscene_supported", "walker_pxscene_instance",
        "walker_dump_pxscene", "walker_imgui_overlay_supported",
        "walker_imgui_overlay_set_enabled", "walker_dump_imgui_overlay",
        "walker_imgui_overlay_version", "walker_imgui_overlay_frames_rendered",
        "walker_shutdown_imgui_overlay", "walker_on_d3d11_device_captured",
        "walker_on_d3d11_device_lost", "walker_render_imgui_frame_if_enabled",
        "walker_dump_build_info",
        "walker_dump_camera_hijack", "walker_camera_hijack_set_enabled",
        "walker_camera_hijack_supported", "walker_camera_hijack_install",
        "walker_camera_hijack_uninstall"};
    constexpr size_t kExportCount =
        sizeof(kExportedNames) / sizeof(kExportedNames[0]);

    WALKERCORE_LOG_INFO("--- WalkerCore build info ---");
    WALKERCORE_LOG_INFO("version         : %d.%d.%d-%s",
                        WALKERCORE_VERSION_MAJOR, WALKERCORE_VERSION_MINOR,
                        WALKERCORE_VERSION_PATCH, WALKERCORE_VERSION_PHASE);
    WALKERCORE_LOG_INFO("build hash      : %s", WALKERCORE_BUILD_HASH);
    WALKERCORE_LOG_INFO("build date      : %s", WALKERCORE_BUILD_ISO);
    WALKERCORE_LOG_INFO("imgui version   : %s",
                        ImGuiOverlay::ImGuiVersionString());
    WALKERCORE_LOG_INFO("abi exports     : %zu", kExportCount);
    WALKERCORE_LOG_INFO("--- end build info ---");
}