// WalkerCore — Knockout tests.

#include "TestHarness.h"

#include <math.h>

#include "TestStubReset.h"
#include "walkercore/Knockout.h"

namespace walkercore {

void TestKnockoutThreshold() {
    KnockoutProvider provider;
    PlayerState player;
    PlayerState_Reset(player);
    player.has_feet = true;

    // Position 0..1: small move — no trigger.
    player.feet_position = Vec3{0, 0, 0};
    provider.Tick(player, 0.016);
    EXPECT_FALSE(provider.isKnockedOut());

    // Position 1: jump above threshold but below the teleport re-prime
    // distance (0.5 m / 16 ms = 31 m/s, default velocity threshold is
    // 12 m/s). Must trigger via the velocity path, NOT be re-primed
    // as a teleport.
    player.feet_position = Vec3{0.5, 0, 0};
    provider.Tick(player, 0.016);
    EXPECT_TRUE(provider.isKnockedOut());

    // Recovery over the configured recovery_seconds.
    KnockoutConfig cfg = provider.config();
    // Tick repeatedly with no movement until recovered.
    player.feet_position = Vec3{0.5, 0, 0};  // not moving
    double total = 0;
    while (!provider.isKnockedOut() == false && total < cfg.recovery_seconds + 1) {
        provider.Tick(player, 0.016);
        total += 0.016;
    }
    EXPECT_FALSE(provider.isKnockedOut());
}

void TestKnockoutRunningDoesNotTrigger() {
    // CRITICAL: a player running at the default run speed (4.5 m/s) must
    // NOT trigger a knock-out. The threshold default (8 m/s) is set to
    // a safe margin above this. We simulate steady-state running at
    // 4.5 m/s for several seconds and check no knock-out fires.
    KnockoutProvider provider;
    PlayerState player;
    PlayerState_Reset(player);
    player.has_feet = true;
    player.feet_position = Vec3{0, 0, 0};

    const double dt = 0.016;
    const double run_speed_mps = 4.5;
    const int frames = 600;  // 10 seconds

    for (int i = 0; i < frames; ++i) {
        player.feet_position.x += run_speed_mps * dt;
        provider.Tick(player, dt);
        EXPECT_FALSE(provider.isKnockedOut());
    }
}

void TestKnockoutFastImpactTriggers() {
    // A genuine car impact (instantaneous 10+ m/s velocity gain) MUST
    // trigger a knock-out. This is the case the threshold is calibrated
    // for. We pick a 0.7 m jump (= 0.7 / 0.016 = 43.75 m/s instantaneous
    // velocity, above the 12 m/s default threshold but below the 1.0 m
    // default teleport re-prime distance) so the velocity branch fires
    // instead of the teleport re-prime branch.
    KnockoutProvider provider;
    PlayerState player;
    PlayerState_Reset(player);
    player.has_feet = true;
    player.feet_position = Vec3{0, 0, 0};
    provider.Tick(player, 0.016);  // priming frame

    // 0.7 m jump = 0.7 / 0.016 = 43.75 m/s instantaneous velocity.
    player.feet_position = Vec3{0.7, 0, 0};
    provider.Tick(player, 0.016);
    EXPECT_TRUE(provider.isKnockedOut());
}

// -----------------------------------------------------------------------------
// KnockoutProvider state-cycle + config field-exhaustive probe.
//
// State machine:
//
//   Idle    --Tick(no trigger)--> Idle (tracking_=true after 1st tick)
//   Idle    --Tick(threshold)-->  KnockedOut (knocked_out=true, timer set)
//   KnockedOut --Tick(dt)-->     KnockedOut (timer counts down)
//   KnockedOut --Tick(recovered)--> Idle (knocked_out=false, mode=Idle)
//
// Config inputs the FSM reads:
//
//   cfg.velocity_change_threshold_mps  -- impact velocity gate
//   cfg.recovery_seconds               -- timer length
//   cfg.fall_height_threshold_m        -- reserved (stub doesn't read)
//
// Tick() inputs:
//
//   player.has_feet           -- gating; false → no-op (no tracking, no trigger)
//   player.feet_position      -- displacement → velocity → trigger
//   player.mode               -- WRITTEN: Idle ↔ KnockedOut
//   dt                        -- gating; <=0 → no-op
//
// State outputs:
//
//   state_.knocked_out          -- bool
//   state_.time_remaining_s     -- double, counts down
//   state_.impact_direction     -- Vec3, opposite of displacement at impact
//
// player.mode is ONLY written by the provider on the trigger and
// recovery transitions (Idle ↔ KnockedOut). On a non-trigger frame
// the provider leaves player.mode untouched — even if it's
// PlayerMode::Standing (the PlayerState_Reset default). This is the
// contract that "the player on foot is not yet walking"; the
// FrameLoop/FSM are responsible for setting it to Idle/Walking/etc.
//
// Each field MUST have an observable effect. The probe is split into
// a state-cycle walk and a per-field block; failures name the field.
// -----------------------------------------------------------------------------

void TestKnockoutProviderStateCycleAndEveryField() {
    ResetKnockoutStubWarnForTesting();  // fresh warn for this probe

    // =================================================================
    // State-cycle walk: Idle → KnockedOut → Idle, asserting each step.
    // =================================================================
    {
        KnockoutProvider provider;
        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;

        // Step 1: priming frame at origin — still Idle, tracking_ flips on.
        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());
        EXPECT_NEAR(provider.state().time_remaining_s, 0.0, 1e-9);
        // Player mode is left at PlayerState_Reset's default
        // (PlayerMode::Standing) — the provider does NOT touch it on a
        // non-trigger frame. Pin the untouched-mode contract here.
        EXPECT_EQ(static_cast<unsigned>(player.mode),
                  static_cast<unsigned>(PlayerMode::Standing));

        // Step 2: tiny move within threshold — still not triggered, mode
        // still untouched.
        player.feet_position = Vec3{0.05, 0, 0};  // 3.125 m/s; below 8
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());
        EXPECT_EQ(static_cast<unsigned>(player.mode),
                  static_cast<unsigned>(PlayerMode::Standing));

        // Step 3: huge move past threshold → Standing → KnockedOut.
        // Use 0.7 m which is above the 12 m/s velocity threshold for
        // the 16 ms tick but below the 1.0 m default teleport re-prime
        // distance — so the velocity branch fires (not the teleport
        // re-prime branch).
        player.feet_position = Vec3{0.75, 0, 0};  // 47 m/s
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());
        EXPECT_TRUE(provider.state().time_remaining_s > 0.0);
        EXPECT_EQ(static_cast<unsigned>(player.mode),
                  static_cast<unsigned>(PlayerMode::KnockedOut));

        // Step 4: while knocked out, Tick counts down. Player can't
        // re-trigger while knocked out — another huge jump should NOT
        // reset the timer. mode stays KnockedOut.
        const double t_before = provider.state().time_remaining_s;
        player.feet_position = Vec3{1000, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());
        EXPECT_TRUE(provider.state().time_remaining_s < t_before);
        EXPECT_EQ(static_cast<unsigned>(player.mode),
                  static_cast<unsigned>(PlayerMode::KnockedOut));

        // Step 5: tick down the full recovery duration in one shot —
        // default 8s, so a 9s tick crosses the boundary. mode flips
        // back to Idle (the provider's recovery contract).
        provider.Tick(player, 9.0);
        EXPECT_FALSE(provider.isKnockedOut());
        EXPECT_NEAR(provider.state().time_remaining_s, 0.0, 1e-9);
        EXPECT_EQ(static_cast<unsigned>(player.mode),
                  static_cast<unsigned>(PlayerMode::Idle));
    }

    // =================================================================
    // Per-field exhaustive block.
    // =================================================================

    // ---- velocity_change_threshold_mps → gates trigger ----
    {
        KnockoutProvider provider;
        KnockoutConfig cfg = provider.config();
        cfg.velocity_change_threshold_mps = 100.0;  // raise bar very high
        cfg.teleport_reprime_distance_m = 100.0;   // disable teleport gate
        provider.SetConfig(cfg);

        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;
        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);  // prime

        // Move "only" 0.5 m → 31 m/s, below the 100 m/s threshold →
        // no trigger. With default threshold (12 m/s) the same jump
        // WOULD have triggered.
        player.feet_position = Vec3{0.5, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());

        // Now move 10 m → 625 m/s, above the raised threshold →
        // trigger. (Teleport gate is disabled so the 10 m jump is
        // treated as a real per-frame displacement rather than as a
        // programmatic position reset.)
        player.feet_position = Vec3{10, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());
    }

    // ---- recovery_seconds → sets the timer length ----
    {
        KnockoutProvider provider;
        KnockoutConfig cfg = provider.config();
        cfg.recovery_seconds = 2.0;            // short recovery
        cfg.teleport_reprime_distance_m = 100.0;  // disable teleport gate
        provider.SetConfig(cfg);

        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;
        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);  // prime

        // Trigger — 10 m jump at dt=16 ms = 625 m/s, above the 12 m/s
        // default velocity threshold. (Teleport gate is disabled so
        // the velocity branch fires, not the re-prime branch.)
        player.feet_position = Vec3{10, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());
        // Timer was initialised to cfg.recovery_seconds.
        EXPECT_NEAR(provider.state().time_remaining_s, 2.0, 1e-6);

        // After 1.5 s of small movement (knocked-out phase doesn't
        // re-trigger), still knocked out.
        provider.Tick(player, 1.5);
        EXPECT_TRUE(provider.isKnockedOut());
        EXPECT_TRUE(provider.state().time_remaining_s > 0.0);

        // After another 1.0 s (total 2.5 s > recovery_seconds) → idle.
        provider.Tick(player, 1.0);
        EXPECT_FALSE(provider.isKnockedOut());
    }

    // ---- impact_direction → opposite of displacement ----
    {
        KnockoutProvider provider;
        KnockoutConfig cfg = provider.config();
        cfg.teleport_reprime_distance_m = 100.0;  // disable teleport gate
        provider.SetConfig(cfg);

        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;
        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);  // prime

        // Move in +X direction → impact came from -X (player was
        // pushed forward). Use 0.7 m so the velocity branch fires
        // instead of the (default, but disabled in this case) teleport
        // re-prime branch.
        player.feet_position = Vec3{0.7, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());
        // impact_direction.x should be negative (opposite of +X push).
        EXPECT_TRUE(provider.state().impact_direction.x < -0.5);
        // Normalised to unit length.
        const double len2 = provider.state().impact_direction.x *
                                  provider.state().impact_direction.x +
                              provider.state().impact_direction.z *
                                  provider.state().impact_direction.z;
        EXPECT_NEAR(len2, 1.0, 1e-6);
        // y component should be 0 (no vertical displacement).
        EXPECT_NEAR(provider.state().impact_direction.y, 0.0, 1e-9);
    }

    // ---- player.has_feet == false → Tick is no-op ----
    {
        KnockoutProvider provider;
        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = false;
        player.feet_position = Vec3{0, 0, 0};

        // Huge jump with has_feet=false must NOT trigger.
        player.feet_position = Vec3{100, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());
    }

    // ---- dt <= 0 → Tick is no-op ----
    {
        KnockoutProvider provider;
        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;
        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);  // prime

        // dt = 0: no trigger.
        player.feet_position = Vec3{100, 0, 0};
        provider.Tick(player, 0.0);
        EXPECT_FALSE(provider.isKnockedOut());

        // dt < 0: no trigger.
        player.feet_position = Vec3{100, 0, 0};
        provider.Tick(player, -0.016);
        EXPECT_FALSE(provider.isKnockedOut());
    }
}

// -----------------------------------------------------------------------------
// Teleport re-prime contract.
//
// The KnockoutProvider's velocity check fires for any per-frame
// displacement over `cfg.velocity_change_threshold_mps * dt`. When
// the FSM snaps the player to a new world anchor (Driving ↔ Walking
// transitions call PlacePlayerAtDoor / PlacePlayerAtSeat, which set
// player.feet_position to the door / seat world coordinate in a
// single Tick), that distance is a programmatic reset, not a
// pedestrian-vs-vehicle impact. The teleport re-prime branch
// re-primes last_position_ to the current coordinate and returns
// without firing the velocity trigger when dist² ≥
// cfg.teleport_reprime_distance_m². The thresholds default to
// velocity_change_threshold_mps = 12.0 (so a normal run / walk
// never trips it) and teleport_reprime_distance_m = 1.0 (so a
// 1.15 m–11.5 m per-frame snap during a cabin transition does
// NOT fire a knock-out). Both tunables live in KnockoutConfig, so
// a future M10 lander can lower one or both when real collision
// normals arrive from PhysX.
//
// Contract under test:
//
//   1. A position jump above the teleport threshold re-primes the
//      baseline without firing the knock-out.
//   2. Subsequent Ticks at the new position stay not-knocked-out
//      unless a real (sub-teleport) velocity trigger fires.
//   3. A real sub-teleport car impact still fires the knock-out —
//      the teleport re-prime is a guard, not a kill-switch.
//   4. Configure the teleport distance to a value greater than the
//      test jump → the velocity path takes over (regression: the
//      threshold must keep working independent of the teleport
//      re-prime).
// -----------------------------------------------------------------------------

void TestKnockoutTeleportReprime() {
    ResetKnockoutStubWarnForTesting();  // fresh warn for this probe

    // ---- Case 1: position jump above teleport threshold → re-prime, no KO ----
    {
        KnockoutProvider provider;
        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;

        // Prime at origin.
        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());

        // Snap 2.0 m in one frame (above the 1.0 m default teleport
        // threshold; matches the 1.15 m – 11.5 m deltas we observed
        // during FSM transitions in the live build).
        player.feet_position = Vec3{2.0, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());

        // Steady at the new position — must remain not knocked out.
        player.feet_position = Vec3{2.0, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());

        // A real sub-teleport car impact at the new position —
        // 0.7 m / 16 ms = 43.75 m/s — must still fire the velocity
        // branch (the teleport re-prime is a guard, not a kill-switch).
        player.feet_position = Vec3{2.7, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());
    }

    // ---- Case 2: setting teleport threshold above the test jump ----
    // → the velocity branch must take over even at 12 m/s.
    {
        KnockoutProvider provider;
        KnockoutConfig cfg = provider.config();
        cfg.teleport_reprime_distance_m = 100.0;  // disable teleport gate
        provider.SetConfig(cfg);

        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;

        // Prime at origin.
        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);

        // 0.7 m / 16 ms = 43.75 m/s — above the 12 m/s default
        // velocity threshold. With the teleport gate disabled, this
        // must fire via the velocity path.
        player.feet_position = Vec3{0.7, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());
    }

    // ---- Case 3: vertical snap (FSM PlacePlayerAtDoor y axis) ----
    {
        KnockoutProvider provider;
        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;

        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);

        // 50 m down — clear teleport (>= 1 m). Without re-prime, this
        // would fire the velocity check at 3,125 m/s.
        player.feet_position = Vec3{0, -50, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());

        // Steady at the fallen coordinate.
        player.feet_position = Vec3{0, -50, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());
    }
}

// -----------------------------------------------------------------------------
// Recovery → re-prime contract.
//
// The KnockoutProvider's velocity check compares against last_position_
// which was captured at the end of the previous Tick. When the player
// recovers from a knock-out (timer expires), last_position_ would
// otherwise still hold the pre-knockout coordinate — leaving a single
// Tick-sized window where any movement the game applied during the
// recovery window (impact sliding, terrain collision, etc.) would be
// compared against an arbitrarily old reference and could trip the
// threshold for a spurious re-knockout.
//
// Contract under test:
//
//   1. First Tick after recovery must NOT trigger a knock-out, even if
//      the player moved a huge distance during the recovery window.
//   2. Subsequent Ticks still apply the velocity check correctly —
//      once the provider has re-primed, a real subsequent impact DOES
//      trigger.
//   3. The recovery re-prime contract applies regardless of how the
//      player moved during recovery (sliding X, falling Y, both).
// -----------------------------------------------------------------------------

void TestKnockoutRecoveryResetsLastPositionBaseline() {
    ResetKnockoutStubWarnForTesting();  // fresh warn for this probe

    // ---- Case 1: player slides horizontally during recovery ----
    {
        KnockoutProvider provider;
        KnockoutConfig cfg = provider.config();
        cfg.recovery_seconds = 0.5;  // short recovery for the test
        provider.SetConfig(cfg);

        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;

        // Prime at the origin.
        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());

        // Hit by a car (0.7 m / 16 ms = 43.75 m/s — above the 12 m/s
        // velocity threshold, below the 1 m teleport re-prime).
        player.feet_position = Vec3{0.7, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());

        // During the 0.5 s recovery window, the game slides the player
        // ~1 m further (e.g. impact sliding). We model that by
        // mutating player.feet_position during the knocked-out ticks
        // (the knocked-out branch doesn't read feet_position for any
        // check, so any value is safe).
        player.feet_position = Vec3{1.7, 0, 0};
        // 31 × 0.016 = 0.496 s < 0.5 s recovery — last iteration still
        // knocked out. 32nd Tick crosses the recovery boundary (cumulative
        // 0.512 s, time_remaining_s ≤ 0).
        for (int i = 0; i < 31; ++i) {
            EXPECT_TRUE(provider.isKnockedOut());  // pre-Tick check
            provider.Tick(player, 0.016);
        }
        EXPECT_TRUE(provider.isKnockedOut());  // 31st Tick: still in
        provider.Tick(player, 0.016);  // 32nd: crosses → recovered
        EXPECT_FALSE(provider.isKnockedOut());

        // The bug we're fixing: if the provider still compares the
        // next position against the pre-knockout (10, 0, 0), the
        // 10 m displacement accumulated during recovery
        // (10 → 20) easily trips the 8 m/s threshold for the very
        // first post-recovery Tick. The fix re-primes via
        // tracking_ = false on the recovery branch, so the first
        // post-recovery Tick is the priming frame (no velocity
        // check) and the SECOND post-recovery Tick sees a fresh
        // baseline.

        // First post-recovery Tick at the post-slide position —
        // should NOT re-knockout (this is the priming frame).
        player.feet_position = Vec3{1.7, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());

        // Steady at the post-slide position — should remain not knocked
        // out (no displacement).
        player.feet_position = Vec3{1.7, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());

        // Now simulate a REAL second car impact after recovery — the
        // velocity check should fire normally. Move 0.7 m instantaneously
        // to land past the threshold but below the teleport threshold.
        player.feet_position = Vec3{2.4, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());
    }

    // ---- Case 2: player falls vertically during recovery ----
    {
        KnockoutProvider provider;
        KnockoutConfig cfg = provider.config();
        cfg.recovery_seconds = 0.3;
        provider.SetConfig(cfg);

        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;

        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);  // prime
        // Trigger horizontal: 0.7 m / 16 ms = 43.75 m/s.
        player.feet_position = Vec3{0.7, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());

        // During recovery, the player falls 50 m straight down.
        player.feet_position = Vec3{10, -50, 0};
        // 18 × 0.016 = 0.288 s < 0.3 s recovery — last iteration still
        // knocked out. 19th Tick crosses (cumulative 0.304 s).
        for (int i = 0; i < 18; ++i) {
            EXPECT_TRUE(provider.isKnockedOut());  // pre-Tick check
            provider.Tick(player, 0.016);
        }
        EXPECT_TRUE(provider.isKnockedOut());  // 18th Tick: still in
        provider.Tick(player, 0.016);  // 19th: crosses → recovered
        EXPECT_FALSE(provider.isKnockedOut());

        // The first post-recovery Tick at the fallen position must
        // NOT spuriously re-knockout — without the fix, the 50 m
        // Y-displacement since the pre-knockout (10, 0, 0) would
        // trip the threshold on this single tick.
        player.feet_position = Vec3{10, -50, 0};
        provider.Tick(player, 0.016);
        EXPECT_FALSE(provider.isKnockedOut());
    }

    // ---- Case 3: short recovery with no movement still works ----
    // Negative control: the recovery-resets-last_position fix must not
    // break the "no movement during recovery" path. Recovery → Tick →
    // Tick should leave the player not-knocked-out indefinitely.
    {
        KnockoutProvider provider;
        KnockoutConfig cfg = provider.config();
        cfg.recovery_seconds = 0.2;
        provider.SetConfig(cfg);

        PlayerState player;
        PlayerState_Reset(player);
        player.has_feet = true;

        player.feet_position = Vec3{0, 0, 0};
        provider.Tick(player, 0.016);  // prime
        // Trigger: 0.7 m / 16 ms = 43.75 m/s, above the 12 m/s default
        // velocity threshold but below the 1 m default teleport re-prime.
        player.feet_position = Vec3{0.7, 0, 0};
        provider.Tick(player, 0.016);
        EXPECT_TRUE(provider.isKnockedOut());

        // Stationary during recovery at the impact point.
        player.feet_position = Vec3{0.7, 0, 0};
        provider.Tick(player, 0.25);  // crosses the boundary
        EXPECT_FALSE(provider.isKnockedOut());

        // Stay still for 100 frames — must not re-trigger.
        for (int i = 0; i < 100; ++i) {
            provider.Tick(player, 0.016);
            EXPECT_FALSE(provider.isKnockedOut());
        }
    }
}

}  // namespace walkercore