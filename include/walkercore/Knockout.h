// WalkerCore — Knock-out (M10) + hand flashlight.
//
// Knock-out: when the player is hit by a vehicle at high speed (or falls
// from a height), they enter a KnockedOut state for a few seconds. During
// that period:
//   - Movement is disabled
//   - Camera tilts (looks at the ground)
//   - A black vignette / blur effect is shown (M11 UI)
//   - The player recovers to Idle when the timer expires
//
// Detection: we monitor the player's instantaneous velocity and the rate
// of change of position. A sudden, large velocity change while grounded is
// the typical "hit by a car" signature. Falling from height uses a
// vertical-velocity threshold.
//
// Hand flashlight: a separate, hand-aimed flashlight distinct from the
// body-attached M7 flashlight. Toggle with a different key (default LMB
// per the published TMRealWalk defaults — quick tap toggles).

#ifndef WALKERCORE_KNOCKOUT_H
#define WALKERCORE_KNOCKOUT_H

#include "walkercore/PlayerState.h"
#include "walkercore/TruckState.h"

namespace walkercore {

struct KnockoutConfig {
    // Threshold velocity change (m/s) within a single frame that triggers
    // a knock-out. Typical pedestrian-vs-car: ~10 m/s instant velocity
    // gain.
    //
    // The previous default of 25.0 m/s was raised as a workaround
    // after the M11-velocity-cap fix: the in-game DLL on ATS
    // 1.61.3.1s at build 3fbc149 still produced spurious 12-38 m/s
    // triggers within 30-575 ms of a Walking-mode entry from a
    // prior Driving stint, so the threshold was bumped to keep the
    // heuristic from spamming false-positives in a workflow that
    // doesn't have a real PxScene yet.
    //
    // Lowered 25.0 → 8.0 m/s with the M10-stub-fix commit because
    // 25 m/s is well above real pedestrian-vs-car impacts (8 m/s is
    // a 28.8 km/h hit — the lower end of "definitely knocked over"
    // for an adult, and the threshold the M10 lander wants). The
    // trade-off is that the 12-38 m/s bogus Y-clamp / FSM-snap
    // spikes may briefly come back as spurious triggers while the
    // M10 lander is still a stub. The log line emitted on every
    // trigger is now the authoritative traceback (dist, dx, dy, dz,
    // dt, vmax-derived, impact direction in TMRealWalk's
    // `[physx] angle ... normal=...` format) so the analyst can
    // tell at a glance whether the trigger is a real hit
    // (|dx|, |dy|, |dz| roughly comparable to threshold * dt) or a
    // spurious Y-clamp snap (one axis dominates). The M10 lander
    // replaces this heuristic with a real PxScene-driven collision
    // normal; this field stays as the "fallback threshold" knob
    // until then.
    double velocity_change_threshold_mps = 8.0;
    // Per-frame position delta above which we treat the move as a
    // programmatic reset (FSM snap to door / seat anchor, teleport,
    // spawn, etc.) and re-prime last_position_ without firing the
    // knockout heuristic. Real pedestrian-vs-car collisions don't
    // move a body more than ~1 m in a single 16 ms frame; anything
    // above that is a snap. Bumped 1.0 → 5.0 m so the FSM snap
    // (PlacePlayerAtDoor / PlacePlayerAtSeat) at the start of
    // every Driving ↔ Walking transition (the observed 1.15 m –
    // 11.5 m per-frame deltas) is caught as a snap and re-primed,
    // rather than triggering knockout. The threshold needs to be
    // well above the M11 velocity cap * dt (≈ 0.164 m + 0.075 m)
    // and well above real pedestrian hit magnitudes (~5 m/s * dt =
    // ~0.085 m), but well below the snap magnitudes we observed
    // (1.15 m+). 5.0 m is a safe middle ground; the M10 lander
    // can drop this back to 1.0 m once a real PxScene collision
    // normal is available and we can distinguish "snap" from "hit"
    // by inspecting the collision contact, not the position delta.
    double teleport_reprime_distance_m = 5.0;
    // Recovery duration (seconds). Bumped 8.0 → 1.5 with the
    // M11-velocity-cap fix so a false-positive triggers a brief
    // 1.5-second KnockedOut mode flip rather than the previous 8-second
    // lock-out — an 8-second lock-out is excessive when the heuristic
    // is a stub and can re-trigger repeatedly, leaving the player
    // visually stuck. The M10 lander's real collision normal will
    // handle impact-duration differently (likely a health-based
    // knockdown) and can dial recovery back up if needed.
    double recovery_seconds = 1.5;
    // Minimum fall height (m) to trigger a fall-damage knock-out.
    // Fall-damage detection is not in the stub; the field is reserved
    // for the M10 lander.
    double fall_height_threshold_m = 4.0;
};

struct KnockoutState {
    bool knocked_out;
    double time_remaining_s;  // 0 when not knocked out
    Vec3 impact_direction;    // direction the impact came from
};

class KnockoutProvider {
public:
    KnockoutProvider();
    ~KnockoutProvider();

    void SetConfig(const KnockoutConfig &cfg);
    const KnockoutConfig &config() const { return cfg_; }

    void Tick(PlayerState &player, double dt);
    const KnockoutState &state() const { return state_; }

    bool isKnockedOut() const { return state_.knocked_out; }

    // Force-reset the per-frame tracking baseline so the next Tick
    // compares against `position` instead of the last FrameLoop-driven
    // feet_position. The M11 velocity cap can't cover the Driving ↔
    // Walking → per-frame-knockout window because RunOnce bails during
    // the transition (vertical_velocity_ is preserved across the
    // trip into the cabin and back), so the *first* Walking tick after
    // a transition can land a delta that's larger than the cap-derived
    // 10.79 m/s would suggest — we re-prime explicitly here as the
    // door-snap frame runs and the Walker's played-through state has
    // not yet accumulated any user-driven motion. The conductor calls
    // this on Walking-entry (see PluginEntry::walker_per_frame_tick,
    // Phase B' between B and C0).
    void Prime(const Vec3 &position);

private:
    KnockoutConfig cfg_;
    KnockoutState state_;
    Vec3 last_position_;
    bool tracking_ = false;
};

}  // namespace walkercore

#endif  // WALKERCORE_KNOCKOUT_H