// WalkerCore — KnockoutProvider implementation (M10).

#include "walkercore/Knockout.h"

#include <math.h>
#include <cmath>

#include "walkercore/Log.h"

namespace walkercore {

namespace {

// One-shot warn flag, file-scope so the test-only reset hook below can
// flip it back. We deliberately keep this in the translation unit's
// anonymous namespace so production code never references it.
bool g_warned = false;

void WarnStub() {
    if (g_warned) return;
    g_warned = true;
    WALKERCORE_LOG_WARN(
        "KnockoutProvider: stub active — vehicle-impact detection uses "
        "velocity-change heuristics only (with teleport re-prime for "
        "FSM position snaps). Real impl (collision normal from PhysX) "
        "lands in M10 lander.");
}

}  // namespace

extern "C" void ResetKnockoutStubWarnForTesting() {
    g_warned = false;
}

KnockoutProvider::KnockoutProvider() {
    state_.knocked_out = false;
    state_.time_remaining_s = 0.0;
    state_.impact_direction = {0, 0, 0};
    last_position_ = {0, 0, 0};
}

KnockoutProvider::~KnockoutProvider() = default;

void KnockoutProvider::SetConfig(const KnockoutConfig &cfg) {
    cfg_ = cfg;
}

void KnockoutProvider::Prime(const Vec3 &position) {
    // Force-reset the per-frame tracking baseline. Used by the per-frame
    // conductor on a Driving→Walking or TransitionIn→Walking FSM
    // transition so the very next Knockout tick compares against the
    // post-transition (door-anchor or in-cabin) coordinate instead of
    // the pre-transition coordinate. Without this explicit reset, the
    // first tick after a Walking re-entry can land a `dist` that's
    // larger than the M11-cap-derived 10.79 m/s upper bound should
    // allow, because:
    //   - RunOnce bails during Driving / TransitionIn / TransitionOut,
    //     so vertical_velocity_ (and therefore player.feet_position.y
    //     integration) stays at its prior Walking-session value across
    //     the entire cabin trip.
    //   - PlacePlayerAtDoor / PlacePlayerAtSeat snap feet_position to
    //     a new anchor in a single Tick, but the teleport_reprime check
    //     catches the snap mid-tween — there are roughly 36 frames of
    //     TransitionOut where RunOnce bails AND no snap happens, so the
    //     player sits at the door-anchor from the snap onward.
    //   - When Walking re-enters, the first FrameLoop RunOnce applies
    //     gravity to whatever vertical_velocity_ was preserved from the
    //     prior session. If that value was -X m/s with X > 9.81, the
    //     cap clamps to -9.81 in the *next* frame, but the first
    //     frame's gravity step + integration can still produce a dy
    //     that, combined with horizontal vx/vz from the door anchor's
    //     transition pre-position, lands above the velocity threshold.
    //
    // The conductor calls Prime() with the current player.feet_position
    // at the moment we recognise the FSM transitioning INTO Walking.
    // The next Tick then sees `tracking_` re-armed via this write so
    // the very first delta computed is from "now" onwards, not from
    // some stale Pre-Pre-Walking coordinate.
    last_position_ = position;
    tracking_ = true;
}

void KnockoutProvider::Tick(PlayerState &player, double dt) {
    WarnStub();

    if (state_.knocked_out) {
        // Already knocked out. Count down.
        state_.time_remaining_s -= dt;
        if (state_.time_remaining_s <= 0.0) {
            state_.knocked_out = false;
            state_.time_remaining_s = 0.0;
            player.mode = PlayerMode::Idle;
            // Force the next non-knocked-out Tick to re-prime via the
            // !tracking_ branch. Without this, last_position_ would
            // still hold the pre-knockout coordinate, and the first
            // post-recovery Tick would compute dist_sq against that
            // stale reference — easily tripping the threshold if the
            // game moved the player during the recovery window (impact
            // sliding, terrain collision, the Knockout vendor's own
            // camera/character controller, etc.). The priming path
            // already exists for the very-first-Tick case; reusing it
            // here keeps the recovery -> idle transition a single,
            // consistent contract.
            tracking_ = false;
            WALKERCORE_LOG_INFO("KnockoutProvider: recovered");
        }
        return;
    }

    if (!player.has_feet || dt <= 0.0) {
        // Player not on foot, or first frame.
        last_position_ = player.feet_position;
        return;
    }

    // Defence-in-depth dt guard. PerFrameThread::Run already skips
    // pathological dt windows at the source (sub-ms from Windows
    // scheduler preemption of sleep_until; >250 ms from post-suspend
    // resume), but any other call path — D3D Present hook, manual
    // external driver, future tick source — can reach Tick() with an
    // arbitrary dt. The arithmetic in this function divides by dt
    // (the velocity computation at the bottom: `velocity = dist /
    // dt`) so an unreasonable dt MUST NOT reach it. The 1 ms lower
    // bound matches PerFrameThread's source-side guard; the 250 ms
    // upper bound absorbs a 4-frame @60 Hz stall without resetting
    // the baseline on every normal hitch (a normal hitch is below
    // this and the next valid dt naturally absorbs the difference).
    //
    // Both branches re-prime last_position_ so the next valid Tick
    // computes against the post-disturbance coordinate instead of
    // the pre-disturbance reference. tracking_ is preserved (set to
    // whatever it was); the next valid dt will either hit the
    // !tracking_ first-frame path (if it was false) or compute
    // velocity from "now" against the just-set last_position_ (if
    // it was true). Either way, the baseline is fresh and a spurious
    // trigger is impossible.
    if (dt < 0.001 || dt > 0.250) {
        last_position_ = player.feet_position;
        return;
    }

    // First tracking frame: just latch the position and return. We
    // don't have a previous position to compare against, so any
    // velocity derived from the priming frame would be meaningless.
    // Skipping the arithmetic here saves the dist² / threshold² math
    // for the first Tick after init.
    if (!tracking_) {
        last_position_ = player.feet_position;
        tracking_ = true;
        return;
    }

    // Squared displacement — we compare dist² against (threshold × dt)²
    // so the threshold check needs no sqrt. The actual sqrt is only
    // computed inside the rare trigger branch (high-impact collision),
    // where we also need the magnitude to normalise the impact
    // direction vector. In steady state the player is not being hit by
    // a vehicle, so dist² stays well below the threshold² and the
    // sqrt is never taken.
    //
    // Teleport re-prime: any per-frame delta larger than
    // cfg_.teleport_reprime_distance_m is treated as a programmatic
    // position reset (FSM `PlacePlayerAtDoor` / `PlacePlayerAtSeat`
    // snaps the feet_position to a world anchor at the start of every
    // cabin transition; we observed 1.15 m – 11.5 m per-frame deltas
    // during Driving ↔ Walking transitions, which dwarf the 12 m/s
    // velocity threshold after even a single 16 ms tick). The right
    // response is "this was never a car crash — re-prime the baseline
    // and wait for the next frame", not "knock the player out". The
    // computed dist² is already available, so this check is just an
    // extra comparison.
    const double dx = player.feet_position.x - last_position_.x;
    const double dy = player.feet_position.y - last_position_.y;
    const double dz = player.feet_position.z - last_position_.z;
    const double dist_sq = dx * dx + dy * dy + dz * dz;
    const double teleport_threshold_sq =
        cfg_.teleport_reprime_distance_m * cfg_.teleport_reprime_distance_m;
    if (dist_sq >= teleport_threshold_sq) {
        // Programmatic snap (FSM transition, teleport, spawn). Re-prime
        // and skip the velocity-trigger branch for this frame.
        last_position_ = player.feet_position;
        return;
    }
    const double dist_threshold_sq = (cfg_.velocity_change_threshold_mps * dt) *
                                     (cfg_.velocity_change_threshold_mps * dt);

    if (dist_sq >= dist_threshold_sq) {
        const double dist = sqrt(dist_sq);
        const double velocity = dist / dt;
        state_.knocked_out = true;
        state_.time_remaining_s = cfg_.recovery_seconds;
        state_.impact_direction = Vec3{-dx / dist, -dy / dist, -dz / dist};
        player.mode = PlayerMode::KnockedOut;
        // Diagnostic-heavy log. The M11 velocity cap bounds per-frame
        // |dy| to MovementConfig::vertical_velocity_max_mps × dt
        // ≤ 0.164 m plus horizontal ≤ 0.075 m, so a healthy trigger
        // reports `velocity` ≤ ~10.79 m/s. Values substantially higher
        // than that mean either (a) the cap is bypassed by a code path
        // the FrameLoop's PhaseIntegrateMovement doesn't cover, or
        // (b) dt is much larger than 16.67 ms (per-frame thread
        // stall). The remaining fields (dx, dy, dz, dist, dt) give
        // the next session enough to localise the source without
        // re-deploying with a different logging path.
        WALKERCORE_LOG_WARN(
            "KnockoutProvider: knock-out triggered (impact velocity %.1f m/s, "
            "dist=%.3f m, dx=%.3f dy=%.3f dz=%.3f, dt=%.4f s, "
            "threshold=%.1f m/s, vmax-derived=%.3f m)",
            velocity, dist, dx, dy, dz, dt,
            cfg_.velocity_change_threshold_mps,
            cfg_.velocity_change_threshold_mps * dt);

        // M10 partial: TMRealWalk-style per-angle collision normal log.
        // Mirrors the "[physx]   angle %3.0f: %s%.2f%c normal=(...)" line
        // in TMRealWalk.dll (see docs/observations/tmrealwalk-findings.md
        // § "Collision world"). We log a single combined pair because the
        // stub has no real multi-probe raycast — the format stays
        // greppable and the next session with a live PxScene slot
        // dispatch can replace this with N probes without changing
        // log tooling.
        //
        // Convention:
        //   - angle is the horizontal yaw (in degrees, 0=+x, 90=+z
        //     following SCS convention) the impact came FROM, computed
        //     from the negated impact_direction.
        //   - normal is the full 3-D surface normal (the impact_direction
        //     we already stored).
        //   - magnitude is the impact velocity in m/s; signed direction
        //     matches the surface that pushed the player the most.
        const double nx = -dx / dist;
        const double ny = -dy / dist;
        const double nz = -dz / dist;
        const double yaw_rad = std::atan2(nz, nx);
        const double yaw_deg = yaw_rad * 57.29577951308232;
        WALKERCORE_LOG_WARN(
            "KnockoutProvider: collision direction (angle=%+.0f deg, "
            "normal=(%+.2f,%+.2f,%+.2f), |v|=%.1f m/s) "
            "[TMRealWalk-style probe]",
            yaw_deg, nx, ny, nz, velocity);
    }
    last_position_ = player.feet_position;
}

}  // namespace walkercore