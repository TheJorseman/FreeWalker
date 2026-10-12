// WalkerCore — Polish providers implementation (M7).
//
// Stubs and basic math. Real impls require:
//   - HeadTrackerProvider: FreeTrackClient.dll (TrackIR) and opentrack UDP.
//     Until they're available, the mouse-only path uses the InputHook.
//   - ShadowProvider: game-side light direction read from game memory.
//     Stub uses a fixed overhead sun direction.
//   - FlashlightProvider: game-side point light creation. Stub writes the
//     state but does not inject a light source.
//   - DoorAnimProvider: game-side skeletal animation. Stub tween only.

#include "walkercore/PolishProviders.h"

#include <math.h>

#include "walkercore/Log.h"

namespace walkercore {

namespace {

// File-scope last-name sentinel so the test-only reset hook below can
// reset it back to nullptr. Production code never observes this
// directly — WarnStub() hides the static from its own callers.
const char *g_last_warned_name = nullptr;

void WarnStub(const char *name) {
    if (g_last_warned_name == name) return;
    g_last_warned_name = name;
    WALKERCORE_LOG_WARN(
        "PolishProvider %s: stub active — output of %s requires real impl "
        "in M7 lander", name, name);
}

}  // namespace

extern "C" void ResetPolishStubWarnForTesting() {
    // Clear the per-name cache so the next WarnStub() call for any
    // real source name emits a fresh warning. Production code never
    // calls this — the one-shot warn flag is meant to fire exactly
    // once per provider-name per process.
    g_last_warned_name = nullptr;
}

// =============================================================================
// HeadTrackerProvider
// =============================================================================

HeadTrackerProvider::HeadTrackerProvider()
    : installed_(false),
      yaw_accum_(0.0f),
      pitch_accum_(0.0f),
      smooth_yaw_(0.0f),
      smooth_pitch_(0.0f),
      has_data_(false),
      last_source_(HeadTrackerSource::Mouse) {}

HeadTrackerProvider::~HeadTrackerProvider() {
    if (installed_) Uninstall();
}

void HeadTrackerProvider::SetConfig(const HeadTrackerConfig &cfg) {
    cfg_ = cfg;
}

bool HeadTrackerProvider::Install() {
    if (installed_) return false;
    installed_ = true;
    WarnStub("HeadTrackerProvider");
    return true;
}

void HeadTrackerProvider::Uninstall() {
    installed_ = false;
}

void HeadTrackerProvider::FeedSample(HeadTrackerSource source,
                                      const HeadTrackerSample &s) {
    float weight = 0.0f;
    switch (source) {
        case HeadTrackerSource::Mouse:
            if (!cfg_.use_mouse) return;
            weight = cfg_.mouse_sensitivity_rad_per_unit;
            break;
        case HeadTrackerSource::TrackIR:
            if (!cfg_.use_trackir) return;
            weight = cfg_.trackir_sensitivity_rad_per_unit;
            WarnStub("HeadTracker/TrackIR");
            break;
        case HeadTrackerSource::Opentrack:
            if (!cfg_.use_opentrack) return;
            weight = cfg_.opentrack_sensitivity_rad_per_unit;
            WarnStub("HeadTracker/Opentrack");
            break;
        default:
            return;
    }
    yaw_accum_ += s.yaw_delta_rad * weight;
    pitch_accum_ += s.pitch_delta_rad * weight;
    has_data_ = true;
    last_source_ = source;
}

HeadTrackerState HeadTrackerProvider::Sample() {
    // Exponential moving average toward the CUMULATIVE target (yaw_accum_),
    // not the per-frame delta. This way the smoothed value approaches
    // the head's held position over time and stays there when input
    // stops — the camera does not drift back toward the neutral pose.
    // (The previous implementation reset yaw_accum_ to 0 after each
    // Sample, which caused the smoothed value to decay exponentially
    // back to 0 whenever input stopped — wrong for head tracking.)
    const float alpha = cfg_.smoothing_factor;
    smooth_yaw_ += (yaw_accum_ - smooth_yaw_) * (1.0f - alpha);
    smooth_pitch_ += (pitch_accum_ - smooth_pitch_) * (1.0f - alpha);

    HeadTrackerState out;
    out.active = has_data_;
    out.dominant_source = last_source_;
    out.yaw_radians = smooth_yaw_;
    out.pitch_radians = smooth_pitch_;

    // We do NOT reset yaw_accum_ / pitch_accum_ — they hold the
    // cumulative target that the smoothed value chases. If the user's
    // head returns toward neutral, FeedSample receives negative deltas
    // and yaw_accum_ shrinks accordingly, which is what we want.
    return out;
}

HeadTrackerState HeadTrackerProvider::Peek() const {
    // Read-only snapshot. Does NOT advance the EMA. Useful for
    // diagnostic dumps and any consumer that wants the latest
    // smoothed state without disturbing the producer's flow.
    HeadTrackerState out;
    out.active = has_data_;
    out.dominant_source = last_source_;
    out.yaw_radians = smooth_yaw_;
    out.pitch_radians = smooth_pitch_;
    return out;
}

// =============================================================================
// ShadowProvider
// =============================================================================

ShadowProvider::ShadowProvider() {
    // Default sun direction: 45° elevation, due south. Will be overridden
    // when the game-side sun position is available.
    cfg_.sun_direction = Vec3{0.0, 0.7071, -0.7071};
}

ShadowProvider::~ShadowProvider() = default;

void ShadowProvider::SetConfig(const ShadowConfig &cfg) {
    cfg_ = cfg;
}

PlayerShadowState ShadowProvider::Compute(const PlayerState &p) const {
    PlayerShadowState out;
    out.active = p.has_feet;
    // Project the player's feet onto the ground along -sun. Since we don't
    // know ground height, we put the shadow at the player's feet Y. When
    // groundY probes are wired in M7 lander, this becomes the ground Y.
    const Vec3 sun = cfg_.sun_direction;
    const double dx = sun.x;
    const double dy = sun.y;
    const double dz = sun.z;
    if (dy < 1e-3) {
        // Sun is on the horizon; shadow would be infinitely far. Use a
        // safe fallback (overhead sun).
        out.worldCentre = p.feet_position;
    } else {
        const double t = -p.feet_position.y / dy;
        out.worldCentre = Vec3{p.feet_position.x + dx * t,
                                0.0,
                                p.feet_position.z + dz * t};
    }
    out.radius = 0.35f * cfg_.scale;
    out.softness = cfg_.softness;
    out.rgba = cfg_.rgba;
    return out;
}

// =============================================================================
// FlashlightProvider
// =============================================================================

FlashlightProvider::FlashlightProvider() : on_(false) {}
FlashlightProvider::~FlashlightProvider() = default;

void FlashlightProvider::SetOn(bool on) { on_ = on; }
void FlashlightProvider::Toggle() { on_ = !on_; }

FlashlightState FlashlightProvider::Compute(const CameraState &c) const {
    FlashlightState out;
    out.on = on_;
    out.origin = c.eye_position;
    // Forward = (sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch))
    // — same convention as CameraState_BuildViewMatrix.
    const float cy = cosf(c.yaw_radians);
    const float sy = sinf(c.yaw_radians);
    const float cp = cosf(c.pitch_radians);
    const float sp = sinf(c.pitch_radians);
    out.direction = Vec3{sy * cp, sp, -cy * cp};
    out.rgba = 0xFFFFFFFFu;
    out.inner_cone_rad = 0.35f;
    out.outer_cone_rad = 0.65f;
    out.intensity = on_ ? 1.0f : 0.0f;
    return out;
}

// =============================================================================
// DoorAnimProvider
// =============================================================================

DoorAnimProvider::DoorAnimProvider() = default;
DoorAnimProvider::~DoorAnimProvider() = default;

void DoorAnimProvider::AnimateTo(float target_angle, float duration_s) {
    start_angle_ = angle_radians_;
    target_angle_ = target_angle;
    duration_ = duration_s > 1e-6f ? duration_s : 1e-6f;
    t_ = 0.0f;
    animating_ = true;
}

void DoorAnimProvider::Tick(double dt) {
    if (!animating_) return;
    t_ = static_cast<float>(static_cast<double>(t_) + dt / static_cast<double>(duration_));
    if (t_ >= 1.0f) {
        t_ = 1.0f;
        angle_radians_ = target_angle_;
        animating_ = false;
        return;
    }
    // Smoothstep easing: 3t² - 2t³.
    const float e = static_cast<float>(t_ * t_ * (3.0f - 2.0f * t_));
    angle_radians_ = start_angle_ + (target_angle_ - start_angle_) * e;
}

}  // namespace walkercore