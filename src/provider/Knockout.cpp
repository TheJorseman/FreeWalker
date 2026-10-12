// WalkerCore — KnockoutProvider implementation (M10).

#include "walkercore/Knockout.h"

#include <math.h>

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
        "velocity-change heuristics only. Real impl (collision normal from "
        "PhysX) lands in M10 lander.");
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
    const double dx = player.feet_position.x - last_position_.x;
    const double dy = player.feet_position.y - last_position_.y;
    const double dz = player.feet_position.z - last_position_.z;
    const double dist_sq = dx * dx + dy * dy + dz * dz;
    const double dist_threshold_sq = (cfg_.velocity_change_threshold_mps * dt) *
                                     (cfg_.velocity_change_threshold_mps * dt);

    if (dist_sq >= dist_threshold_sq) {
        const double dist = sqrt(dist_sq);
        const double velocity = dist / dt;
        state_.knocked_out = true;
        state_.time_remaining_s = cfg_.recovery_seconds;
        state_.impact_direction = Vec3{-dx / dist, -dy / dist, -dz / dist};
        player.mode = PlayerMode::KnockedOut;
        WALKERCORE_LOG_WARN(
            "KnockoutProvider: knock-out triggered (impact velocity %.1f m/s)",
            velocity);
    }
    last_position_ = player.feet_position;
}

}  // namespace walkercore