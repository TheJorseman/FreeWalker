// WalkerCore — CameraState domain type.
//
// Holds the camera matrices we set on each D3D11 Present. Three pieces:
//   - eye_position: where the camera is in world space.
//   - view_matrix: world → camera transform (4×4, row-major, float).
//   - projection_matrix: camera → clip-space transform.
//
// WalkerCore does not own a camera manager like the game does. We compute
// the view matrix from PlayerState every frame and push it through the
// D3D11 hook. The game's own camera (free camera, cabin camera, etc.) is
// either paused (we hold the input from it) or replaced (we draw our view
// and bypass theirs).
//
// We use row-major 4×4 matrices in float because D3D11 takes them
// directly with no transposing. World + view are right-handed (same as
// the game's SCS coordinate system).

#ifndef WALKERCORE_CAMERA_STATE_H
#define WALKERCORE_CAMERA_STATE_H

#include <math.h>

#include "walkercore/PlayerState.h"

namespace walkercore {

// 4×4 row-major matrix. Index [row][col].
using Mat4 = float[4][4];

// Identity.
inline void Mat4_Identity(Mat4 &m) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
}

// Build a right-handed view matrix from eye + yaw + pitch (radians).
// Standard "look at" formulation: forward = (sin(yaw)*cos(pitch),
// sin(pitch), cos(yaw)*cos(pitch)); up = (0, 1, 0). The matrix converts
// world-space points to view space (where the camera is at origin looking
// down -Z).
//
// We use Euler YXZ order: yaw around world Y, then pitch around camera X.
void CameraState_BuildViewMatrix(Mat4 &view,
                                 const Vec3 &eye,
                                 float yaw_radians,
                                 float pitch_radians);

// Build the rotation block (rows 0/1/2, cols 0/1/2) of the view matrix
// from yaw + pitch only. Leaves row 3 untouched. Used by
// CameraState_Update so that on a rotation-cache hit only the translation
// row needs to be refreshed per frame.
void CameraState_BuildViewRotation(Mat4 &view,
                                   float yaw_radians,
                                   float pitch_radians);

// Refresh only the translation row (row 3) of view[]. Reads rows 0/1/2
// (the rotation block left in place by BuildViewRotation or a prior
// full BuildViewMatrix). Cheap: three dot products. Eye position
// changes per frame; this is the per-frame minimum.
void CameraState_UpdateViewTranslation(Mat4 &view, const Vec3 &eye);

// Perspective projection matrix with horizontal FOV in radians. near/far
// in meters. Aspect is width / height. D3D11 clip space is left-handed
// ([-1, 1] in x and y, [0, 1] in z), so we negate Z in the standard
// right-handed formulation.
//
// Field-of-view convention: horizontal. Most games store FOV as vertical;
// we expose horizontal and let the camera provider convert (the game's
// own horizontal FOV is typically around 70°–90°).
void CameraState_BuildProjectionMatrix(Mat4 &proj,
                                       float horizontal_fov_radians,
                                       float aspect,
                                       float near_z,
                                       float far_z);

struct CameraState {
    // Eye in world space. Derived from PlayerState each frame.
    Vec3 eye_position;

    // Yaw + pitch. Same encoding as PlayerState.
    float yaw_radians;
    float pitch_radians;

    // Horizontal field of view in radians. Configurable via INI in M6+.
    // Default value matches TMRealWalk's default (~80°).
    float horizontal_fov_radians;

    // Projection constants.
    float aspect;
    float near_z;
    float far_z;

    // Pre-computed matrices. CameraState_Update writes these.
    Mat4 view;
    Mat4 projection;

    bool valid;

    // Projection cache. CameraState_Update rebuilds projection only when
    // one of (aspect, horizontal_fov_radians, near_z, far_z) differs from
    // the values used the last time the matrix was filled, OR when
    // projection_dirty_ is set. In a typical play session all four inputs
    // stay pinned after init — aspect only changes when the swap chain is
    // resized — so the cache hit is the steady-state case and the rebuild
    // is the rare event. Saves one tanf + one divide + 16 stores per frame
    // on the per-frame hot path.
    //
    // The flag is exposed (not private) so production code that mutates
    // any of these fields directly (the swapchain-aspect path is routed
    // through FrameLoop::SetAspect which sets it for you, but direct
    //   state.aspect = newValue;
    // edits — including test fixtures — get the same guarantee via the
    // field-comparison check below). Set it to true to force a rebuild;
    // any subsequent call to CameraState_Update will see it set, run the
    // rebuild, and clear it.
    bool projection_dirty_;
    float proj_cached_aspect;
    float proj_cached_h_fov;
    float proj_cached_near;
    float proj_cached_far;

    // View-matrix rotation cache. The rotation block (rows 0/1/2 cols
    // 0/1/2 — forward / right / up basis) depends only on yaw_radians
    // and pitch_radians. The translation row (row 3) depends on eye
    // position, which changes every walking frame. Splitting the
    // matrix update lets us skip 4 sin/cos + 12 multiplies + 1 sqrt
    // per frame when yaw and pitch are unchanged — the common case
    // while walking forward without mouselook, and any moment the
    // player stops to look around without rotating the view.
    //
    // Direct edits to yaw_radians / pitch_radians (including by tests)
    // are caught by the field-comparison check, the same way projection
    // handles direct edits to its 4 inputs.
    bool view_rotation_dirty_;
    float view_cached_yaw;
    float view_cached_pitch;

    // Pre-computed sin/cos of yaw_radians. CameraState_BuildViewRotation
    // (called when the view-rotation cache misses) needs one sinf + one
    // cosf per call; the same values were just computed by the
    // FrameLoop's PhaseIntegrateMovement when the player is on foot.
    // When the FrameLoop's AdoptPlayer path writes these (the
    // camera.yaw_radians came from the player, and the player just
    // computed its own trig in PhaseApplyMouseLook), BuildViewRotation
    // reads them and skips the local sinf/cosf. On the override path
    // (camera_ = camera_override_), the override carries its own
    // precomputed_yaw_trig_valid_ = false (CameraState_Reset clears
    // it), so BuildViewRotation falls back to computing fresh.
    //
    // Setter is exposed so FrameLoop::RunOnce can install the cached
    // values after AdoptPlayer. Tests that mutate yaw_radians directly
    // should clear precomputed_yaw_trig_valid_ = false to keep the
    // cache honest.
    bool precomputed_yaw_trig_valid_;
    float precomputed_sin_yaw_;
    float precomputed_cos_yaw_;
};

inline void CameraState_Reset(CameraState &c) {
    c.eye_position = {0.0, 0.0, 0.0};
    c.yaw_radians = 0.0f;
    c.pitch_radians = 0.0f;
    c.horizontal_fov_radians = 1.4f;  // ~80°
    c.aspect = 16.0f / 9.0f;
    c.near_z = 0.1f;
    c.far_z = 4000.0f;  // generous far plane for the game's open world
    Mat4_Identity(c.view);
    Mat4_Identity(c.projection);
    c.valid = false;
    // Force the first CameraState_Update to rebuild projection even though
    // proj_cached_* below match the defaults — init runs this through
    // BuildProjectionMatrix once so a debugger can step through a
    // populated projection matrix from the very first Update() call.
    c.projection_dirty_ = true;
    c.proj_cached_aspect = 0.0f;
    c.proj_cached_h_fov = 0.0f;
    c.proj_cached_near = 0.0f;
    c.proj_cached_far = 0.0f;
    // Force the first CameraState_Update to rebuild the view rotation
    // block, even though the proj_cached_* below default to 0 and would
    // otherwise need a separate sentinel. Set view_rotation_dirty_ true
    // to dodge the case where a Reset right before a real-frame sequence
    // returns identical-zero yaw/pitch by coincidence.
    c.view_rotation_dirty_ = true;
    c.view_cached_yaw = 0.0f;
    c.view_cached_pitch = 0.0f;
    // Clear the precomputed yaw trig cache. BuildViewRotation will
    // recompute sin(yaw)/cos(yaw) on its next call. The FrameLoop's
    // AdoptPlayer path will set this back to true on the next Walking
    // frame where it copies the player's precomputed values.
    c.precomputed_yaw_trig_valid_ = false;
    c.precomputed_sin_yaw_ = 0.0f;
    c.precomputed_cos_yaw_ = 0.0f;
}

// Recompute view and projection from current fields. Call before reading
// the matrices for the renderer.
void CameraState_Update(CameraState &c);

// Convenience: copy from PlayerState (eye position + yaw/pitch).
void CameraState_AdoptPlayer(CameraState &c, const PlayerState &p);

}  // namespace walkercore

#endif  // WALKERCORE_CAMERA_STATE_H