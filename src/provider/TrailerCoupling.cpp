// WalkerCore — CouplingProvider implementation (M9).

#include "walkercore/TrailerCoupling.h"

#include <math.h>

#include <algorithm>

#include "walkercore/Log.h"
#include "walkercore/TruckDoorConfig.h"

namespace walkercore {

namespace {

// File-scope warn flag so the test-only reset hook can flip it back.
bool g_warned = false;

void WarnStub() {
    if (g_warned) return;
    g_warned = true;
    WALKERCORE_LOG_WARN(
        "CouplingProvider: stub active — waypoints are truck-local "
        "placeholders, the SCS trailer-attach API is not yet wired. "
        "Real impl lands in M9 lander.");
}

constexpr double kTwoPi = 6.28318530717958647692;

}  // namespace

extern "C" void ResetCouplingStubWarnForTesting() {
    g_warned = false;
}

CouplingProvider::CouplingProvider() {
    state_.step = CouplingStep::Idle;
    state_.progress = 0.0f;
    state_.waypoint_local = {0, 0, 0};
}

CouplingProvider::~CouplingProvider() = default;

void CouplingProvider::SetStepDuration(CouplingStep step, double seconds) {
    const unsigned idx = static_cast<unsigned>(step);
    if (idx < sizeof(step_durations_) / sizeof(step_durations_[0])) {
        step_durations_[idx] = seconds;
    }
}

void CouplingProvider::Start() {
    state_.step = CouplingStep::ApproachToKingPin;
    state_.progress = 0.0f;
    WALKERCORE_LOG_INFO("CouplingProvider: started");
}

void CouplingProvider::Cancel() {
    state_.step = CouplingStep::Idle;
    state_.progress = 0.0f;
    WALKERCORE_LOG_INFO("CouplingProvider: cancelled");
}

void CouplingProvider::ForceComplete() {
    state_.step = CouplingStep::Complete;
    state_.progress = 1.0f;
}

CouplingState CouplingProvider::WaypointForStep(CouplingStep step,
                                                  const TruckState &truck) const {
    CouplingState out;
    out.step = step;
    out.progress = state_.progress;

    // Placeholder waypoints in truck-local. These are reasonable defaults
    // for a US daycab + dry-van trailer; the M9 lander swaps them for
    // values read from the trailer model.
    Vec3 waypoint = {0, 0, 0};
    float radius = 1.0f;
    switch (step) {
        case CouplingStep::ApproachToKingPin:
            waypoint = Vec3{0.0, 0.5, 2.5};   // back of cab, just behind 5th wheel
            radius = 1.5f;
            break;
        case CouplingStep::LowerLandingGear:
            waypoint = Vec3{0.0, 0.4, 4.5};   // near trailer landing gear
            radius = 1.0f;
            break;
        case CouplingStep::ConnectAirLines:
            waypoint = Vec3{0.0, 0.6, 4.6};   // trailer front, electrical+air
            radius = 1.0f;
            break;
        case CouplingStep::RaiseLandingGear:
            waypoint = Vec3{0.0, 0.4, 4.5};   // back to crank handle
            radius = 1.0f;
            break;
        case CouplingStep::Complete:
            waypoint = Vec3{0.0, 0.0, 2.5};
            radius = 5.0f;
            break;
        default:
            break;
    }

    if (truck.has_placement) {
        const double heading =
            static_cast<double>(truck.placement.orientation.heading) * kTwoPi;
        const Vec3 world = Vec3{
            truck.placement.position.x +
                TruckLocalToWorld(waypoint, heading).x,
            truck.placement.position.y + waypoint.y,
            truck.placement.position.z +
                TruckLocalToWorld(waypoint, heading).z};
        out.waypoint_local = world;
    } else {
        out.waypoint_local = waypoint;
    }
    out.waypoint_radius_m = radius;
    return out;
}

void CouplingProvider::AdvanceIfReady(const TruckState &truck,
                                       const PlayerState &player,
                                       bool interact_pressed, double dt) {
    WarnStub();

    const auto step = state_.step;
    if (step == CouplingStep::Idle || step == CouplingStep::Complete) {
        return;
    }

    const CouplingState wp = WaypointForStep(step, truck);

    // Distance from player to waypoint.
    const double dx = player.feet_position.x - wp.waypoint_local.x;
    const double dy = player.feet_position.y - wp.waypoint_local.y;
    const double dz = player.feet_position.z - wp.waypoint_local.z;
    const double dist = sqrt(dx * dx + dy * dy + dz * dz);

    if (dist > static_cast<double>(wp.waypoint_radius_m)) {
        // Out of range — pause progress, but don't auto-cancel; the player
        // can come back.
        return;
    }

    // In range. For ApproachToKingPin, the step auto-completes (no time).
    // For the others, the player must hold F or just stand still; we use
    // stand-still with progress advancing toward the step duration.
    const double dur = step_durations_[static_cast<unsigned>(step)];
    if (dur <= 0.0) {
        // Instant step.
        state_.progress = 1.0f;
    } else {
        // Progress advances at 1/dur per second. interact_pressed also
        // speeds up (1.5×) — TMRealWalk lets the player "push" the crank.
        const double speed = interact_pressed ? 1.5 : 1.0;
        const double cur = static_cast<double>(state_.progress);
        state_.progress = static_cast<float>(
            std::min(1.0, cur + speed * (dt / dur)));
    }

    if (state_.progress >= 1.0f) {
        // Advance to next step. Use the LastStep sentinel as the bound so
        // adding a new step earlier in the enum doesn't silently break
        // the transition (the previous version hardcoded `% 6` and had a
        // dead branch wrapping back to Idle).
        const unsigned cur = static_cast<unsigned>(state_.step);
        const unsigned next = cur + 1;
        const unsigned last = static_cast<unsigned>(CouplingStep::LastStep);
        if (next >= last) {
            state_.step = CouplingStep::Complete;
        } else {
            state_.step = static_cast<CouplingStep>(next);
        }
        state_.progress = 0.0f;
        WALKERCORE_LOG_INFO("CouplingProvider: step advanced to %u",
                            static_cast<unsigned>(state_.step));
    }

    // Refresh waypoint in state for the consumer.
    state_.waypoint_local = wp.waypoint_local;
    state_.waypoint_radius_m = wp.waypoint_radius_m;
}

void CouplingProvider::Tick(const TruckState &truck, const PlayerState &player,
                             double dt, bool interact_pressed) {
    AdvanceIfReady(truck, player, interact_pressed, dt);
}

}  // namespace walkercore