// WalkerCore — MovementConfig tests.

#include "TestHarness.h"
#include "walkercore/MovementConfig.h"

namespace walkercore {

void TestMovementConfigDefaults() {
    MovementConfig c;
    // Sanity ranges for an adult walking simulator.
    EXPECT_TRUE(c.walkSpeedMps > 0.5f && c.walkSpeedMps < 3.0f);
    EXPECT_TRUE(c.runSpeedMps > c.walkSpeedMps);
    EXPECT_TRUE(c.crouchSpeedMps > 0.0f && c.crouchSpeedMps < c.walkSpeedMps);
    EXPECT_TRUE(c.gravityMps2 > 0.0f && c.gravityMps2 < 20.0f);
    EXPECT_TRUE(c.jumpInitialVelocityMps > 0.0f);
    EXPECT_TRUE(c.mouseSensitivityRadPerUnit > 0.0f);
    EXPECT_TRUE(c.eyeHeightStandingM > c.eyeHeightCrouchingM);
    EXPECT_TRUE(c.headBobVerticalAmpM >= 0.0f);
    EXPECT_TRUE(c.headBobLateralAmpM >= 0.0f);
    EXPECT_TRUE(c.headBobFrequencyHz > 0.0f);
    EXPECT_TRUE(c.maxStepUpM >= 0.0f);
    EXPECT_TRUE(c.maxSlopeAngleRad > 0.0f && c.maxSlopeAngleRad < 1.6f);
    // Vertical-velocity safety cap. Must be > 0 (PhaseIntegrateMovement
    // divides by it conceptually via the clamp) and ≤ gravity × a
    // human-skydiving-relevant upper bound. The default of 9.81
    // (one second of gravity) lands in the middle.
    EXPECT_TRUE(c.vertical_velocity_max_mps > 0.0f);
    EXPECT_TRUE(c.vertical_velocity_max_mps <= 50.0f);
}

}  // namespace walkercore