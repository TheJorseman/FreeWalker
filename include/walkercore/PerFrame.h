// WalkerCore — per-frame tick contract for the M2 lander.
//
// The D3D11 hook (M2 lander) installs `IDXGISwapChain::Present` and, on
// each call, invokes walker_per_frame_tick() to advance one frame of
// simulation. The hook is the only place we touch the renderer, and the
// tick is the only place we touch game state outside of SCS callbacks.
//
// Per-frame contract:
//
//   1. walker_per_frame_tick takes dt_seconds (caller measures from its
//      own clock; we use it to integrate movement and animate).
//   2. The tick calls FrameLoop::RunOnce, which handles input sampling,
//      mouse-look, movement integration, collision, head bob, and camera
//      update.
//   3. After RunOnce, the tick calls WalkerFsm::Update. If the FSM wants
//      to override the camera (during a transition), it writes into the
//      FrameLoop via OverrideCamera for the next frame's RunOnce.
//   4. The tick polls the settings file (mtime check) so live INI edits
//      take effect without restarting the game.
//   5. The tick returns nothing; the render layer reads g_frame_loop's
//      camera via a getter (defined below).
//
// Order matters. The render layer must call walker_per_frame_tick BEFORE
// drawing, so that the camera matrices are up to date when IDXGISwapChain
// uses them. Then it either pushes our view/projection or the original
// game matrices, depending on WalkerFsm::ShouldRenderOverride().
//
// Threading:
//   - The D3D11 hook fires on the game thread. SCS callbacks also fire on
//     the game thread. We have no workers. All state is single-threaded.
//   - Reentrancy from the same thread is safe: the WH_KEYBOARD_LL hook
//     may fire inside walker_per_frame_tick on the same thread, but the
//     hook only updates a small struct; it never re-enters our tick.

#ifndef WALKERCORE_PER_FRAME_H
#define WALKERCORE_PER_FRAME_H

#include "walkercore/CameraState.h"
#include "walkercore/PlayerState.h"

#ifdef __cplusplus
extern "C" {
#endif

// Advance one frame of simulation. Idempotent if not in walk mode (no-op).
// Safe to call from the D3D11 Present hook on the game thread.
//
// The first call after scs_telemetry_init has dt=0 because we haven't
// measured a baseline yet; subsequent calls pass the measured delta.
void walker_per_frame_tick(double dt_seconds);

// Read-only access to the latest camera + player state. The D3D11 hook
// uses these to push matrices and to debug-draw a marker at the player
// position.
const walkercore::CameraState *walker_camera(void);
const walkercore::PlayerState *walker_player(void);

// Should the render layer use walker's view/projection matrices this frame?
// Returns 1 if the FSM is overriding the game camera; 0 if the game's own
// camera should be left alone.
int walker_should_override_render(void);

// Diagnostics — return the last controller's snapshot for the F12 dump.
// Returns a null-terminated label like "Walking" / "TransitionIn 0.42" /
// "Driving".
const char *walker_fsm_state_label(void);

// Diagnostics — write a multi-line snapshot of WalkerCore's internal state
// to the SCS log (or the OutputDebugStringW fallback when no log sink is
// bound). The dump covers:
//   - Negotiated SCS API version
//   - WalkerFsm state + transition progress
//   - TruckState snapshot (placement, velocity, fuel, adblue, parking brake,
//     trailer attached, and the per-field has_* flags)
//   - FrameLoop camera (eye position, yaw, pitch, FOV)
//   - FrameLoop player (feet position, mode, eye height, has_feet)
//   - Settings summary (movement speeds, head-bob amplitudes, mouse sens,
//     FOV, transition seconds)
//
// Output is plain ASCII so any sink (game log, debugger, stderr) renders
// it legibly. Safe to call from any thread that the SCS log accepts;
// typically called once per F12 key rising edge from the input hook.
void walker_dump_state(void);

// Diagnostics — focused dump of just the live settings (movement config,
// FOV, transition seconds, key bindings). Use this when investigating "did
// the INI reload actually pick up my edit?" without scrolling through the
// full walker_dump_state() output. Safe to call at any time; no-ops when
// no SettingsStore has been wired yet.
void walker_dump_settings(void);

// Diagnostics — focused dump of just the in-game settings menu runtime
// state (open flag, dirty flag). Use this when investigating "did
// Ctrl+F10 actually open the menu?" or "did the slider edit actually
// dirty the menu?". Safe to call at any time; no-ops when no
// SettingsMenu has been wired yet.
void walker_dump_menu(void);

// Diagnostics — focused dump of the head tracker subsystem (live
// config + smoothed state via Peek()). Use this when investigating
// "did my INI override for use_trackir take effect?" or "is the head
// tracker producing samples?". Safe to call at any time; no-ops when
// no HeadTrackerProvider has been wired yet.
void walker_dump_head_tracker(void);

// Diagnostics — focused dump of the shadow provider (config + the
// shadow projection for the current player state). Use this when
// investigating "is the shadow positioned where I expect?". Safe to
// call at any time; no-ops when no ShadowProvider has been wired yet,
// and reports "(no player state)" if no FrameLoop is wired.
void walker_dump_shadow(void);

// Diagnostics — focused dump of the flashlight provider (on/off +
// state computed from the live CameraState). Use this when
// investigating "did G toggle the light?" or "what direction does the
// cone point in?". Safe to call at any time; no-ops when no
// FlashlightProvider has been wired yet, and reports "(no camera
// state)" if no FrameLoop is wired.
void walker_dump_flashlight(void);

// Diagnostics — focused dump of the door animation provider (current
// swing angle + animating flag + the live target on the active
// animation). Use this when investigating "did the door open all the
// way?" or "is the open animation stuck mid-way?". Safe to call at
// any time; no-ops when no DoorAnimProvider has been wired yet.
void walker_dump_door(void);

// Diagnostics — focused dump of the negotiated game identity + the
// VersionEntry that matched (or "no entry matched" if the build is
// not in the table). Use this when investigating "did the build
// match?" or "which features does the plugin think are validated
// here?". Safe to call at any time; reports "(not initialized)"
// until scs_telemetry_init has populated the mirror.
void walker_dump_version(void);

// Diagnostics — focused dump of the WalkerFsm (state + transition
// progress + ShouldRenderOverride). Use this when investigating "what
// is the walker doing right now?" without scrolling through
// walker_dump_state(). Safe to call at any time; reports "(not
// initialized)" when no FSM has been wired yet. The state label
// comes from the mirror so the dump never perturbs the FSM.
void walker_dump_fsm(void);

// Diagnostics — focused dump of the input subsystem (per-key held /
// pressed / released bits, mouse delta, last update timestamp).
// Use this when investigating "did the keyboard hook fire?" or
// "what did the player press in the last frame?" without scrolling
// through walker_dump_state(). Safe to call at any time; reports
// "(not initialized)" when no InputHook has been wired yet, and
// "(no sample)" if the hook is installed but has not yet produced
// an InputState.
void walker_dump_input(void);

// Diagnostics — focused dump of the M3 PhysX adapter: install status,
// the catalog of known PxScene method VAs, and the slot dispatch the
// adapter would use if a live PxScene instance were available. Use
// this when investigating "did M3 find the catalog?" or "is the
// catalog up to date for this build?". Safe to call at any time;
// reports "(not initialized)" when no PhysXAdapter has been wired
// yet. When the adapter is installed but no live PxScene has been
// resolved, slot columns stay at "-1" (the no-hit fallback path).
void walker_dump_physx(void);

// M3 lander wire-up point. The real adapter calls this once when its
// PxScene / public PhysX SDK link succeeds. Until then, the stub returns
// "no hit" with one warning.
void walker_install_real_collision(void *world_geometry);

// M2 lander wire-up point. The D3D11 hook calls this from its Present
// trampoline with the swap-chain description's width/height. Until it
// is wired, the camera aspect defaults to 16:9.
void walker_set_swapchain_aspect(float aspect);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // WALKERCORE_PER_FRAME_H