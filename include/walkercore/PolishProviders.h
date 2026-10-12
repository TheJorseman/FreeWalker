// WalkerCore — polish feature providers (M7).
//
// Header-only declarations for the four M7 providers. Real implementations
// live in src/provider/; until they land the stubs in src/workflow/ are
// good enough to verify the render-layer wiring.

#ifndef WALKERCORE_POLISH_PROVIDERS_H
#define WALKERCORE_POLISH_PROVIDERS_H

#include "walkercore/PlayerState.h"
#include "walkercore/PolishFeatures.h"
#include "walkercore/TruckDoorConfig.h"

namespace walkercore {

// -----------------------------------------------------------------------------
// Head tracker provider.
// -----------------------------------------------------------------------------

// Configuration. M6 INI overrides these.
struct HeadTrackerConfig {
    // Source enable flags. mouse defaults to false because the
    // FrameLoop's PhaseApplyMouseLook is already the canonical source
    // of yaw/pitch deltas while no real TrackIR/opentrack feed is
    // connected. The M7 lander flips this to true (or the user does,
    // via INI) once it has its own mouse-feed pipeline that runs in
    // parallel to the FrameLoop's path.
    bool use_mouse = false;
    bool use_trackir = false;
    bool use_opentrack = false;

    // Per-source sensitivity (radians per raw input unit).
    float mouse_sensitivity_rad_per_unit = 0.0022f;
    float trackir_sensitivity_rad_per_unit = 1.0f;
    float opentrack_sensitivity_rad_per_unit = 1.0f;

    // Low-pass smoothing factor. 0 = no smoothing (raw delta applied),
    // 1 = full smoothing (no movement). 0.85 is a good default.
    float smoothing_factor = 0.85f;
};

class HeadTrackerProvider {
public:
    HeadTrackerProvider();
    ~HeadTrackerProvider();

    HeadTrackerProvider(const HeadTrackerProvider &) = delete;
    HeadTrackerProvider &operator=(const HeadTrackerProvider &) = delete;

    void SetConfig(const HeadTrackerConfig &cfg);
    const HeadTrackerConfig &config() const { return cfg_; }

    bool Install();
    void Uninstall();

    // Feed a sample from an external source (mouse, TrackIR, opentrack).
    void FeedSample(HeadTrackerSource source, const HeadTrackerSample &s);

    // Drain into out. The cumulative-target accumulator
    // (yaw_accum_ / pitch_accum_) is NOT reset by this call — the
    // smoothed output converges on the held position over time and
    // STAYS there when input stops. Resetting would cause the camera
    // to drift back toward the neutral pose on every idle frame, which
    // is the bug this provider was rewritten to fix. Negative deltas
    // (the head returns toward neutral) shrink the cumulative target
    // through subsequent FeedSample calls.
    HeadTrackerState Sample();

    // Read-only snapshot of the latest smoothed state WITHOUT advancing
    // the EMA. Use this from diagnostic dumps and any read-only consumer
    // that should not perturb the producer's internal state. The M7 lander
    // still drives Sample() from its frame loop.
    HeadTrackerState Peek() const;

private:
    HeadTrackerConfig cfg_;
    bool installed_;
    float yaw_accum_;
    float pitch_accum_;
    float smooth_yaw_;
    float smooth_pitch_;
    bool has_data_;
    HeadTrackerSource last_source_;
};

// -----------------------------------------------------------------------------
// Player shadow provider.
// -----------------------------------------------------------------------------

struct ShadowConfig {
    // Sun direction in world space. Normalised. Default is overhead at
    // 45° from +Y (so the shadow is visible but not too long).
    Vec3 sun_direction;
    // Shadow scale relative to the player capsule radius.
    float scale = 1.2f;
    // Soft edge width.
    float softness = 0.15f;
    // Default colour.
    uint32_t rgba = 0x80000000u;  // 50% black
};

class ShadowProvider {
public:
    ShadowProvider();
    ~ShadowProvider();

    void SetConfig(const ShadowConfig &cfg);
    const ShadowConfig &config() const { return cfg_; }

    // Compute the shadow state for the given player position. Cheap.
    PlayerShadowState Compute(const PlayerState &p) const;

private:
    ShadowConfig cfg_;
};

// -----------------------------------------------------------------------------
// Flashlight provider.
// -----------------------------------------------------------------------------

struct FlashlightState {
    bool on;
    // Origin and direction in world space. Direction is normalised.
    Vec3 origin;
    Vec3 direction;
    // Colour (RGBA).
    uint32_t rgba;
    // Cone angles (radians).
    float inner_cone_rad;
    float outer_cone_rad;
    // Intensity (0..1).
    float intensity;
};

class FlashlightProvider {
public:
    FlashlightProvider();
    ~FlashlightProvider();

    // Toggle / set explicitly.
    void SetOn(bool on);
    void Toggle();
    bool isOn() const { return on_; }

    // Compute the current state from the camera. The flashlight lives at
    // the camera origin, points along forward.
    FlashlightState Compute(const CameraState &c) const;

private:
    bool on_;
};

// -----------------------------------------------------------------------------
// Door animation provider (M7).
// -----------------------------------------------------------------------------

struct DoorAnimState {
    float angle_radians;   // current door swing angle
    bool animating;
};

class DoorAnimProvider {
public:
    DoorAnimProvider();
    ~DoorAnimProvider();

    // Start an animation toward target_angle over duration_s seconds.
    void AnimateTo(float target_angle, float duration_s);

    // Advance one frame. dt in seconds.
    void Tick(double dt);

    void Stop() { animating_ = false; }

    DoorAnimState state() const {
        return DoorAnimState{angle_radians_, animating_};
    }

private:
    float angle_radians_ = 0.0f;
    float start_angle_ = 0.0f;
    float target_angle_ = 0.0f;
    float t_ = 0.0f;
    float duration_ = 0.0f;
    bool animating_ = false;
};

}  // namespace walkercore

#endif  // WALKERCORE_POLISH_PROVIDERS_H