// WalkerCore — Movement configuration.
//
// Numeric parameters that control how the walker moves. Defaults match
// TMRealWalk's published values (which the surveyed DLL ships out of the
// box). All knobs are overridable from the M6 INI.
//
// Units:
//   - Speeds in metres per second.
//   - Angles in radians.
//   - Sensitivity in radians per raw input unit (effectively per pixel of
//     raw mouse delta — we use raw input not deltas-from-Windows, so each
//     integer tick is ~1/1200 of the screen at 800 DPI mice).

#ifndef WALKERCORE_MOVEMENT_CONFIG_H
#define WALKERCORE_MOVEMENT_CONFIG_H

namespace walkercore {

struct MovementConfig {
    // Locomotion speeds.
    float walkSpeedMps = 1.6f;       // ~5.8 km/h, normal walking.
    float runSpeedMps = 4.5f;      // ~16 km/h, light run (Shift held).
    float crouchSpeedMps = 0.7f;   // very slow when crouched (Ctrl held).

    // Vertical motion.
    float jumpInitialVelocityMps = 4.5f;   // initial vy when Space pressed.
    float gravityMps2 = 9.81f;             // gravitational accel downward.

    // Mouse look.
    float mouseSensitivityRadPerUnit = 0.0022f;  // radians per raw input unit.

    // Eye height.
    float eyeHeightStandingM = 1.65f;
    float eyeHeightCrouchingM = 0.85f;
    // Crouch transition speed (m/s of height change).
    float crouchBlendMps = 4.0f;

    // Head bob (when walking, not running).
    float headBobVerticalAmpM = 0.025f;     // metres.
    float headBobLateralAmpM = 0.020f;
    float headBobFrequencyHz = 1.7f;        // bob cycles per second at walkSpeed.

    // Step-up — the player can step onto obstacles up to this height without
    // being blocked (smooths out curb / kerb geometry).
    float maxStepUpM = 0.45f;

    // Slope limit (radians). ~50°.
    float maxSlopeAngleRad = 0.87f;
};

}  // namespace walkercore

#endif  // WALKERCORE_MOVEMENT_CONFIG_H