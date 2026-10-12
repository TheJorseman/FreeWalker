// WalkerCore — DLL entry point and SCS telemetry plugin exports.
//
// M0 scope:
//  - DllMain that just initializes the logging facade with no sink (we log
//    via OutputDebugStringW until scs_telemetry_init hands us the SCS log).
//  - scs_telemetry_init that negotiates the API version (we accept 1.00
//    and 1.01), captures the SCS log sink, logs that we are loaded, and
//    reports the negotiated version.
//  - scs_telemetry_shutdown that logs clean shutdown.
//
// What M0 does NOT do:
//  - Register channels or events (M1).
//  - Touch the game's camera, input, or D3D11 (M2+).
//  - Read truck state (M1).
//
// ABI note:
//  The SDK struct `scs_telemetry_init_params_t` is the common ancestor;
//  v100 and v101 inherit from it. We cast through a pointer rather than
//  reinterpret_cast because the structs are layout-checked by the SDK
//  via scs_check_size macros.

#include <windows.h>

#include <cstdio>
#include <string>

#include "walkercore/BuildInfo.h"
#include "walkercore/CameraState.h"
#include "walkercore/D3D11Hook.h"
#include "walkercore/D3D12Hook.h"
#include "walkercore/DiagnosticAccess.h"
#include "walkercore/FrameLoop.h"
#include "walkercore/FuelPump.h"
#include "walkercore/GameVersion.h"
#include "walkercore/InputHook.h"
#include "walkercore/InputState.h"
#include "walkercore/Knockout.h"
#include "walkercore/Log.h"
#include "walkercore/PerFrame.h"
#include "walkercore/PerFrameThread.h"
#include "walkercore/PhysXAdapter.h"
#include "walkercore/PlayerState.h"
#include "walkercore/PolishProviders.h"
#include "walkercore/ScsTelemetryProvider.h"
#include "walkercore/TrailerCoupling.h"
#include "walkercore/SettingsMenu.h"
#include "walkercore/VersionTable.h"
#include "walkercore/WalkerFsm.h"
#include "walkercore/WorldState.h"

#include "scssdk.h"
#include "scssdk_telemetry.h"

namespace {

// Pointer to the SCS-provided log sink. Stored process-wide; only the main
// thread touches it. Set in scs_telemetry_init, cleared in
// scs_telemetry_shutdown. Typed using our SDK-free alias so this file
// stays the only place that bridges between walkercore's facade and the
// SDK's scs_log_t.
walkercore_log_fn_t g_game_log = nullptr;

// String form of the negotiated API version, cached for the F12 diagnostic
// dump in M1+.
const char *g_negotiated_version = "none";

// SCS telemetry provider. Owns the TruckState snapshot and the channel
// registration lifecycle. Lives for the entire time between
// scs_telemetry_init and scs_telemetry_shutdown.
//
// Construction is on first init; we don't pre-allocate because the SCS
// loader runs DllMain with no SDK context and we don't want to touch
// anything until we have it.
walkercore::ScsTelemetryProvider *g_telemetry = nullptr;

// Per-frame workflow. Constructed alongside the telemetry provider;
// the D3D11 hook (M2+) drives RunOnce() each frame.
walkercore::FrameLoop *g_frame_loop = nullptr;

// M5: state machine for enter/exit transitions. Reads FrameLoop + telemetry
// to decide whether the player is in/out of the cabin and tweens the camera
// during transitions.
walkercore::WalkerFsm *g_fsm = nullptr;

// M3 stub: collision provider. The real adapter lives in src/provider/;
// this is the wire-up point. Until the real adapter lands, sphere sweeps
// return "no hit" (the player passes through geometry, with one warning).
walkercore::PhysXAdapter *g_world = nullptr;

// M4 input. The hook is installed once we have a game window (M2 lander);
// until then, g_input stays null and the FrameLoop no-ops on input phases.
walkercore::InputHook *g_input = nullptr;

// M6: settings store. Reads WalkerCore.ini on init, polls for changes,
// pushes into FrameLoop / InputHook / WalkerFsm.
walkercore::SettingsStore *g_settings = nullptr;

// Last FSM state for which the per-frame conductor encoded a label into
// the diagnostic-dump canonical buffer (see DiagnosticDump.cpp's
// g_fsm_label_buf + walker_fsm_state_label()'s readback path). Used to
// skip the re-encode in steady state — Driving or Walking with no
// state change produces an identical label every frame, so the
// EncodeFsmLabel + SetFsmLabel work is wasted. The per-frame
// conductor drives this; transitions force a re-encode every frame
// regardless because the tween progress changes.
walkercore::WalkerFsmState g_prev_encoded_fsm_state =
    walkercore::WalkerFsmState::Driving;

// M2 lander: D3D11 Present hook. Installed at init time, drives every
// frame via walker_per_frame_tick. The D3D11 hook fires when the game
// uses D3D11; ATS 1.61.x uses D3D12 (see g_d3d12_hook below), in which
// case this hook's D3D11CreateDeviceAndSwapChain trampoline is never
// called and g_d3d11_hook->Install() returns false.
walkercore::D3D11Hook *g_d3d11_hook = nullptr;

// M2 lander (D3D12 path): DXGI factory + Present hook for D3D12 games.
// Installed in parallel with the D3D11 hook. Whichever path fires first
// (the one matching the game's D3D runtime) becomes the per-frame
// owner. Both hooks write to the same g_frame_loop, so there is no
// double-tick risk: only one will successfully capture a swap chain.
walkercore::D3D12Hook *g_d3d12_hook = nullptr;

// M2 fallback lander: background per-frame ticker. Activated when the
// game's DXGI/D3D swap chain was created BEFORE WalkerCore was loaded
// (which is the normal case: SCS plugins load AFTER the game's graphics
// subsystem has already initialized, so neither D3D11 nor D3D12 Present
// hooks fire). The background ticker drives walker_per_frame_tick at
// ~60 Hz and shares the per-frame lock with the D3D Present hooks via
// walker_try_enter_per_frame_tick / walker_exit_per_frame_tick —
// whichever tick site acquires the lock first runs the tick; the other
// skips this frame.
walkercore::PerFrameThread *g_per_frame_thread = nullptr;

// M8: FuelProvider owns the local fill-rate math + station availability.
// ScsTelemetryProvider gives us truck.fuel / truck.adblue amounts; we
// sync them into FuelProvider each frame. The dump and the F-interact
// handler both read FuelProvider.
walkercore::FuelProvider *g_fuel = nullptr;

// M10: KnockoutProvider tracks impact velocity changes and flips the
// player's Mode to KnockedOut for a recovery period. The Tick mutates
// the FrameLoop's PlayerState in place. We run it before the FrameLoop's
// RunOnce so PhaseAnimatePlayer sees the knocked-out mode and skips
// the head bob + crouch blend.
walkercore::KnockoutProvider *g_knockout = nullptr;

// M9: CouplingProvider walks the player through the 5-step trailer-attach
// flow (approach king pin → lower landing gear → connect air lines →
// raise landing gear → complete). Currently a stub: uses placeholder
// truck-local waypoints, doesn't talk to the SCS trailer-attach API.
// Wired in here so the stub-warn fires at runtime (user-visible signal
// that M9 is not yet shipped) and the M9 lander only has to swap the
// implementation, not re-wire the per-frame conductor.
walkercore::CouplingProvider *g_coupling = nullptr;

// M11: in-game settings menu. The renderer is a no-op until the ImGui
// lander lands; the edit API (SetSliderValue / SetBoolValue /
// SetKeyBinding) and the dirty-flag flow are real. Open() / Close() /
// Toggle() are wired to the Ctrl+F10 rising edge from the input hook.
walkercore::SettingsMenu *g_settings_menu = nullptr;

// M7: FlashlightProvider tracks on/off state. Toggle is wired to the G
// key and the right mouse button rising edges while the FSM is in
// Walking mode. The actual point-light injection is M7 lander work;
// until then this only writes the state, which the dump surfaces.
walkercore::FlashlightProvider *g_flashlight = nullptr;

// M7: DoorAnimProvider tweens the cabin door's swing angle in
// lockstep with the FSM transitions. We tick it each frame and
// trigger AnimateTo when the FSM state changes (Driving → Walking
// opens the door, Walking → Driving closes it). The angle is
// exposed via the dump and consumed by renderers that want to
// drive a door mesh in M7 lander.
walkercore::DoorAnimProvider *g_door_anim = nullptr;

// M7: HeadTrackerProvider. The default config leaves all sources off
// so the standard FrameLoop mouse-look path isn't double-counted. The
// M7 lander enables TrackIR or opentrack and feeds real samples
// through FeedSample. Today this just keeps the provider live so the
// dump surfaces its state — when the user enables a source in INI,
// the smoothed offsets will start flowing.
walkercore::HeadTrackerProvider *g_head_tracker = nullptr;

// M7: ShadowProvider. Pure function over PlayerState — no Install(),
// no Tick. The provider is constructed once and the dump computes
// the shadow snapshot from the FrameLoop's player state at dump
// time. The real game-side sun direction is M7 lander work; until
// then the default overhead-45° direction is used.
walkercore::ShadowProvider *g_shadow = nullptr;

// Cached FSM state from the previous frame. Used to detect transitions
// in walker_per_frame_tick and fire door animations on the rising edge.
// Initialised to Driving — the FSM's default state — so the first frame
// after init does NOT count as a transition.
walkercore::WalkerFsmState g_prev_fsm_state = walkercore::WalkerFsmState::Driving;

}  // namespace

// -----------------------------------------------------------------------------
// Per-frame tick (C ABI exported).
// -----------------------------------------------------------------------------

extern "C" void walker_per_frame_tick(double dt_seconds) {
  if (g_frame_loop == nullptr) return;

  // Phase A: sample input. We do this BEFORE the FSM runs so the FSM can
  // observe rising edges (F10 toggle-walk, F interact) on the same frame
  // the user pressed them, and so the FSM's camera override takes effect
  // on the same frame it is produced (instead of one frame later).
  //
  // SampleInput is the canonical frame sample: it copies the InputHook's
  // accumulated snapshot into last_input_ and clears the source's edge
  // bits so the next frame's events accumulate fresh. The flag it sets
  // (input_sampled_this_frame_) is what stops RunOnce() below from
  // re-sampling the now edge-cleared source and wiping the rising edge
  // the downstream providers (fuel / coupling / flashlight) need to
  // see on the same frame. Without the flag, the cached `in` reference
  // below would alias last_input_ after RunOnce re-wrote it with edge-
  // cleared data, and the F-interact handler in the fuel provider, the
  // coupling step advance, and the flashlight toggle would all silently
  // no-op on the F press frame.
  g_frame_loop->SampleInput();

  // Cache the input snapshot for the rest of the conductor. The
  // sample-into-last_input_ step above makes `lastInput()` stable
  // for the duration of this frame; we read it 5 more times below
  // (F12 dump, settings menu toggle, FSM toggle-walk, fuel interact,
  // coupling interact, flashlight toggle) and each call is a
  // cross-translation-unit function call that the compiler cannot
  // inline. Lifting one const ref at the top saves 5 redundant
  // calls per frame.
  const walkercore::InputState &in = g_frame_loop->lastInput();

  // Cache the F interact rising edge once. The F key drives the
  // F-interact handler in the FSM (Phase B), the fuel provider's
  // start/stop fueling toggle (Phase E), and the coupling step
  // advance (Phase E5). Reading it from the same `in` snapshot
  // three times would re-call the cross-translation-unit
  // `IsKeyPressed` accessor each time: with
  // -fvisibility-inlines-hidden, every inline function in InputState.h
  // gets an out-of-line weak-symbol copy the linker can resolve
  // from any TU, so the call is not folded into the caller. Lifting
  // one bool at the top saves 2 redundant calls per frame in Walking
  // mode.
  const bool interact_pressed =
      walkercore::IsKeyPressed(in, walkercore::WalkercoreKey::Interact);

  // Cache the truck + player pointers once. Phase B passes them to the
  // FSM, Phase C0 (knockout) reads player, Phase E (fuel) and Phase E5
  // (coupling) read both directly. Without -fvisibility-inlines-hidden
  // both `g_telemetry->state()` and `g_frame_loop->player()` would be
  // inlined in the caller's TU; with the hidden-visibility flag the
  // compiler emits out-of-line weak copies anyway and every call is a
  // real function call. The pointers are stable for the lifetime of
  // the providers, so hoisting them is safe.
  //
  // The fallback TruckState is a function-static default-constructed
  // instance used when the telemetry provider is not yet bound; its
  // has_placement flag stays false so PlacePlayerAtSeat /
  // PlayerCanEnterDoor / ComputeTransitionTarget no-op correctly. It
  // used to live as a static local inside the Phase B block, but
  // promoting it to function scope is required now that Phase E and
  // Phase E5 share the same truck_ptr cache.
  static const walkercore::TruckState kEmptyTruckState{};
  const walkercore::TruckState *truck_ptr =
      (g_telemetry != nullptr) ? &g_telemetry->state() : &kEmptyTruckState;
  walkercore::PlayerState *player_ptr = &g_frame_loop->player();

  // M5 Lander hookup: when the SCS configuration event has delivered
  // per-truck cabin + head anchors, rebuild the door config from
  // them and push to the FSM. This runs BEFORE Phase B (FSM
  // update) so a TransitionOut Driving → Walking triggered this
  // frame uses the per-truck door pivot, not the INI default.
  //
  // We only push when the anchors are both present (the
  // configuration event delivers them as a single unit; partial
  // delivery would yield a degenerate config). We do not gate on
  // player state — the FSM holds the door config across Driving
  // and Walking, and a per-truck swap during Driving is the
  // expected case (player buys a new truck, drives out, the door
  // pivot updates mid-drive before the next exit).
  //
  // The push is cheap: one TruckDoorConfig copy per frame at
  // most, only when the SDK delivers new anchors (the
  // configuration event fires once per truck swap, not per
  // frame). We do not diff against the FSM's current config —
  // SetDoorConfig is a struct copy, not a per-field comparison,
  // and the configuration event is rare enough that the cost is
  // invisible.
  if (g_fsm != nullptr && truck_ptr->has_cabin_position &&
      truck_ptr->has_head_position) {
    const walkercore::TruckDoorConfig per_truck =
        walkercore::TruckDoorConfigFromTruckState(*truck_ptr);
    g_fsm->SetDoorConfig(per_truck);
  }

  // Diagnostic dump on F12 rising edge. Lives in walker_per_frame_tick so
  // it's reachable regardless of walk mode (useful while driving too) and
  // is independent of the FSM. The InputHook clears the rising edge in
  // the next Sample() call — but we read it on the same frame we sampled,
  // so the edge is visible here exactly once per press.
  if (walkercore::IsKeyPressed(in, walkercore::WalkercoreKey::DumpState)) {
    walker_dump_state();
  }

  // Ctrl+F10 opens / closes the in-game settings menu. The rising edge
  // is set by the keyboard callback when F10 is pressed with the crouch
  // key (default Ctrl) already held. Independent of walk mode so the
  // menu is reachable from Driving too.
  if (g_settings_menu != nullptr &&
      walkercore::IsKeyPressed(in, walkercore::WalkercoreKey::ToggleMenu)) {
    g_settings_menu->Toggle();
  }

  // Phase B: drive the WalkerFsm. It reads player + truck + input, may
  // change state, may write to the FrameLoop's player_ via the bound
  // player_target_ pointer (snap to door / seat at transition start),
  // and may write a camera override for THIS frame's RunOnce to consume.
  //
  // Cache the FSM state once here so the gates in Phase E / E5 / F /
  // G can read the same value without going through g_fsm->state() —
  // each cross-translation-unit call is a real function call that
  // the compiler can't inline. The state is stable between the
  // Update() above and the next per-frame tick, so this single read
  // is sufficient.
  walkercore::WalkerFsmState fsm_state = walkercore::WalkerFsmState::Driving;
  float transition_t = 0.0f;
  if (g_fsm != nullptr) {
    // Skip the per-frame copy that a struct-by-value WalkerFsmInputs
    // would require. UpdateFromPointers takes the truck and player
    // addresses directly; the helpers inside Update (PlacePlayerExited,
    // PlayerCanEnterDoor, ComputeTransitionTarget) all read through
    // the same pointers, so the conductor's stack work is now two
    // pointer loads + two bools.
    const bool toggle_walk_pressed =
        walkercore::IsKeyPressed(in, walkercore::WalkercoreKey::ToggleWalk);

    walkercore::CameraState target;
    g_fsm->UpdateFromPointers(truck_ptr, player_ptr, interact_pressed,
                              toggle_walk_pressed, dt_seconds, &target);
    if (target.valid) {
      g_frame_loop->OverrideCamera(target);
    }

    fsm_state = g_fsm->state();
    transition_t = g_fsm->transitionProgress();
  }

  // Phase B' (M11 follow-up): KnockoutProvider baseline re-prime on FSM
  // entry into Walking. The M11 velocity cap (MovementConfig::
  // vertical_velocity_max_mps = 9.81 m/s, applied in FrameLoop::
  // PhaseIntegrateMovement) bounds per-frame vertical motion to ≤ 0.164
  // m and combined motion to ≤ 10.79 m/s — well below the Knockout
  // threshold (now 25 m/s after the M11 tuning). Yet the in-game DLL
  // at build 3fbc149 still showed spurious 12-38 m/s KO triggers
  // within 30-575 ms of every Walking re-entry on ATS 1.61.3.1s (see
  // commit c90916a's game.log.txt evidence in the comments). The cap
  // bounds the integrated motion the FrameLoop produces; what's not
  // covered is the elapsed-time delta the KnockoutProvider's per-
  // frame `last_position_` baseline has accumulated since the last
  // Walking tick — the baseline references a coordinate from the prior
  // session, the cap-driven motion in frame 0 of the new session is
  // tiny, but the FOOTPRINT of the simulated ground (where vy was
  // preserved at large magnitude, then re-primed against a door
  // anchor via the teleport_reprime branch) means the very first
  // velocity check on the new last_position_ baseline covers a 0.6 s
  // TransitionOut tween plus the snap — a window where the FrameLoop
  // is bailing and the player's position is held at the door anchor
  // while the KnockoutProvider's internal baseline hasn't moved.
  //
  // We re-prime explicitly here so the next Tick starts a fresh
  // baseline at the post-transition coordinate. The previous FSM
  // state is tracked locally (file-local statics would also work but
  // we don't want to risk contamination across reload).
  {
    static walkercore::WalkerFsmState prev_fsm_for_knockout_prime =
        walkercore::WalkerFsmState::Driving;
    if (g_knockout != nullptr &&
        fsm_state == walkercore::WalkerFsmState::Walking &&
        prev_fsm_for_knockout_prime != walkercore::WalkerFsmState::Walking) {
      // We just entered Walking from any other state. Reset the
      // KnockoutProvider's per-frame tracking baseline to wherever the
      // player currently is — that's the post-transition (door-anchor
      // or in-cabin) coordinate, with the FrameLoop cap now active
      // and ready to bound motion.
      g_knockout->Prime(player_ptr->feet_position);
      WALKERCORE_LOG_INFO(
          "KnockoutProvider: baseline re-primed on Walking entry "
          "(pos=(%.2f, %.2f, %.2f), prev_state=%s)",
          player_ptr->feet_position.x,
          player_ptr->feet_position.y,
          player_ptr->feet_position.z,
          walkercore::WalkerFsmStateName(prev_fsm_for_knockout_prime));
    }
    prev_fsm_for_knockout_prime = fsm_state;
  }
  // Steady-state skip: in Driving or Walking, the label is the same
  // string every frame, so re-running EncodeFsmLabel + SetFsmLabel
  // every frame is wasted work (snprintf + strncpy each frame at
  // 60 Hz). The helper decides when to actually re-encode:
  //   - state changed → re-encode;
  //   - or current state is in a transition (t changes per frame).
  // The canonical buffer lives in DiagnosticDump.cpp; reading it back
  // happens via walker_fsm_state_label() (the exported ABI) and via
  // walker_dump_state() (the F12 dump), both of which now read from
  // the same canonical buffer (no more parallel buffers).
  if (walkercore::diagnostic_internal::FsmLabelNeedsReencode(
          g_prev_encoded_fsm_state, fsm_state)) {
    char stack_buf[64];
    walkercore::diagnostic_internal::EncodeFsmLabel(
        fsm_state, transition_t, stack_buf, sizeof(stack_buf));
    walkercore::diagnostic_internal::SetFsmLabel(stack_buf);
    g_prev_encoded_fsm_state = fsm_state;
  }

  // Phase C0: M10 knock-out tracker. Runs before RunOnce so the player's
  // mode is up to date when PhaseAnimatePlayer checks it (KnockedOut is
  // not in the "moving" set, so head bob and crouch blend skip).
  if (g_knockout != nullptr) {
    g_knockout->Tick(*player_ptr, dt_seconds);
  }

  // Phase C: integrate movement and apply the camera override. The
  // FrameLoop checks the FSM state — only Walking does work; the rest is
  // an input-sample-only pass. We pass dt_seconds explicitly so the
  // FrameLoop uses the same frame delta the renderer just observed,
  // instead of measuring its own wall-clock delta separately.
  g_frame_loop->RunOnce(dt_seconds);

  // Phase D: poll the settings file for live edits (cheap).
  if (g_settings != nullptr) {
    g_settings->CheckFileChanged();
  }

  // Phase E: M8 fuel pump. The provider syncs from telemetry, decides if
  // a station is in range, and advances the fill rate while fueling. The
  // F-interact toggle is handled here so it works in any mode (drive or
  // walk) as long as the player is on foot — the FSM is the authority on
  // whether the player is on foot, so we gate by FSM state == Walking.
  //
  // Pass the cached `*truck_ptr` / `*player_ptr` snapshots to Fuel.Tick
  // (saves two cross-TU accessor calls vs re-reading g_telemetry->state()
  // and g_frame_loop->player() here).
  //
  // IMPORTANT: Tick() OVERWRITES the provider's internal station_ on
  // every call (it recomputes the in-range check from the new truck /
  // player positions). The conductor must therefore call Tick() BEFORE
  // reading station() for the same frame — otherwise the cached
  // FuelStationState reference is bound to the PREVIOUS frame's station_,
  // and a player who walks INTO the station and presses F on the same
  // frame would see station.available == false from last frame's Tick
  // and StartFueling would not fire. The previous version of this
  // block read station() first (one-frame stale) — pinning the fix in
  // TestFuelStationReadAfterTickReflectsCurrentPlayerPosition.
  if (g_fuel != nullptr && g_fsm != nullptr &&
      fsm_state == walkercore::WalkerFsmState::Walking &&
      g_telemetry != nullptr) {
    g_fuel->Tick(*truck_ptr, *player_ptr, dt_seconds);
    if (interact_pressed) {
      // Read the station state once, AFTER Tick(), so the cached
      // reference reflects this frame's recomputation. The previous
      // version read station() BEFORE Tick(), which made the read
      // stale by one frame and could swallow a same-frame
      // press-F-when-entering-station input.
      const walkercore::FuelStationState &station = g_fuel->station();
      if (g_fuel->isFueling()) {
        g_fuel->StopFueling();
        WALKERCORE_LOG_INFO("fuel: stopped fueling");
      } else if (station.available) {
        g_fuel->StartFueling();
        WALKERCORE_LOG_INFO("fuel: started fueling at station (distance %.2f m)",
                            static_cast<double>(station.distance_m));
      }
    }
  }

  // Phase E5: M9 trailer coupling. The provider's Tick reads the player's
  // distance to the active step's waypoint and advances (or cancels) the
  // 5-step coupling flow. The stub uses placeholder truck-local
  // waypoints; the first Tick fires the stub-warn so the user sees the
  // M9 lander is not yet shipped. Only Tick on foot — the coupling flow
  // is a player-on-foot minigame.
  //
  // Use the cached interact_pressed + cache_truck_ptr + cached
  // player_ptr for the same reason Phase E does — they are the
  // snapshots the FSM, knockout, and fuel phases already saw on this
  // frame, so the coupling provider sees the same state the rest of
  // the conductor saw.
  if (g_coupling != nullptr && g_fsm != nullptr && g_telemetry != nullptr &&
      fsm_state == walkercore::WalkerFsmState::Walking) {
    g_coupling->Tick(*truck_ptr, *player_ptr, dt_seconds, interact_pressed);
  }

  // Phase F: M7 flashlight. Toggle on the G key rising edge or the RMB
  // rising edge. Both bindings map to the same toggle in M7's current
  // scope; once the real point-light injection lands, G can become
  // "hold to shine" while RMB keeps the tap-toggle semantic. Gated on
  // Walking to match the fuel provider — the flashlight is a walker
  // feature, not a driver feature.
  if (g_flashlight != nullptr && g_fsm != nullptr &&
      fsm_state == walkercore::WalkerFsmState::Walking) {
    if (walkercore::IsKeyPressed(in, walkercore::WalkercoreKey::Flashlight) ||
        walkercore::IsKeyPressed(in, walkercore::WalkercoreKey::FlashlightToggle)) {
      // Snapshot the pre-toggle state so the log can report the new
      // value without a second cross-TU call to g_flashlight->isOn()
      // after Toggle(). The FlashlightProvider's Toggle() flips the
      // internal on_ bool; reading it before the call (and reporting
      // !was_on in the log) is one cross-TU call instead of two, and
      // also makes the log message self-documenting about which key
      // the user pressed.
      const bool was_on = g_flashlight->isOn();
      g_flashlight->Toggle();
      WALKERCORE_LOG_INFO("flashlight: toggled %s → %s",
                          was_on ? "ON" : "OFF",
                          was_on ? "OFF" : "ON");
    }
  }

  // Phase G: M7 door animator. Detect FSM state changes; fire AnimateTo
  // on the rising edge of a transition (Driving → not-Driving opens
  // the door, Walking → not-Walking closes it). The transition itself
  // tweens the camera over transition_seconds_, and the door tween
  // happens in parallel over door_open_duration_s. We Tick the animator
  // every frame regardless of state, so a tween started on a previous
  // frame keeps advancing after the transition completes.
  if (g_door_anim != nullptr && g_fsm != nullptr) {
    const walkercore::WalkerFsmState cur = fsm_state;
    if (cur != g_prev_fsm_state) {
      const walkercore::TruckDoorConfig &cfg = g_fsm->doorConfig();
      // Driving → not-Driving: the player is exiting. Open the door.
      // Walking → not-Walking: the player is entering. Close the door.
      if (g_prev_fsm_state == walkercore::WalkerFsmState::Driving &&
          cur != walkercore::WalkerFsmState::Driving) {
        g_door_anim->AnimateTo(cfg.door_max_open_rad,
                                cfg.door_open_duration_s);
        WALKERCORE_LOG_INFO("door anim: opening (target=%.2f rad over %.2f s)",
                            static_cast<double>(cfg.door_max_open_rad),
                            static_cast<double>(cfg.door_open_duration_s));
      } else if (g_prev_fsm_state == walkercore::WalkerFsmState::Walking &&
                 cur != walkercore::WalkerFsmState::Walking) {
        g_door_anim->AnimateTo(0.0f, cfg.door_open_duration_s);
        WALKERCORE_LOG_INFO("door anim: closing (target=0 over %.2f s)",
                            static_cast<double>(cfg.door_open_duration_s));
      }
      g_prev_fsm_state = cur;
    }
    g_door_anim->Tick(dt_seconds);
  }
}

extern "C" const walkercore::CameraState *walker_camera(void) {
  if (g_frame_loop == nullptr) return nullptr;
  return &g_frame_loop->camera();
}

extern "C" const walkercore::PlayerState *walker_player(void) {
  if (g_frame_loop == nullptr) return nullptr;
  return &g_frame_loop->player();
}

extern "C" int walker_should_override_render(void) {
  if (g_fsm == nullptr) return 0;
  return g_fsm->ShouldRenderOverride() ? 1 : 0;
}

extern "C" const char *walker_fsm_state_label(void) {
  // Read from the canonical FSM label buffer maintained by the per-frame
  // conductor (DiagnosticDump.cpp's g_fsm_label_buf via
  // GetFsmLabel()). Previously this function returned a parallel buffer
  // that the per-frame conductor kept in lock-step; that double-buffer
  // was removed as part of the per-frame hot-path optimisation. Both
  // this export and walker_dump_state() read the same buffer now.
  return walkercore::diagnostic_internal::GetFsmLabel();
}

// walker_dump_state lives in DiagnosticDump.cpp (separate translation
// unit) so the test build can include it without dragging in PluginEntry
// and the SCS SDK. PluginEntry wires the global pointers via the
// diagnostic_internal::Set* accessors below.

extern "C" void walker_install_real_collision(void *world_geometry) {
  // M3 lander calls this with a real IWorldGeometry*. We swap the binding
  // on FrameLoop and tear down the stub.
  if (g_frame_loop == nullptr || world_geometry == nullptr) return;
  g_frame_loop->BindWorld(static_cast<walkercore::IWorldGeometry *>(world_geometry));
  WALKERCORE_LOG_INFO("real collision provider installed");
}

extern "C" void walker_set_swapchain_aspect(float aspect) {
  if (g_frame_loop == nullptr) return;
  g_frame_loop->SetAspect(aspect);
}

// SCS SDK entry points. Both must be visible from the DLL's export
// table; MinGW with -fvisibility=hidden requires explicit
// __declspec(dllexport). MSVC picks them up via WalkerCore.def.
extern "C" __declspec(dllexport) SCSAPI_RESULT scs_telemetry_init(
    const scs_u32_t version,
    const scs_telemetry_init_params_t *const params) {
  if (params == nullptr) {
    return SCS_RESULT_invalid_parameter;
  }

  // The SDK uses C++ struct inheritance: the public base struct
  // scs_telemetry_init_params_t is a tiny stub with a marker function, and
  // every concrete version extends it. The concrete layouts are checked
  // at SDK compile time via scs_check_size.
  const scs_telemetry_init_params_v100_t *const v100 =
      reinterpret_cast<const scs_telemetry_init_params_v100_t *>(params);

  if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01) {
    if (v100 != nullptr && v100->common.log != nullptr) {
      v100->common.log(SCS_LOG_TYPE_error,
                       "WalkerCore: requested API version is not supported");
    }
    return SCS_RESULT_unsupported;
  }

  g_game_log = reinterpret_cast<walkercore_log_fn_t>(v100->common.log);
  walkercore_log_init(g_game_log);

  g_negotiated_version = (version == SCS_TELEMETRY_VERSION_1_01) ? "1.01" : "1.00";
  walkercore::diagnostic_internal::SetScsVersion(g_negotiated_version);

  WALKERCORE_LOG_INFO("loaded (build %S, v%d.%d.%d-%s, game=%s id=%s ver=0x%08X)",
                      WALKERCORE_VERSION_STRING,
                      WALKERCORE_VERSION_MAJOR, WALKERCORE_VERSION_MINOR, WALKERCORE_VERSION_PATCH,
                      WALKERCORE_VERSION_PHASE,
                      v100->common.game_name ? v100->common.game_name : "(null)",
                      v100->common.game_id ? v100->common.game_id : "(null)",
                      static_cast<unsigned>(v100->common.game_version));

  WALKERCORE_LOG_INFO("telemetry: SCS API version negotiated = %s",
                      g_negotiated_version);

  // M0.5: identify the game build and look it up. Unknown builds are
  // tolerated — features will be off until we add an entry for them.
  // Adapter-layer extracts the SDK fields and hands the SDK-free
  // GameIdentity constructor POD; domain code never sees scssdk.h.
  const walkercore::GameIdentity id = walkercore::GameIdentity::FromFields(
      v100->common.game_id != nullptr ? v100->common.game_id : "",
      v100->common.game_version);
  // Mirror to the diagnostic layer so walker_dump_version() can
  // surface "did the build match?" without re-running the SDK
  // negotiation. The mirror copy keeps the SDK-free header happy
  // (no string lifetime constraint carries into DiagnosticDump.cpp).
  walkercore::diagnostic_internal::SetIdentity(id.game_id, id.game_version);
  const walkercore::VersionEntry *entry = walkercore::VersionTable::Lookup(id);
  if (entry != nullptr) {
    WALKERCORE_LOG_INFO("game build matched: %s (features=0x%08X)",
                        entry->label, entry->features_enabled);
  } else {
    WALKERCORE_LOG_WARN(
        "game build not in version table: id=%s ver=0x%08X (%u.%u) "
        "- WalkerCore will load but most subsystems will refuse to activate",
        id.game_id, id.game_version, id.major, id.minor);
  }

  // M1: spin up the telemetry provider and register the channels we
  // care about. This is the first time WalkerCore touches the game state.
  g_telemetry = new walkercore::ScsTelemetryProvider();
  walkercore::diagnostic_internal::SetTelemetry(g_telemetry);
  const int registered = g_telemetry->RegisterAll(
      const_cast<void *>(static_cast<const void *>(&v100->register_for_channel)));
  WALKERCORE_LOG_INFO("M1: snapshot provider live (%d channels)",
                     registered);

  // M9 lander: register for the configuration event so we get the
  // truck's hook_position (truck-local fifth-wheel offset) before any
  // channel callback fires. The SDK does not replay this event, so we
  // MUST register here during init. CouplingProvider uses the stored
  // hook_position to compute the world king-pin location; the
  // trailer.world.placement channel registered above gives the trailer
  // pose. Best-effort: a failure here logs a warning and the
  // CouplingProvider falls back to truck-local placeholder waypoints.
  g_telemetry->RegisterEvents(
      const_cast<void *>(static_cast<const void *>(&v100->register_for_event)));

  // M2: frame loop orchestration. The D3D11 hook (M2 lander) calls
  // g_frame_loop->RunOnce() every frame. For M4 the frame loop is fully
  // populated; the D3D11 hook and Input/World providers will be wired in
  // when their respective landers land. Until then, the frame loop runs
  // with no input and no collision — RunOnce is a no-op outside walk mode
  // and inside walk mode it logs and waits.
  g_frame_loop = new walkercore::FrameLoop();
  walkercore::diagnostic_internal::SetFrameLoop(g_frame_loop);

  // M3 (stub): install the world geometry provider. The stub returns
  // "no hit" so the player passes through everything; the one-shot
  // warning is enough for now.
  g_world = new walkercore::PhysXAdapter();
  g_world->Install();
  g_frame_loop->BindWorld(g_world);

  // M4: install the input hook. The hook needs a game window for full
  // mouse capture; for now we install with a null window and the
  // keyboard hook will work, mouse WM_INPUT waits for the M2 lander.
  g_input = new walkercore::InputHook();
  g_input->Install(nullptr);
  g_frame_loop->BindInput(g_input);
  walkercore::diagnostic_internal::SetInput(g_input);

  // M5: state machine for cabin transitions. Bound to FrameLoop so it can
  // override the camera during tweens. The FSM is the SINGLE authority on
  // walk mode (Driving / Walking / TransitionIn / TransitionOut) — the
  // FrameLoop queries it each frame to decide whether to do movement
  // work. We also bind FrameLoop's player_ to the FSM so the FSM can
  // snap the player to door / seat anchors when transitions start.
  g_fsm = new walkercore::WalkerFsm();
  g_frame_loop->BindFsm(g_fsm);
  g_fsm->BindPlayerTarget(&g_frame_loop->player());
  walkercore::diagnostic_internal::SetFsm(g_fsm);

  // M6: settings store. Loads WalkerCore.ini (creates defaults on first
  // run), binds to the live objects, applies once. Polling for reload is
  // wired once per second from the per-frame loop in the M2 lander.
  g_settings = new walkercore::SettingsStore();
  g_settings->BindFrameLoop(g_frame_loop);
  g_settings->BindInput(g_input);
  walkercore::diagnostic_internal::SetSettings(g_settings);
  g_settings->BindFsm(g_fsm);
  const std::string ini_path = walkercore::DefaultSettingsPath();
  if (!g_settings->LoadFromPath(ini_path)) {
    WALKERCORE_LOG_INFO(
        "M6: no existing settings at %s — writing defaults", ini_path.c_str());
    g_settings->SaveToPath(ini_path);
    g_settings->LoadFromPath(ini_path);  // re-apply defaults
  }

  // M8: fuel pump provider. The local tank starts at zero (stale); the
  // first tick that has telemetry syncs it. The fuel/adblue section of
  // the dump reads from this object directly.
  g_fuel = new walkercore::FuelProvider();
  walkercore::diagnostic_internal::SetFuel(g_fuel);

  // M10: knock-out tracker. The provider flips PlayerState.mode to
  // KnockedOut when the player's velocity change exceeds the threshold.
  // We Tick() it BEFORE the FrameLoop RunOnce so PhaseAnimatePlayer sees
  // the updated mode and skips the head-bob + crouch-blend phases while
  // the player is recovering.
  g_knockout = new walkercore::KnockoutProvider();
  walkercore::diagnostic_internal::SetKnockout(g_knockout);

  // M7: flashlight provider. Owns the on/off state. Toggle edges are
  // handled by walker_per_frame_tick; we only own the lifetime here.
  // The point-light injection is M7 lander work — until then this only
  // exposes on/off + a snapshot via the dump.
  g_flashlight = new walkercore::FlashlightProvider();
  walkercore::diagnostic_internal::SetFlashlight(g_flashlight);

  // M9: trailer-coupling provider. Stub uses truck-local waypoint
  // placeholders and does not talk to the SCS trailer-attach API; the
  // first Tick() will fire the stub-warn into the game log so the user
  // sees M9 is not yet shipped. The M9 lander only has to swap the
  // implementation, not re-wire the per-frame conductor.
  g_coupling = new walkercore::CouplingProvider();

  // M7: door animator. Tweens the driver's door swing angle when the
  // FSM transitions between Driving and Walking (door opens on exit,
  // closes on entry). Tick is wired in walker_per_frame_tick; we only
  // own the lifetime here. The skeletal animation is M7 lander work —
  // until then the angle is exposed via the dump and consumed by
  // renderers that want to drive a door mesh.
  g_door_anim = new walkercore::DoorAnimProvider();
  walkercore::diagnostic_internal::SetDoorAnim(g_door_anim);

  // M7: head tracker provider. Owns the smoothed yaw/pitch output of
  // the head-tracking pipeline. Default config disables every source so
  // the FrameLoop's mouse-look path isn't double-counted. The M7 lander
  // enables TrackIR / opentrack and feeds samples; until then this just
  // keeps the provider alive for the dump to surface.
  g_head_tracker = new walkercore::HeadTrackerProvider();
  g_head_tracker->Install();
  walkercore::diagnostic_internal::SetHeadTracker(g_head_tracker);

  // Bind the head tracker to the settings store so ApplyAll() (called
  // on every LoadFromPath / SetCurrent) writes the live INI / menu
  // overrides into the provider. We bind AFTER the load so the loaded
  // values flow in immediately via the next ApplyAll().
  g_settings->BindHeadTracker(g_head_tracker);
  g_settings->ApplyAll();

  // M7: shadow provider. Pure function over PlayerState; no Install /
  // no Tick. The dump calls Compute(player) when both a FrameLoop and
  // a ShadowProvider are wired, so this just keeps the provider live.
  // The real game-side sun direction is M7 lander work; until then
  // the default overhead-45° sun is used.
  g_shadow = new walkercore::ShadowProvider();
  walkercore::diagnostic_internal::SetShadow(g_shadow);

  // M11: in-game settings menu. The renderer is a no-op stub until the
  // ImGui lander lands; the edit cycle (Open → edit → Close → save) is
  // real and exercisable via Ctrl+F10. Bind it to the SettingsStore so
  // SetSliderValue / SetBoolValue / SetKeyBinding write through.
  g_settings_menu = new walkercore::SettingsMenu();
  g_settings_menu->Bind(g_settings);
  walkercore::diagnostic_internal::SetSettingsMenu(g_settings_menu);

  WALKERCORE_LOG_INFO(
      "M2/M4/M5/M6/M8/M10/M11: frame loop + FSM + settings + fuel + knockout + menu live "
      "(collision stub, input hook live, transitions + INI + fuel + knockout + menu ready)");

  // M2 lander: install the D3D11 Present hook. When the game creates its
  // device + swap chain, our wrapper captures them, hooks Present, and
  // each frame calls walker_per_frame_tick(dt).
  g_d3d11_hook = new walkercore::D3D11Hook();
  g_d3d11_hook->BindFrameLoop(g_frame_loop);
  if (g_d3d11_hook->Install()) {
    WALKERCORE_LOG_INFO("M2 lander: D3D11 hook active");
    walkercore::diagnostic_internal::SetD3D11(g_d3d11_hook);
  } else {
    WALKERCORE_LOG_WARN(
        "M2 lander: D3D11 hook install failed (d3d11.dll not loaded yet?) — "
        "frame loop will not tick until game creates its device");
    delete g_d3d11_hook;
    g_d3d11_hook = nullptr;
  }

  // M2 lander (D3D12 path): install the D3D12 + DXGI factory + Present
  // hook. Whichever path matches the game's D3D runtime (D3D11 or
  // D3D12) becomes the per-frame owner. ATS 1.61.x uses D3D12 — the
  // D3D11 hook above will fail to install in that case, and this
  // D3D12 hook will be the one that actually drives the per-frame
  // tick. Both hook the same IDXGISwapChain::Present vtable slot, but
  // their install paths are independent, so there is no double-tick
  // risk: the game calls exactly one CreateDXGIFactory1 (or exactly
  // one D3D11CreateDeviceAndSwapChain), only one chain captures a
  // swap chain, and only that chain reaches the Present patch.
  g_d3d12_hook = new walkercore::D3D12Hook();
  g_d3d12_hook->BindFrameLoop(g_frame_loop);
  if (g_d3d12_hook->Install()) {
    WALKERCORE_LOG_INFO("M2 lander: D3D12 + DXGI factory hook active");
    walkercore::diagnostic_internal::SetD3D12(g_d3d12_hook);
  } else {
    WALKERCORE_LOG_WARN(
        "M2 lander: D3D12 hook install failed (d3d12.dll or dxgi.dll "
        "not loaded yet?) — frame loop will not tick until game "
        "creates its device");
    delete g_d3d12_hook;
    g_d3d12_hook = nullptr;
  }

  // M2 fallback lander: start the background per-frame ticker.
  //
  // This is the always-on path for games where the D3D Present hooks
  // never fire (the SCS loader runs plugins AFTER the game's graphics
  // subsystem already initialized the swap chain). The ticker drives
  // walker_per_frame_tick at ~60 Hz and cooperates with the D3D
  // Present hooks via the per-frame lock — so when (and if) a Present
  // path does fire later (resolution change → swap chain
  // re-creation), the background ticker silently defers to the
  // render-thread tick. There is no separate "is the render path
  // active?" flag to keep in sync; the lock is the only source of
  // truth.
  //
  // We start it AFTER every provider has been wired (above) AND AFTER
  // the D3D hooks' Install() — Install() does not touch the swap
  // chain yet (it hooks CreateDXGIFactory1 / D3D11CreateDeviceAnd
  // SwapChain, which only fire on FUTURE factory creations), so the
  // order between Install() and ticker Start() doesn't matter for
  // correctness; we keep them together for readability.
  g_per_frame_thread = new walkercore::PerFrameThread();
  if (!g_per_frame_thread->Start()) {
    // Should be impossible (just constructed), but defensive — a
    // second Start() call from the same instance is the only failure
    // mode. Don't tear down on this; the game still runs without a
    // per-frame tick driver and the user gets a useful log line.
    WALKERCORE_LOG_WARN(
        "PerFrameThread::Start returned false (ticker already running?) — "
        "frame loop will not tick");
  }

  return SCS_RESULT_ok;
}

extern "C" __declspec(dllexport) SCSAPI_VOID scs_telemetry_shutdown(void) {
  // M2: tear down the D3D12 hook first — it holds the MinHook state
  // for d3d12.dll + dxgi.dll and the captured D3D12 device. We
  // uninstall D3D12 before D3D11 because both paths share the
  // IDXGISwapChain::Present vtable patch in the abstract sense, but
  // the static state lives in two different translation units; the
  // first to be torn down must release its MH_ALL_HOOKS gate.
  if (g_d3d12_hook != nullptr) {
    walkercore::diagnostic_internal::SetD3D12(nullptr);
    g_d3d12_hook->Uninstall();
    delete g_d3d12_hook;
    g_d3d12_hook = nullptr;
  }

  // M2: tear down the D3D11 hook next.
  if (g_d3d11_hook != nullptr) {
    walkercore::diagnostic_internal::SetD3D11(nullptr);
    g_d3d11_hook->Uninstall();
    delete g_d3d11_hook;
    g_d3d11_hook = nullptr;
  }

  // M6: settings last — it has non-owning pointers to FrameLoop / InputHook
  // and we want those alive until the very end.
  if (g_settings != nullptr) {
    delete g_settings;
    g_settings = nullptr;
  }

  // M5: tear down the FSM first; it holds a pointer to FrameLoop's camera
  // and could write into a dangling camera during a DSR.
  if (g_fsm != nullptr) {
    delete g_fsm;
    g_fsm = nullptr;
  }

  // M2: tear down the per-frame ticker first so no new
  // walker_per_frame_tick call lands while we destroy the providers
  // it depends on. The background thread's last tick is past the join
  // point; any in-flight Present hook also tries the lock first and
  // skips if we are already mid-teardown.
  if (g_per_frame_thread != nullptr) {
    g_per_frame_thread->Stop();
    delete g_per_frame_thread;
    g_per_frame_thread = nullptr;
  }

  // M2: tear down the frame loop first so any in-flight Present hook
  // doesn't see dangling pointers.
  if (g_frame_loop != nullptr) {
    delete g_frame_loop;
    g_frame_loop = nullptr;
  }

  // M4: tear down the input hook (unhooks the keyboard low-level hook).
  if (g_input != nullptr) {
    g_input->Uninstall();
    delete g_input;
    g_input = nullptr;
  }
  // Clear the diagnostic pointer before the object is gone so a stale
  // dump can't read freed memory.
  walkercore::diagnostic_internal::SetInput(nullptr);

  // M3 (stub): uninstall the world geometry provider.
  if (g_world != nullptr) {
    g_world->Uninstall();
    delete g_world;
    g_world = nullptr;
  }

  // M8: tear down the fuel provider. Clear the diagnostic pointer first
  // so any in-flight dump reads null instead of freed memory.
  walkercore::diagnostic_internal::SetFuel(nullptr);
  if (g_fuel != nullptr) {
    delete g_fuel;
    g_fuel = nullptr;
  }

  // M10: tear down the knockout provider.
  walkercore::diagnostic_internal::SetKnockout(nullptr);
  if (g_knockout != nullptr) {
    delete g_knockout;
    g_knockout = nullptr;
  }

  // M7: tear down the flashlight provider. Clear the diagnostic pointer
  // first so any in-flight dump reads null instead of freed memory.
  walkercore::diagnostic_internal::SetFlashlight(nullptr);
  if (g_flashlight != nullptr) {
    delete g_flashlight;
    g_flashlight = nullptr;
  }

  // M7: tear down the door animator. Clear the diagnostic pointer
  // first so any in-flight dump reads null instead of freed memory.
  walkercore::diagnostic_internal::SetDoorAnim(nullptr);
  if (g_door_anim != nullptr) {
    delete g_door_anim;
    g_door_anim = nullptr;
  }

  // M7: tear down the head tracker. Clear the diagnostic pointer first
  // so any in-flight dump reads null instead of freed memory.
  walkercore::diagnostic_internal::SetHeadTracker(nullptr);
  if (g_head_tracker != nullptr) {
    delete g_head_tracker;
    g_head_tracker = nullptr;
  }

  // M7: tear down the shadow provider. Clear the diagnostic pointer
  // first so any in-flight dump reads null instead of freed memory.
  walkercore::diagnostic_internal::SetShadow(nullptr);
  if (g_shadow != nullptr) {
    delete g_shadow;
    g_shadow = nullptr;
  }

  // M9: tear down the coupling provider. No diagnostic mirror — the
  // provider's state is exposed via walker_dump_input / the F12 dump
  // only via its effect on TruckState, not a direct pointer mirror.
  if (g_coupling != nullptr) {
    delete g_coupling;
    g_coupling = nullptr;
  }

  // M11: tear down the settings menu. Closing it first ensures any
  // unsaved edits are persisted (or dropped, depending on dirty_).
  // Clear the diagnostic pointer before deleting so any in-flight dump
  // reads null instead of freed memory.
  walkercore::diagnostic_internal::SetSettingsMenu(nullptr);
  if (g_settings_menu != nullptr) {
    if (g_settings_menu->isOpen()) {
      g_settings_menu->Close();
    }
    delete g_settings_menu;
    g_settings_menu = nullptr;
  }

  // M1: tear down the telemetry provider BEFORE logging goes away. The
  // provider destructor doesn't talk to the SDK; only UnregisterAll does.
  if (g_telemetry != nullptr) {
    // We don't have the unregister fn pointer cached, but the SCS SDK
    // will auto-unregister everything on shutdown anyway (per
    // scssdk_telemetry_channel.h doc). Still call UnregisterAll via
    // re-fetched v100 layout if needed — for M1 we skip it and rely on
    // the SDK's auto-cleanup. M3+ may revisit.
    delete g_telemetry;
    g_telemetry = nullptr;
  }

  WALKERCORE_LOG_INFO("unloaded (clean)");
  walkercore_log_shutdown();
  g_game_log = nullptr;
  g_negotiated_version = "none";
  walkercore::diagnostic_internal::SetScsVersion(g_negotiated_version);
  walkercore::diagnostic_internal::SetTelemetry(nullptr);
  walkercore::diagnostic_internal::SetFsm(nullptr);
  walkercore::diagnostic_internal::SetFrameLoop(nullptr);
  walkercore::diagnostic_internal::SetSettings(nullptr);
}

// Standard DLL entry. We do almost nothing here; the SDK contract is that
// scs_telemetry_init is the moment we get a log sink and game metadata.
//
// DLL_PROCESS_ATTACH runs on the game thread when the loader maps us.
// DLL_PROCESS_DETACH runs when the game exits or unloads us; we keep
// cleanup light because process shutdown may already be in progress.
extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(
    HMODULE module, DWORD reason_for_call, LPVOID /*reserved*/) {
  switch (reason_for_call) {
    case DLL_PROCESS_ATTACH:
      // Skip DLL_THREAD_ATTACH / DLL_THREAD_DETACH notifications for
      // worker threads — WalkerCore runs on the game thread only, and
      // the notifications would just add per-thread load-time overhead.
      DisableThreadLibraryCalls(module);
      // At this point we have no SCS sink yet; log_fallback in Log.cpp
      // will route to OutputDebugStringW.
      break;
    case DLL_PROCESS_DETACH:
      // scs_telemetry_shutdown will already have run if the game called
      // it; do not double-free.
      break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      // WalkerCore runs on the game thread; we don't care about other
      // threads until M3+ when we might add a worker.
      break;
  }
  return TRUE;
}