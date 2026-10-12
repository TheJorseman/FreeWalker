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
    // gain. We default to 8.0 m/s — comfortably above the player's
    // normal run speed (4.5 m/s in MovementConfig) so a running player
    // doesn't false-trigger, while still catching genuine car impacts
    // (8+ m/s instantaneous velocity gain). The proper delta-v tracking
    // (compare against previous-frame velocity, not absolute velocity)
    // is the M10 lander's job — the stub uses absolute velocity as an
    // approximation.
    double velocity_change_threshold_mps = 8.0;
    // Recovery duration (seconds).
    double recovery_seconds = 8.0;
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

private:
    KnockoutConfig cfg_;
    KnockoutState state_;
    Vec3 last_position_;
    bool tracking_ = false;
};

}  // namespace walkercore

#endif  // WALKERCORE_KNOCKOUT_H