// WalkerCore — internal accessor for the diagnostic dump.
//
// Lives in the public include path because PluginEntry.cpp needs to wire
// the process-wide subsystem pointers (telemetry, FSM, frame loop,
// settings, negotiated SCS version) into DiagnosticDump.cpp's
// walker_dump_state(). The header is named with the "Access" suffix to
// signal "not a public API — only the adapter layer uses this".
//
// Domain code never includes this header: walker_dump_state is exposed
// via walkercore/PerFrame.h, which is the SDK-free public surface.

#ifndef WALKERCORE_DIAGNOSTIC_ACCESS_H
#define WALKERCORE_DIAGNOSTIC_ACCESS_H

#include <stdint.h>

namespace walkercore {

class WalkerFsm;
class ScsTelemetryProvider;
class FrameLoop;
class SettingsStore;
class SettingsMenu;
class InputHook;
class FuelProvider;
class KnockoutProvider;
class D3D11Hook;
class D3D12Hook;
class FlashlightProvider;
class DoorAnimProvider;
class HeadTrackerProvider;
class ShadowProvider;

// Forward-declared so EncodeFsmLabel / FsmLabelNeedsReencode can take it
// by value without forcing every consumer of DiagnosticAccess.h to pull
// in WalkerFsm.h. The full definition lives in walkercore/WalkerFsm.h.
enum class WalkerFsmState : unsigned;

namespace diagnostic_internal {

// Wire the negotiated SCS API version string ("1.00", "1.01", or "none"
// before init / after shutdown).
void SetScsVersion(const char *v);

// Mirror the negotiated game identity (game_id + game_version) so the
// focused walker_dump_version() dump can surface "did the game match
// the VersionTable, and with what feature set?" without re-running the
// SDK negotiation. game_id is expected to outlive scs_telemetry_shutdown
// per the SCS SDK contract; we copy the string into a small buffer so
// the diagnostic layer does not need an extra lifetime constraint.
void SetIdentity(const char *game_id, uint32_t game_version);

// Reset the identity mirror to the "no game yet" state. Tests use
// this to drive walker_dump_version() into the (not initialized)
// path. PluginEntry.cpp does not call this — the adapter never
// transitions the mirror back to "pre-init" once it is set.
void ClearIdentity();

// Mirror the latest FSM state label written by PluginEntry.cpp. The label
// is the human-readable form like "Walking" or "TransitionIn t=0.42".
//
// Implementation: bounded text copy via strncpy (not snprintf with "%s")
// — strncpy is materially faster on long runs of the per-frame conductor
// (M2 lander's D3D11 Present hook calls walker_per_frame_tick at 60 Hz).
void SetFsmLabel(const char *l);

// Read the mirrored FSM label. Used by walker_dump_state() to avoid
// recomputing the FSM state on every dump, AND by walker_fsm_state_label()
// (the exported ABI for the M2 lander) which now reads from the same
// canonical buffer instead of maintaining its own copy.
const char *GetFsmLabel();

// Encode the FSM state (+ transition progress for the tween states) into
// a human-readable label. Output format:
//
//   Driving       → "Driving"
//   Walking       → "Walking"
//   TransitionIn  → "TransitionIn · t=0.42"
//   TransitionOut → "TransitionOut · t=0.87"
//
// transition_progress is only used for the two transition states; for
// Driving / Walking the value is ignored. Returns the number of bytes
// written excluding the NUL terminator, clamped to out_size - 1 when
// the formatted output didn't fit. The buffer is always NUL-terminated.
size_t EncodeFsmLabel(WalkerFsmState state,
                     float transition_progress,
                     char *out_buf,
                     size_t out_size);

// Cache-decision helper for the per-frame conductor's steady-state skip.
// Returns true when the FSM label needs a fresh encode this frame:
//
//   - prev_encoded_state != current_state: the state itself changed
//     (e.g. Driving → TransitionOut at a press of F10).
//   - OR current_state is one of the transition types: the tween
//     progress changes every frame during a transition, so a label like
//     "TransitionIn · t=0.42" needs to refresh every frame even when
//     the state is the same.
//
// Returns false only when prev_encoded_state == current_state and the
// state is one of the two steady states (Driving / Walking), in which
// case the label is identical to the previous encode and we can skip
// the work.
bool FsmLabelNeedsReencode(WalkerFsmState prev_encoded_state,
                           WalkerFsmState current_state);

// Wire the live subsystem pointers. Pass nullptr when the subsystem is
// destroyed so walker_dump_state() prints "(not initialized)" for it.
void SetFsm(const WalkerFsm *f);
void SetTelemetry(const ScsTelemetryProvider *t);
void SetFrameLoop(const FrameLoop *f);
void SetSettings(const SettingsStore *s);
void SetSettingsMenu(const SettingsMenu *m);
void SetInput(const InputHook *i);
void SetFuel(const FuelProvider *f);
void SetKnockout(const KnockoutProvider *k);
void SetD3D11(const D3D11Hook *d);
void SetD3D12(const D3D12Hook *d);
void SetFlashlight(const FlashlightProvider *f);
void SetDoorAnim(const DoorAnimProvider *d);
void SetHeadTracker(const HeadTrackerProvider *h);
void SetShadow(const ShadowProvider *s);

}  // namespace diagnostic_internal
}  // namespace walkercore

#endif  // WALKERCORE_DIAGNOSTIC_ACCESS_H