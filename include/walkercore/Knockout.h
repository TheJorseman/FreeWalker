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
    // The previous default of 12.0 m/s was tuned to allow a 4.5 m/s run
    // speed + a 9.81 m/s gravity-driven freefall combine to ~10.79 m/s
    // without tripping, with 12 m/s as the headroom. Empirically, the
    // in-game DLL on ATS 1.61.3.1s at build 3fbc149 (and the cap-only
    // follow-ups) still produced spurious 12-38 m/s triggers within
    // 30-575 ms of a Walking-mode entry from a prior Driving stint, even
    // though the cap should keep the per-frame |dy| ≤ 0.164 m. The
    // discrepancy between the cap-derived 10.79 m/s upper bound and the
    // observed 12-38 m/s triggers suggests motion is reaching
    // `player_.feet_position` from a path the FrameLoop cap doesn't
    // cover — likely a leftover stale-tick interaction with the
    // KnockoutProvider's per-frame tracking_ baseline across the FSM's
    // Driving ↔ Transition → Walking cycle (the cap is on
    // `vertical_velocity_` but the per-frame delta the Knockout
    // heuristic reads is the integrated motion between two consecutive
    // Tick() calls, which can span sub-frame states). Until the M10
    // lander brings a real PhysX-derived collision normal, the safe
    // remediation is to raise the threshold so the heuristic stops
    // false-tripping while still catching genuine car impacts (real
    // ped-vs-truck + ped-vs-trailer hits are 25-60 m/s instantaneous).
    //
    // Bumped 12.0 → 25.0 with the M11-velocity-cap fix (commit 3fbc149)
    // so the heuristic remains meaningful for true impacts without
    // spamming false-positives in a workflow that doesn't have a real
    // PxScene yet.
    double velocity_change_threshold_mps = 25.0;
    // Per-frame position delta above which we treat the move as a
    // programmatic reset (FSM snap to door / seat anchor, teleport,
    // spawn, etc.) and re-prime last_position_ without firing the
    // knockout heuristic. Real pedestrian-vs-car collisions don't move
    // a body more than ~1 m in a single 16 ms frame; anything above
    // that is a snap. Defaults to 1.0 m / frame (~60 m/s instantaneous
    // velocity — well above any genuine car impact, well below the
    // "Position snapped back to cabin seat" deltas we observed in the
    // hook-disabled build, which were 1.15 m–11.5 m per frame). The M10
    // lander replaces this heuristic with a real PhysX-driven
    // collision normal and can keep the same field semantics (it's a
    // "snap re-prime threshold" guard, not a velocity check).
    double teleport_reprime_distance_m = 1.0;
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