// WalkerCore — DoorAnim tests.

#include "TestHarness.h"

#include <math.h>

#include "walkercore/PolishProviders.h"

namespace walkercore {

void TestDoorAnimEasing() {
    DoorAnimProvider anim;
    EXPECT_FALSE(anim.state().animating);

    anim.AnimateTo(1.5708f /* 90° */, 0.5f);
    EXPECT_TRUE(anim.state().animating);
    EXPECT_NEAR(anim.state().angle_radians, 0.0f, 1e-6);

    // Tick halfway through. With smoothstep (3t² - 2t³) at t=0.5 → 0.5.
    // Half the duration is 0.25 s; tick 0.25 s.
    anim.Tick(0.25);
    EXPECT_TRUE(anim.state().animating);
    EXPECT_NEAR(anim.state().angle_radians, 0.7854f /* 45° */, 0.01);

    // Tick the rest.
    anim.Tick(0.3);
    EXPECT_FALSE(anim.state().animating);
    EXPECT_NEAR(anim.state().angle_radians, 1.5708f /* 90° */, 1e-3);
}

// -----------------------------------------------------------------------------
// DoorAnimProvider field-exhaustive probe.
//
// External surface:
//
//   AnimateTo(target_angle, duration_s)
//       Captures start_angle_ from the CURRENT angle_radians_ (not from
//       the previous target_angle_ — re-targeting mid-tween restarts
//       from wherever the door currently is). duration_s is clamped to
//       a 1e-6 s minimum so the next Tick can't divide by zero.
//   Tick(dt)
//       Advances t_ by dt / duration_. Stops at t=1 and snaps
//       angle_radians_ to target_angle_; sets animating_=false.
//       No-op when not animating.
//   Stop()
//       Sets animating_=false. angle_radians_ stays where it was.
//       Subsequent Tick is a no-op (early return on !animating_).
//
// State outputs:
//
//   state().angle_radians  -- current angle (between start and target
//                             during a tween, exactly target at the end)
//   state().animating      -- bool: true while a tween is in flight
//
// Each contract below pins one observable behaviour. The probe uses
// a fresh DoorAnimProvider per block so residual state from one
// case can't perturb another.
// -----------------------------------------------------------------------------

void TestDoorAnimProviderEveryFieldHasObservableEffect() {
    // ---- Case 1: target_angle → final angle_radians_ lands at target ----
    {
        DoorAnimProvider anim;
        anim.AnimateTo(2.5f, 0.05f);  // 2.5 rad ≈ 143°
        anim.Tick(0.06);               // overshoot to guarantee completion
        EXPECT_FALSE(anim.state().animating);
        EXPECT_NEAR(anim.state().angle_radians, 2.5f, 1e-4);
    }
    {
        // Negative target — door swings the other way.
        DoorAnimProvider anim;
        anim.AnimateTo(-1.0f, 0.05f);
        anim.Tick(0.06);
        EXPECT_NEAR(anim.state().angle_radians, -1.0f, 1e-4);
    }

    // ---- Case 2: duration_s → time to reach target is proportional ----
    {
        DoorAnimProvider slow;
        slow.AnimateTo(1.0f, 0.5f);  // 500 ms
        slow.Tick(0.1);              // 20 %
        const float slow_20 = slow.state().angle_radians;
        EXPECT_TRUE(slow.state().animating);
        EXPECT_TRUE(slow_20 > 0.0f);
        EXPECT_TRUE(slow_20 < 1.0f);

        DoorAnimProvider fast;
        fast.AnimateTo(1.0f, 0.05f);  // 50 ms (10× shorter)
        fast.Tick(0.01);              // 20 % (same fraction of duration)
        const float fast_20 = fast.state().angle_radians;
        EXPECT_TRUE(fast.state().animating);
        // Same t → same smoothstep value → same fraction of target.
        EXPECT_NEAR(slow_20, fast_20, 1e-3);
    }

    // ---- Case 3: AnimateTo mid-tween captures start from current angle ----
    {
        DoorAnimProvider anim;
        // First tween: 0 → 1.5 rad over 0.5 s.
        anim.AnimateTo(1.5f, 0.5f);
        anim.Tick(0.25);  // halfway; angle ~ 0.75 (smoothstep at t=0.5 → 0.5)
        const float mid_angle = anim.state().angle_radians;
        EXPECT_TRUE(mid_angle > 0.5f);
        EXPECT_TRUE(mid_angle < 1.0f);

        // Re-target: 0.5 rad, same long duration. start_angle_ should
        // be captured from mid_angle (NOT 1.5 — the previous target).
        anim.AnimateTo(0.5f, 0.5f);
        // Right after AnimateTo, t resets to 0 → angle_radians_ = start.
        EXPECT_NEAR(anim.state().angle_radians, mid_angle, 1e-5);

        // Halfway through the new tween: angle = lerp(mid, 0.5, 0.5).
        anim.Tick(0.25);
        const float new_half = anim.state().angle_radians;
        EXPECT_TRUE(new_half < mid_angle);  // moving toward 0.5
        EXPECT_TRUE(new_half > 0.5f);       // but didn't overshoot
    }

    // ---- Case 4: duration_s <= 0 clamps to 1e-6 (no divide-by-zero) ----
    {
        DoorAnimProvider anim;
        anim.AnimateTo(1.0f, 0.0f);  // zero duration
        // Even a tiny dt advances t past 1.0 immediately because
        // duration_ was clamped to 1e-6. dt = 0.001 / 1e-6 = 1000.
        anim.Tick(0.001);
        EXPECT_FALSE(anim.state().animating);
        EXPECT_NEAR(anim.state().angle_radians, 1.0f, 1e-4);

        // Negative duration clamped too.
        DoorAnimProvider anim2;
        anim2.AnimateTo(0.7f, -5.0f);
        anim2.Tick(0.001);
        EXPECT_FALSE(anim2.state().animating);
        EXPECT_NEAR(anim2.state().angle_radians, 0.7f, 1e-4);
    }

    // ---- Case 5: Tick with dt = 0 → no progress, still animating ----
    {
        DoorAnimProvider anim;
        anim.AnimateTo(1.0f, 0.5f);
        const float before = anim.state().angle_radians;
        anim.Tick(0.0);  // zero dt
        EXPECT_TRUE(anim.state().animating);
        EXPECT_NEAR(anim.state().angle_radians, before, 1e-6);
    }

    // ---- Case 6: Tick when NOT animating → no-op ----
    {
        DoorAnimProvider anim;
        // Never call AnimateTo; animating_ stays false.
        const float before = anim.state().angle_radians;  // 0
        anim.Tick(1.0);  // huge dt
        EXPECT_FALSE(anim.state().animating);
        EXPECT_NEAR(anim.state().angle_radians, before, 1e-9);
    }

    // ---- Case 7: Stop() preserves angle, kills animating ----
    {
        DoorAnimProvider anim;
        anim.AnimateTo(2.0f, 0.5f);
        anim.Tick(0.25);  // mid-tween
        const float mid = anim.state().angle_radians;
        EXPECT_TRUE(anim.state().animating);

        anim.Stop();
        EXPECT_FALSE(anim.state().animating);
        EXPECT_NEAR(anim.state().angle_radians, mid, 1e-6);  // preserved

        // Subsequent Tick is a no-op (animating_ is false).
        anim.Tick(10.0);
        EXPECT_FALSE(anim.state().animating);
        EXPECT_NEAR(anim.state().angle_radians, mid, 1e-6);
    }

    // ---- Case 8: completion snaps angle exactly to target ----
    {
        DoorAnimProvider anim;
        anim.AnimateTo(1.2f, 0.1f);
        // Tick a tiny amount first — angle moves.
        anim.Tick(0.04);  // 40 % in
        const float before_snap = anim.state().angle_radians;
        EXPECT_TRUE(before_snap > 0.0f);
        EXPECT_TRUE(before_snap < 1.2f);

        // Now tick past the end.
        anim.Tick(0.2);
        EXPECT_FALSE(anim.state().animating);
        EXPECT_NEAR(anim.state().angle_radians, 1.2f, 1e-6);  // exact target
    }
}

}  // namespace walkercore