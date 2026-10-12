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

    // Safety cap on |vertical_velocity_| applied every Walking tick in
    // PhaseIntegrateMovement after the gravity step. The cap exists
    // because the M3 collision adapter is currently a stub
    // (PhysXAdapter logs "no collision geometry loaded" once per
    // session) and PhaseResolveCollision early-returns when world_ is
    // null — without a ground/floor hit, gravity integrates without
    // bound across every Walking tick, and the residual vy persists
    // across FSM transitions (Driving / TransitionIn / TransitionOut
    // all gate RunOnce, so the value survives the trip back into the
    // cabin). When the FSM re-enters Walking, the stale vy drives a
    // per-frame |dy| that the velocity-change knockout check sees as a
    // 18–28 m/s "impact" within ~30 ms of the door-snap frame
    // (log evidence on ATS 1.61.3.1s with the in-game DLL build at
    // commit 79be7b6 — see the M10-stub lander notes). The cap forces
    // terminal vertical velocity to 9.81 m/s so single-frame |dy| stays
    // at 9.81 × dt ≤ 0.167 m, well below the 12 m/s velocity threshold
    // (which evaluates dist/dt against dist² / dt² ≥ 0.037 m²). 9.81
    // matches the magnitude vy reaches after one second of gravity —
    // a representative "fast fall" without pretending to model sky-
    // diving terminal velocities we don't actually integrate. INI-
    // overridable via the M6 SettingsStore once the bind lands.
    float vertical_velocity_max_mps = 9.81f;

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