// WalkerCore — CameraState implementation.

#include "walkercore/CameraState.h"

#include <math.h>

namespace walkercore {

namespace {

// sin/cos wrappers to keep the rest of the file readable.
inline float SinF(float x) { return sinf(x); }
inline float CosF(float x) { return cosf(x); }
inline float TanF(float x) { return tanf(x); }

// Write the rotation block (rows 0/1/2 cols 0/1/2) of the view matrix
// from already-computed sin(yaw), cos(yaw), sin(pitch), cos(pitch).
// Both CameraState_BuildViewRotation (which computes its own sin/cos)
// and CameraState_Update (which uses precomputed values when the
// FrameLoop's AdoptPlayer path fills the cache) call into this helper.
// Keeping the trig-only and the precomputed paths aligned through a
// shared writer avoids any drift between the two code paths.
inline void WriteRotationBlockFromTrig(Mat4 &view,
                                       float sy, float cy,
                                       float sp, float cp) {
    const float fx = sy * cp;
    const float fy = sp;
    const float fz = -cy * cp;

    // right = forward × world_up(0,1,0).
    //   (a × b).x = a.y * b.z - a.z * b.y = fy*0 - fz*1 = -fz
    //   (a × b).y = a.z * b.x - a.x * b.z = fz*0 - fx*0 = 0
    //   (a × b).z = a.x * b.y - a.y * b.x = fx*1 - fy*0 = fx
    // Normalise: when pitch != 0 the result has length |cos(pitch)| < 1.
    // When pitch == 0, |right| == 1 exactly and the sqrt + divide is
    // pure waste — the most common case (looking horizontally) runs every
    // walking frame the user moves the mouse, so the sqrt dominates the
    // per-frame cost when the rotation cache misses. Skip the sqrt when
    // sp is near zero and set the unit right vector directly.
    float rx = -fz;
    float ry = 0.0f;
    float rz = fx;
    if (fabsf(sp) >= 1e-6f) {
        const float rlen = sqrtf(rx * rx + rz * rz);
        if (rlen > 1e-6f) {
            rx /= rlen;
            rz /= rlen;
        } else {
            // Degenerate (pitch = +/- 90°): fall back to yaw-only basis.
            rx = sy;
            rz = -cy;
        }
    }
    // else: pitch is ~0, right is already unit length (cy, 0, sy) — no
    // normalisation needed. ry stays 0.

    // up = right × forward (recompute exact orthonormal up).
    //   (r × f).x = r.y * f.z - r.z * f.y = 0*fz - rz*fy = -rz*fy
    //   (r × f).y = r.z * f.x - r.x * f.z = rz*fx - rx*fz
    //   (r × f).z = r.x * f.y - r.y * f.x = rx*fy - 0*fx = rx*fy
    const float ux = -rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy;

    // Compose view matrix rotation block as row-major:
    //   row 0 = right,    row 1 = up,    row 2 = forward.
    // Row 3 (translation) is intentionally NOT written here — see
    // CameraState_UpdateViewTranslation for the per-frame path that
    // just refreshes row 3 using the existing rows 0/1/2.
    view[0][0] = rx;  view[0][1] = ry;  view[0][2] = rz;  view[0][3] = 0.0f;
    view[1][0] = ux;  view[1][1] = uy;  view[1][2] = uz;  view[1][3] = 0.0f;
    view[2][0] = fx;  view[2][1] = fy;  view[2][2] = fz;  view[2][3] = 0.0f;
}

}  // namespace

void CameraState_BuildViewRotation(Mat4 &view,
                                    float yaw_radians,
                                    float pitch_radians) {
    // Forward vector in world space. Right-handed, +Y up. Camera looks
    // down its local -Z, so the world's +Z (south) corresponds to "behind"
    // the camera, and forward in D3D view space is forward by YXZ Euler:
    //   forward.x = sin(yaw) * cos(pitch)
    //   forward.y = sin(pitch)
    //   forward.z = -cos(yaw) * cos(pitch)   (negated for D3D LH view)
    const float cp = CosF(pitch_radians);
    const float sp = SinF(pitch_radians);
    const float cy = CosF(yaw_radians);
    const float sy = SinF(yaw_radians);
    WriteRotationBlockFromTrig(view, sy, cy, sp, cp);
}

void CameraState_UpdateViewTranslation(Mat4 &view, const Vec3 &eye) {
    // Recompute only row 3 (translation) from the eye position and
    // whatever rows 0/1/2 the rotation cache left in place. This is
    // dramatically cheaper than CameraState_BuildViewRotation — three
    // dot products, no trig, no sqrt — and the eye changes every
    // walking frame, so we always run this even on a cache hit.
    //
    // Cast to float to keep the strict -Wdouble-promotion flag happy
    // (mixing float axes with double eye coords without a cast promotes
    // everything to double). The numeric result is the same.
    const float rx = view[0][0], ry = view[0][1], rz = view[0][2];
    const float ux = view[1][0], uy = view[1][1], uz = view[1][2];
    const float fx = view[2][0], fy = view[2][1], fz = view[2][2];
    const float ex = static_cast<float>(eye.x);
    const float ey = static_cast<float>(eye.y);
    const float ez = static_cast<float>(eye.z);
    view[3][0] = -(rx * ex + ry * ey + rz * ez);
    view[3][1] = -(ux * ex + uy * ey + uz * ez);
    view[3][2] = -(fx * ex + fy * ey + fz * ez);
    view[3][3] = 1.0f;
}

void CameraState_BuildViewMatrix(Mat4 &view,
                                const Vec3 &eye,
                                float yaw_radians,
                                float pitch_radians) {
    // Compose the full view matrix in one go — rotation block followed
    // by translation row. Useful for tests and one-shot construction
    // (e.g. saved camera state); the per-frame conductor uses
    // CameraState_Update, which has the cache gate.
    CameraState_BuildViewRotation(view, yaw_radians, pitch_radians);
    CameraState_UpdateViewTranslation(view, eye);
}

void CameraState_BuildProjectionMatrix(Mat4 &proj,
                                       float horizontal_fov_radians,
                                       float aspect,
                                       float near_z,
                                       float far_z) {
    // Derive the vertical "f" factor from the horizontal FOV. Identity:
    //   tan(half_v_fov) = tan(half_h_fov) / aspect
    //   f = cot(half_v_fov) = aspect / tan(half_h_fov)
    // Then proj[0][0] = f/aspect = cot(half_h_fov), which gives x_clip = 1
    // at the right frustum edge.
    const float half_h_fov = horizontal_fov_radians * 0.5f;
    const float t = TanF(half_h_fov);
    const float f = (t > 1e-6f) ? (aspect / t) : 1e6f;  // degenerate guard

    const float fn = far_z;
    const float nn = near_z;

    Mat4_Identity(proj);
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    // D3D clip space is left-handed in Z: [0, 1].
    proj[2][2] = fn / (fn - nn);
    proj[2][3] = 1.0f;
    proj[3][2] = -(fn * nn) / (fn - nn);
    proj[3][3] = 0.0f;
}

void CameraState_Update(CameraState &c) {
    // View rotation block. Rebuild only when yaw or pitch changed, OR
    // when the explicit dirty flag is set. Eye position does NOT enter
    // this decision — translation is updated unconditionally below.
    if (c.view_rotation_dirty_ ||
        c.view_cached_yaw != c.yaw_radians ||
        c.view_cached_pitch != c.pitch_radians) {
        // Prefer the precomputed sin(yaw)/cos(yaw) the FrameLoop filled
        // when AdoptPlayer copied the player's yaw into c.yaw_radians.
        // PhaseApplyMouseLook computed the same values for
        // PhaseIntegrateMovement, so this path skips one sinf + one
        // cosf per Walking frame on cache-miss rotations. The override
        // path (camera_ = camera_override_) leaves
        // precomputed_yaw_trig_valid_ = false (CameraState_Reset clears
        // it on the freshly constructed CameraState the FSM uses), so
        // BuildViewRotation recomputes from scratch on transitions.
        if (c.precomputed_yaw_trig_valid_) {
            // Cache valid; skip sin/cos for the yaw and write the matrix
            // directly. We still need sin/cos of pitch (pitch trig is not
            // cached — it changes less per frame than yaw and is not used
            // elsewhere on the hot path, so caching it would not save
            // work).
            const float sy = c.precomputed_sin_yaw_;
            const float cy = c.precomputed_cos_yaw_;
            const float sp = SinF(c.pitch_radians);
            const float cp = CosF(c.pitch_radians);
            WriteRotationBlockFromTrig(c.view, sy, cy, sp, cp);
        } else {
            CameraState_BuildViewRotation(c.view, c.yaw_radians, c.pitch_radians);
        }
        c.view_cached_yaw = c.yaw_radians;
        c.view_cached_pitch = c.pitch_radians;
        c.view_rotation_dirty_ = false;
    }

    // View translation row. Always update — eye_position changes every
    // walking frame and only the rotation block is cacheable.
    CameraState_UpdateViewTranslation(c.view, c.eye_position);

    // Rebuild projection only when one of the four inputs changed, OR the
    // explicit dirty flag is set. In the steady state (no swap-chain
    // resize, no INI override of the FOV, no near/far change) the cache
    // hits on every frame and we save 1 tanf + 1 divide + 16 stores.
    if (c.projection_dirty_ ||
        c.proj_cached_aspect != c.aspect ||
        c.proj_cached_h_fov != c.horizontal_fov_radians ||
        c.proj_cached_near != c.near_z ||
        c.proj_cached_far != c.far_z) {
        CameraState_BuildProjectionMatrix(
            c.projection, c.horizontal_fov_radians, c.aspect, c.near_z, c.far_z);
        c.proj_cached_aspect = c.aspect;
        c.proj_cached_h_fov = c.horizontal_fov_radians;
        c.proj_cached_near = c.near_z;
        c.proj_cached_far = c.far_z;
        c.projection_dirty_ = false;
    }
    c.valid = true;
}

void CameraState_AdoptPlayer(CameraState &c, const PlayerState &p) {
    c.eye_position = PlayerState_EyePosition(p);
    c.yaw_radians = p.yaw_radians;
    c.pitch_radians = p.pitch_radians;
}

}  // namespace walkercore