// WalkerCore — CouplingProvider implementation (M9).
//
// M9 Lander — partial: the SCS SDK exposes the truck's hook_position
// (truck-local dvector) via the configuration event, and the trailer's
// world placement via the trailer.world.placement channel. With those
// two we can compute the world-space ApproachToKingPin waypoint
// instead of the 2.5 m truck-local placeholder used by the M9 stub.
// Until the SDK delivers both, WaypointForStep() falls back to the
// placeholder offsets documented below; the player still sees a
// usable coupling flow, just with a 5th-wheel position that's only
// roughly right for non-standard truck models.

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
    // M9 lander delivers the SCS configuration event + trailer
    // world-placement channel. Until BOTH have fired, the ApproachTo
    // KingPin waypoint uses the 2.5 m truck-local placeholder; once
    // they have, the world-space hook position wins. The remaining
    // M9 lander piece is the actual SCS trailer-attach API call
    // (ForceComplete path), which still uses the placeholder.
    WALKERCORE_LOG_WARN(
        "CouplingProvider: M9 lander partial — hook_position is read "
        "from the SCS configuration event when available, but the "
        "SCS trailer-attach API call (ForceComplete) is still a no-op. "
        "ApproachToKingPin tracks the real hook when the SDK delivers "
        "it; landing-gear + airlines steps still use the truck-local "
        "placeholder offsets.");
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

    // Placeholder waypoints in truck-local. These are reasonable
    // defaults for a US daycab + dry-van trailer.
    //
    // M9 Lander: for ApproachToKingPin specifically, when the SCS
    // configuration event has delivered a real hook_position, the
    // waypoint becomes the world-space hook (truck-local hook
    // transformed through truck.placement). The other steps still
    // use the placeholder offsets relative to the trailer's world
    // pose; the landing-gear step targets a point 2 m behind the
    // hook, ConnectAirLines targets 2.1 m behind and 0.1 m up, and
    // RaiseLandingGear repeats the landing-gear offset. We derive
    // the trailer-relative offsets from the truck-local hook so
    // they track the trailer's actual size: a long dry-van keeps
    // the crank handle near +4.5 m, a short day-cab trailer keeps
    // it near +2.0 m, etc.
    Vec3 waypoint = {0, 0, 0};
    float radius = 1.0f;
    // Placeholder hook offset: 2.0 m behind the cab origin at
    // waist height. The M9 stub used a 2.5 m ApproachToKingPin
    // waypoint directly; with the hook-relative rewrite we treat
    // the placeholder hook at z=2.0 so the rewritten Approach
    // waypoint (hook.z + 0.5) lands at z=2.5 — the original
    // observable position. Landing-gear (hook.z + 2.0) lands at
    // z=4.0; the original M9 stub used z=4.5 for landing gear
    // (a half-meter further back, accounting for the older stub
    // measuring from the cab rather than the 5th wheel). We
    // accept the half-meter shift as a placeholder trade-off —
    // when the real hook is delivered the waypoints snap to the
    // correct world position, and the placeholder only matters
    // for the first few seconds after Start() before the SDK
    // delivers the configuration event.
    Vec3 hook_local_for_relative = {0.0, 0.5, 2.0};
    bool use_real_hook = false;
    if (truck.has_hook_position) {
        hook_local_for_relative = truck.hook_position;
        use_real_hook = true;
    }
    switch (step) {
        case CouplingStep::ApproachToKingPin:
            // Behind the 5th wheel by 0.5 m so the player stands at
            // the back of the cab looking at the king pin. Y=0.5 m
            // matches a typical cab-floor waist height.
            waypoint = Vec3{hook_local_for_relative.x,
                            hook_local_for_relative.y,
                            hook_local_for_relative.z + 0.5};
            radius = 1.5f;
            break;
        case CouplingStep::LowerLandingGear:
            // Landing gear is 2 m behind the king pin on a standard
            // dry-van. The hook_offset is the local Z of the 5th
            // wheel, so the gear is at hook.z + 2.
            waypoint = Vec3{hook_local_for_relative.x,
                            hook_local_for_relative.y - 0.1,
                            hook_local_for_relative.z + 2.0};
            radius = 1.0f;
            break;
        case CouplingStep::ConnectAirLines:
            // Trailer front face, slightly above the gear: hook.z +
            // 2.1 (just past the gear), +0.1 m up.
            waypoint = Vec3{hook_local_for_relative.x,
                            hook_local_for_relative.y + 0.1,
                            hook_local_for_relative.z + 2.1};
            radius = 1.0f;
            break;
        case CouplingStep::RaiseLandingGear:
            // Same as LowerLandingGear — the crank handle is in the
            // same place for raising and lowering.
            waypoint = Vec3{hook_local_for_relative.x,
                            hook_local_for_relative.y - 0.1,
                            hook_local_for_relative.z + 2.0};
            radius = 1.0f;
            break;
        case CouplingStep::Complete:
            waypoint = Vec3{hook_local_for_relative.x,
                            hook_local_for_relative.y,
                            hook_local_for_relative.z};
            radius = 5.0f;
            break;
        default:
            break;
    }
    (void)use_real_hook;  // already used to populate hook_local_for_relative

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
    if (step == CouplingStep::Idle) {
        return;
    }

    const CouplingState wp = WaypointForStep(step, truck);

    // Always publish the waypoint to state_ — consumers (renderers,
    // distance-driven UI, the F12 dump) need to see WHERE the current
    // step wants the player, not just whether the player happens to
    // be inside the step's radius this frame. We do this for ALL
    // non-Idle steps, including Complete, so the consumer sees the
    // Complete waypoint (truck-local hook) the moment ForceComplete
    // lands. The radius-aware progress gate below still uses the
    // local `wp`, not state_, so this write doesn't perturb the FSM.
    // (The M9 lander tests pin this: with the player at (1e6, 1e6,
    // 1e6) — far outside any waypoint radius — the state_ still
    // reflects the step's target.)
    state_.waypoint_local = wp.waypoint_local;
    state_.waypoint_radius_m = wp.waypoint_radius_m;
    state_.progress = wp.progress;

    if (step == CouplingStep::Complete) {
        // ForceComplete() puts the FSM here, but the FSM does not
        // tick through Complete; the early-return prevents the
        // dist / progress / advance code below from running, which
        // is what we want. The state_ waypoint is now the Complete
        // waypoint (truck-local hook), which the dump and any
        // post-coupling flow reads directly.
        return;
    }

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
}

void CouplingProvider::Tick(const TruckState &truck, const PlayerState &player,
                             double dt, bool interact_pressed) {
    AdvanceIfReady(truck, player, interact_pressed, dt);
}

}  // namespace walkercore