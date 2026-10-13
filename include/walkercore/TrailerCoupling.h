// WalkerCore — Trailer coupling (M9).
//
// Walks the player through the steps of connecting a trailer to a truck.
// The flow has 5 steps; each one is a sub-task that takes a few seconds of
// in-character animation:
//
//   1. ApproachToKingPin      — walk from cabin back to the fifth wheel
//   2. LowerLandingGear       — turn the crank handle (3–4 rotations)
//   3. ConnectAirLines        — plug in air + electrical
//   4. RaiseLandingGear       — turn the crank handle up (3–4 rotations)
//   5. Complete               — done; back to walk mode
//
// Each step has:
//   - a waypoint position (truck-local)
//   - a duration in seconds the player has to stand still while doing it
//   - a "completed" predicate
//
// Stub:
//   - Uses placeholder waypoints (typical tractor-trailer dimensions).
//   - Doesn't talk to the SCS trailer-attach API; that's M9 lander.
//
// Per-step advance:
//   - User presses F (Interact) when ready, OR
//   - Player walks out of the step's "staying area" and the step cancels.

#ifndef WALKERCORE_TRAILER_COUPLING_H
#define WALKERCORE_TRAILER_COUPLING_H

#include "walkercore/PlayerState.h"
#include "walkercore/TruckState.h"

namespace walkercore {

enum class CouplingStep : unsigned {
    Idle = 0,
    ApproachToKingPin,
    LowerLandingGear,
    ConnectAirLines,
    RaiseLandingGear,
    Complete,
    LastStep,  // sentinel — array bound + iteration terminator
};

// Human-readable name for a CouplingStep. Returns ASCII literals
// ("Idle", "ApproachToKingPin", ...). The exhaustive coverage is
// guaranteed by -Wswitch-enum in the implementation; the diagnostic
// dump uses this so analysts see the step name instead of the
// numeric enum value.
const char *CouplingStepName(CouplingStep s);

struct CouplingState {
    CouplingStep step = CouplingStep::Idle;
    // 0..1 progress through the current step's animation.
    float progress = 0.0f;
    // Where the player has to be to make progress.
    Vec3 waypoint_local;
    // Radius around the waypoint within which the player can interact.
    float waypoint_radius_m = 1.0f;
};

class CouplingProvider {
public:
    CouplingProvider();
    ~CouplingProvider();

    void Tick(const TruckState &truck, const PlayerState &player, double dt,
              bool interact_pressed);

    CouplingState state() const { return state_; }

    bool isActive() const {
        return state_.step != CouplingStep::Idle &&
               state_.step != CouplingStep::Complete;
    }

    void Start();
    void Cancel();
    void ForceComplete();  // for M9 lander when SCS confirms attach

    // M9 lander hook for the official SCS telemetry channel
    // `trailer.connected`. When the channel reports `attached=true` and
    // a coupling step is in progress, the provider auto-completes the
    // flow instead of waiting for the player to walk through every
    // step. When the channel reports `attached=false`, the provider
    // resets so a subsequent Start() begins fresh. Calls are no-ops
    // when the FSM is Idle (no in-progress coupling) and are
    // idempotent across identical transitions.
    void OnTrailerAttachStateChanged(bool attached);

    // Per-step duration (seconds) — config so M6 INI can tune.
    void SetStepDuration(CouplingStep step, double seconds);

private:
    void AdvanceIfReady(const TruckState &truck, const PlayerState &player,
                        bool interact_pressed, double dt);
    CouplingState WaypointForStep(CouplingStep step, const TruckState &truck) const;

    CouplingState state_;
    double step_durations_[static_cast<unsigned>(CouplingStep::LastStep)] = {
        0.0,   // Idle
        1.0,   // Approach (auto-completes when within radius)
        4.0,   // LowerLandingGear (3 turns × 1.3 s)
        2.0,   // ConnectAirLines
        4.0,   // RaiseLandingGear
        0.0,   // Complete
    };
};

}  // namespace walkercore

#endif  // WALKERCORE_TRAILER_COUPLING_H