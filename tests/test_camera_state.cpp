// WalkerCore — CameraState tests.
//
// Pure math. Verifies:
//   - view matrix translates a world-space point into view space correctly
//   - view matrix is orthonormal (right + up + forward form a basis)
//   - projection matrix maps a frustum corner to ±1 in clip space
//   - CameraState_AdoptPlayer pulls eye_position from feet + eye_height

#include "TestHarness.h"
#include "walkercore/CameraState.h"
#include "walkercore/PlayerState.h"
#include "walkercore/Time.h"

#include <math.h>

namespace walkercore {

namespace {

// Helpers.
inline double Dot4(const float a[4], const float b[4]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

}  // namespace

void TestCameraViewMatrix() {
    // Standing at the origin, looking forward (yaw=0, pitch=0).
    Mat4 view;
    CameraState_BuildViewMatrix(view, Vec3{0, 0, 0}, 0.0f, 0.0f);

    // Right axis = (1, 0, 0).
    const float p_fwd[4] = {0, 0, 100, 1};
    const float p_back[4] = {0, 0, -100, 1};
    const float p_right[4] = {100, 0, 0, 1};
    const float p_up[4] = {0, 100, 0, 1};

    // In view space, looking down -Z, +Z is behind the camera. So a point
    // 100 units in front of the camera (-Z in view) should have a negative
    // z in view space.
    const double fwd_z = Dot4(view[2], p_fwd);  // row 2 == forward basis
    const double back_z = Dot4(view[2], p_back);
    const double right_x = Dot4(view[0], p_right);  // row 0 == right basis
    const double up_y = Dot4(view[1], p_up);        // row 1 == up basis

    EXPECT_LT(fwd_z, 0.0);
    EXPECT_GT(back_z, 0.0);
    EXPECT_NEAR(right_x, 100.0, 1e-3);
    EXPECT_NEAR(up_y, 100.0, 1e-3);

    // Forward . right should be ~0.
    const float f[3] = {view[2][0], view[2][1], view[2][2]};
    const float r[3] = {view[0][0], view[0][1], view[0][2]};
    const double dot = f[0] * r[0] + f[1] * r[1] + f[2] * r[2];
    EXPECT_NEAR(dot, 0.0, 1e-4);

    // Forward . up should be ~0.
    const float u[3] = {view[1][0], view[1][1], view[1][2]};
    const double dot2 = f[0] * u[0] + f[1] * u[1] + f[2] * u[2];
    EXPECT_NEAR(dot2, 0.0, 1e-4);
}

void TestCameraProjectionMatrix() {
    Mat4 proj;
    // 90° horizontal FOV at 16:9. Near 0.1, far 1000.
    const float hfov = 1.5708f;  // π/2
    CameraState_BuildProjectionMatrix(proj, hfov, 16.0f / 9.0f, 0.1f, 1000.0f);

    // proj[1][1] should equal f_y = aspect / tan(half_h_fov) = (16/9)/1 = 1.778.
    EXPECT_NEAR(proj[1][1], 16.0f / 9.0f, 1e-4);
    // proj[0][0] should equal f_y / aspect = 1.
    EXPECT_NEAR(proj[0][0], 1.0f, 1e-4);

    // proj[2][2] = far/(far-near) = 1000/999.9 ≈ 1.0001.
    EXPECT_NEAR(proj[2][2], 1000.0f / 999.9f, 1e-4);
    // proj[3][2] = -far*near/(far-near) = -100/999.9 ≈ -0.10001.
    EXPECT_NEAR(proj[3][2], -100.0f / 999.9f, 1e-4);
    // proj[2][3] = 1 (perspective divide).
    EXPECT_NEAR(proj[2][3], 1.0f, 1e-6);
    // proj[3][3] = 0.
    EXPECT_NEAR(proj[3][3], 0.0f, 1e-6);

    // Identity guard: degenerate FOV (very small horizontal FOV) should
    // not blow up — f_y should be capped rather than infinity.
    Mat4 proj2;
    CameraState_BuildProjectionMatrix(proj2, 0.001f, 1.0f, 0.1f, 1000.0f);
    EXPECT_LT(proj2[1][1], 1e7f);
}

void TestCameraAdoptPlayer() {
    CameraState c;
    CameraState_Reset(c);

    PlayerState p;
    PlayerState_Reset(p);
    p.feet_position = Vec3{10, 0, 20};
    p.eye_height = 1.65f;
    p.yaw_radians = 1.2f;
    p.pitch_radians = 0.3f;

    CameraState_AdoptPlayer(c, p);
    EXPECT_NEAR(c.eye_position.x, 10.0, 1e-6);
    EXPECT_NEAR(c.eye_position.y, 1.65, 1e-4);
    EXPECT_NEAR(c.eye_position.z, 20.0, 1e-6);
    EXPECT_NEAR(c.yaw_radians, 1.2f, 1e-6);
    EXPECT_NEAR(c.pitch_radians, 0.3f, 1e-6);
}

// -----------------------------------------------------------------------------
// CameraState_Update field-exhaustive probe.
//
// CameraState_Update(c) reads 7 fields and writes 16 matrix components:
//
//   c.eye_position             → view[3][0..2]  (translation row)
//   c.yaw_radians              → view[0], view[2]  (rotation around world Y)
//   c.pitch_radians            → view[1], view[2][1]  (rotation around X)
//   c.horizontal_fov_radians   → proj[0][0], proj[1][1]
//   c.aspect                   → proj[0][0], proj[1][1]
//   c.near_z                   → proj[2][2], proj[3][2]
//   c.far_z                    → proj[2][2], proj[3][2]
//
// Each MUST have an observable effect. If a future refactor drops a
// read of one of these fields, the case for that field fails with a
// clear name in the failure message. Each case uses a fresh CameraState
// so the others aren't perturbed by lingering field state.
// -----------------------------------------------------------------------------

void TestCameraStateUpdateEveryFieldHasObservableEffect() {
    // ---- Case 1: c.eye_position → view[3][0..2] (translation row) ----
    {
        CameraState c;
        CameraState_Reset(c);
        c.eye_position = Vec3{0.0, 0.0, 0.0};
        CameraState_Update(c);
        const float b_t0 = c.view[3][0];
        const float b_t1 = c.view[3][1];
        const float b_t2 = c.view[3][2];

        CameraState c2;
        CameraState_Reset(c2);
        c2.eye_position = Vec3{5.0, -3.0, 2.0};  // distinct translation
        CameraState_Update(c2);
        // Translation row changed (each component differs).
        EXPECT_TRUE(c2.view[3][0] != b_t0);
        EXPECT_TRUE(c2.view[3][1] != b_t1);
        EXPECT_TRUE(c2.view[3][2] != b_t2);
        // Negative control: forward basis (row 2) unchanged because eye
        // only affects the translation row.
        EXPECT_NEAR(c2.view[2][0], c.view[2][0], 1e-5);
        EXPECT_NEAR(c2.view[2][1], c.view[2][1], 1e-5);
        EXPECT_NEAR(c2.view[2][2], c.view[2][2], 1e-5);
        // Negative control: projection untouched.
        EXPECT_NEAR(c2.projection[0][0], c.projection[0][0], 1e-6);
        EXPECT_NEAR(c2.projection[1][1], c.projection[1][1], 1e-6);
    }

    // ---- Case 2: c.yaw_radians → forward/right basis changes ----
    {
        CameraState c;
        CameraState_Reset(c);
        c.yaw_radians = 0.0f;
        c.pitch_radians = 0.0f;
        CameraState_Update(c);
        const float f0_yaw0_x = c.view[2][0];
        const float f0_yaw0_z = c.view[2][2];

        CameraState c2;
        CameraState_Reset(c2);
        c2.yaw_radians = 1.5708f;  // 90° — forward should now point along +X
        c2.pitch_radians = 0.0f;
        CameraState_Update(c2);
        const float f0_yaw90_x = c2.view[2][0];
        const float f0_yaw90_z = c2.view[2][2];

        // Forward changed dramatically.
        EXPECT_TRUE(f0_yaw0_x != f0_yaw90_x);
        EXPECT_TRUE(f0_yaw0_z != f0_yaw90_z);
        // Negative control: pitch == 0 in both → forward.y unchanged.
        EXPECT_NEAR(c2.view[2][1], c.view[2][1], 1e-5);
        // Negative control: projection untouched (yaw doesn't feed proj).
        EXPECT_NEAR(c2.projection[0][0], c.projection[0][0], 1e-6);
    }

    // ---- Case 3: c.pitch_radians → up vector changes ----
    {
        CameraState c;
        CameraState_Reset(c);
        c.pitch_radians = 0.0f;
        CameraState_Update(c);
        const float baseline_up_y = c.view[1][1];
        const float baseline_fwd_y = c.view[2][1];

        CameraState c2;
        CameraState_Reset(c2);
        c2.pitch_radians = 0.5f;  // ~28°
        CameraState_Update(c2);
        // Forward.y is sin(pitch) — non-zero when pitched.
        EXPECT_TRUE(c2.view[2][1] != baseline_fwd_y);
        // Up vector was (0, 1, 0) at pitch=0; pitched up shifts up.y.
        EXPECT_TRUE(c2.view[1][1] != baseline_up_y);
        // Negative control: with yaw=0 in both cases, forward.x
        // = sin(yaw)*cos(pitch) = 0 regardless of pitch. (forward.z
        // DOES change with pitch because it's -cos(yaw)*cos(pitch),
        // so it's NOT a valid negative control — it's a positive.)
        EXPECT_NEAR(c2.view[2][0], c.view[2][0], 1e-5);
        // Negative control: right vector unchanged (yaw=0, so right is
        // world +X regardless of pitch).
        EXPECT_NEAR(c2.view[0][0], c.view[0][0], 1e-5);
        EXPECT_NEAR(c2.view[0][1], c.view[0][1], 1e-5);
        EXPECT_NEAR(c2.view[0][2], c.view[0][2], 1e-5);
        // Negative control: projection untouched.
        EXPECT_NEAR(c2.projection[0][0], c.projection[0][0], 1e-6);
    }

    // ---- Case 4: c.horizontal_fov_radians → proj[0][0], proj[1][1] ----
    {
        CameraState c;
        CameraState_Reset(c);
        c.horizontal_fov_radians = 1.4f;  // default ~80°
        CameraState_Update(c);
        const float b_p00 = c.projection[0][0];
        const float b_p11 = c.projection[1][1];

        CameraState c2;
        CameraState_Reset(c2);
        c2.horizontal_fov_radians = 1.0f;  // narrower FOV → larger f_y
        CameraState_Update(c2);
        EXPECT_TRUE(c2.projection[0][0] != b_p00);
        EXPECT_TRUE(c2.projection[1][1] != b_p11);
        // Negative control: Z projection columns (where near_z/far_z live)
        // unchanged.
        EXPECT_NEAR(c2.projection[2][2], c.projection[2][2], 1e-6);
        EXPECT_NEAR(c2.projection[3][2], c.projection[3][2], 1e-6);
    }

    // ---- Case 5: c.aspect → proj[1][1] (and indirectly proj[0][0] cancels) ----
    //
    // Subtle: the projection formula is
    //     f       = aspect / tan(half_h_fov)
    //     proj[0][0] = f / aspect = 1 / tan(half_h_fov)
    //     proj[1][1] = f             = aspect / tan(half_h_fov)
    // The aspect cancels out of proj[0][0] — horizontal FOV is the
    // fixed input, so the horizontal-axis scale is aspect-independent.
    // Only proj[1][1] (vertical-axis scale) changes with aspect.
    {
        CameraState c;
        CameraState_Reset(c);
        c.aspect = 16.0f / 9.0f;  // default ~1.778
        CameraState_Update(c);
        const float b_p00 = c.projection[0][0];   // aspect-INDEPENDENT
        const float b_p11 = c.projection[1][1];   // aspect-dependent

        CameraState c2;
        CameraState_Reset(c2);
        c2.aspect = 4.0f / 3.0f;  // narrower aspect (4/3 ≈ 1.333)
        CameraState_Update(c2);
        // Positive: proj[1][1] changes with aspect.
        EXPECT_TRUE(c2.projection[1][1] != b_p11);
        // Negative control: proj[0][0] is independent of aspect.
        EXPECT_NEAR(c2.projection[0][0], b_p00, 1e-6);
        // Negative control: Z projection columns unchanged.
        EXPECT_NEAR(c2.projection[2][2], c.projection[2][2], 1e-6);
        EXPECT_NEAR(c2.projection[3][2], c.projection[3][2], 1e-6);
    }

    // ---- Case 6: c.near_z → proj[2][2], proj[3][2] ----
    {
        CameraState c;
        CameraState_Reset(c);
        c.near_z = 0.1f;
        c.far_z = 4000.0f;
        CameraState_Update(c);
        const float b_p22 = c.projection[2][2];
        const float b_p32 = c.projection[3][2];

        CameraState c2;
        CameraState_Reset(c2);
        c2.near_z = 1.0f;  // 10× farther near plane
        c2.far_z = 4000.0f;
        CameraState_Update(c2);
        EXPECT_TRUE(c2.projection[2][2] != b_p22);
        EXPECT_TRUE(c2.projection[3][2] != b_p32);
        // Negative control: FOV basis unchanged.
        EXPECT_NEAR(c2.projection[0][0], c.projection[0][0], 1e-6);
        EXPECT_NEAR(c2.projection[1][1], c.projection[1][1], 1e-6);
    }

    // ---- Case 7: c.far_z → proj[2][2], proj[3][2] ----
    {
        CameraState c;
        CameraState_Reset(c);
        c.near_z = 0.1f;
        c.far_z = 1000.0f;
        CameraState_Update(c);
        const float b_p22 = c.projection[2][2];
        const float b_p32 = c.projection[3][2];

        CameraState c2;
        CameraState_Reset(c2);
        c2.near_z = 0.1f;
        c2.far_z = 10000.0f;  // 10× farther
        CameraState_Update(c2);
        EXPECT_TRUE(c2.projection[2][2] != b_p22);
        EXPECT_TRUE(c2.projection[3][2] != b_p32);
        // Negative control: FOV basis unchanged.
        EXPECT_NEAR(c2.projection[0][0], c.projection[0][0], 1e-6);
        EXPECT_NEAR(c2.projection[1][1], c.projection[1][1], 1e-6);
    }
}

// CameraState_Update perf budget — pin the per-call cost of the mat-rebuild
// path that runs every FrameLoop tick. CameraState_BuildViewMatrix does 4
// sin/cos + cross products + a 4x4 row-major fill; BuildProjectionMatrix
// does 1 tan + a div + a 4x4 fill. All pure float math, no allocations,
// no provider hops. Real cost on modern x86 is well under 1 µs/call. The
// 5 µs/call ceiling below leaves 10× headroom and catches a regression
// that, say, allocates a temporary mat, routes through a virtual dispatch,
// or promotes the math to double.
//
// The benchmark uses realistic camera angles (yaw + pitch non-trivial)
// because the degenerate-branch path (pitch = ±90°) skips a sqrt and the
// test should exercise the common path, not the corner case.
void TestCameraStateUpdatePerfBudget() {
    CameraState c;
    CameraState_Reset(c);
    c.eye_position = Vec3{12.5, 2.0, -7.3};  // non-origin to exercise translation row
    c.yaw_radians = 0.7f;
    c.pitch_radians = -0.3f;
    c.horizontal_fov_radians = 1.4f;  // ~80°
    c.aspect = 16.0f / 9.0f;
    c.near_z = 0.1f;
    c.far_z = 4000.0f;

    constexpr int kWarmup = 5000;
    constexpr int kIterations = 100000;

    // Warm caches + branch predictors.
    for (int i = 0; i < kWarmup; ++i) {
        CameraState_Update(c);
    }

    const uint64_t t0 = walkercore::NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        CameraState_Update(c);
    }
    const uint64_t t1 = walkercore::NowMicros();

    const uint64_t total_us = t1 - t0;
    const double per_call_us =
        static_cast<double>(total_us) / static_cast<double>(kIterations);

    // 5 µs/call ceiling. The "include <walkercore/CameraState.h>" smoke
    // import of NowMicros is on purpose — the function lives in Time.h
    // and we surface it here so the next contributor who looks at this
    // file finds it.
    EXPECT_LT(per_call_us, 5.0);
    EXPECT_NE(total_us, 0ull);

    // Sanity: the matrices actually got written. A regression that
    // short-circuits CameraState_Update (e.g. an `if (c.valid) return;`
    // short-circuit with `valid` already set) would skip the work and
    // also fail to recompute the matrices. Pin a known fingerprint of
    // view[0][0] and projection[1][1] so we can't go silently fast.
    CameraState_Reset(c);
    c.eye_position = Vec3{0.0, 1.0, 0.0};
    c.yaw_radians = 0.0f;
    c.pitch_radians = 0.0f;
    CameraState_Update(c);
    // yaw=0, pitch=0 → forward = (0, 0, -1), right = (1, 0, 0), up = (0,1,0)
    EXPECT_NEAR(c.view[0][0], 1.0f, 1e-6f);   // right.x
    EXPECT_NEAR(c.view[1][1], 1.0f, 1e-6f);   // up.y
    EXPECT_NEAR(c.view[2][2], -1.0f, 1e-6f);  // forward.z
    // projection[1][1] = cot(half_v_fov), which the impl derives from
    // the horizontal FOV as aspect / tan(half_h_fov). With aspect = 16/9
    // ≈ 1.778 and half_h_fov = 0.7 rad (tan ≈ 0.842), the value is
    // ~2.110. The "1/tan(half_h_fov)" reading would give 1.187 and
    // confuses horizontal with vertical — caught this on first build.
    const float half_h_fov = c.horizontal_fov_radians * 0.5f;
    const float t = tanf(half_h_fov);
    EXPECT_NEAR(c.projection[1][1], c.aspect / t, 1e-5f);
    // Negative control: the horizontal-fov–derived column must NOT equal
    // the naive cot(half_h_fov) reading (it would be a sign the formula
    // collapsed into a 1-form somewhere — a regression we want caught
    // before it ships to the renderer).
    EXPECT_GT(fabsf(c.projection[1][1] - (1.0f / t)), 0.1f);
}

// -----------------------------------------------------------------------------
// Projection-cache probe.
//
// CameraState_Update has a small cache for the projection matrix: it
// rebuilds only when one of (aspect, horizontal_fov_radians, near_z,
// far_z) actually changes vs the values used the last time the matrix
// was filled, OR when projection_dirty_ is set. In a typical play
// session all four inputs are pinned after init, so the steady-state
// path is a cache hit that skips 1 tanf + 1 divide + 16 stores per
// frame. This probe pins the cache behaviour:
//   - The first Update after CameraState_Reset rebuilds (dirty flag set).
//   - A second Update with identical fields is a cache HIT — projection
//     bytes are untouched, and the dirty flag has been cleared.
//   - Each of the four input fields, on its own, invalidates the cache.
//   - Setting projection_dirty_ explicitly invalidates the cache even
//     when no field changed.
//   - An Edit-in-place of a projection row still rebuilds correctly —
//     i.e. the cache does not return STALE data when an invalidating
//     change is followed by Update.
// -----------------------------------------------------------------------------

void TestCameraStateProjectionCacheHitWhenInputsUnchanged() {
    CameraState c;
    CameraState_Reset(c);
    // First call rebuilds (dirty bit cleared).
    CameraState_Update(c);
    EXPECT_FALSE(c.projection_dirty_);

    // Capture a snapshot of every projection byte. The second call must
    // produce a byte-for-byte identical projection[] — proof that the
    // cache short-circuited the rebuild path. (If the rebuild path ran
    // again, the math would still produce identical bytes for the same
    // inputs, so this test isn't enough on its own — see the next
    // sub-test for direct cache-invalidation coverage. The combination
    // pins both "second call is correct" AND "second call observed the
    // same path as the first".)
    float proj_bytes[16];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            proj_bytes[i * 4 + j] = c.projection[i][j];
        }
    }

    CameraState_Update(c);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            EXPECT_NEAR(c.projection[i][j], proj_bytes[i * 4 + j], 0.0f);
        }
    }
    EXPECT_FALSE(c.projection_dirty_);
}

// Cache-invalidation: each of the four inputs must trigger a rebuild.
// We edit a projection[] cell in place to a sentinel between the first
// and second Update calls. The cache MISS path overwrites the sentinel;
// the cache HIT path would leave it. So the sentinel-after-update
// pins whether the rebuild ran.
void TestCameraStateProjectionCacheInvalidatedByEachInput() {
    // --- aspect change ---
    {
        CameraState c;
        CameraState_Reset(c);
        CameraState_Update(c);
        EXPECT_FALSE(c.projection_dirty_);
        // Sentinel-edit a projection byte, then change aspect.
        const float sentinel = -12345.5f;
        c.projection[1][1] = sentinel;
        c.aspect = 4.0f / 3.0f;
        CameraState_Update(c);
        // If the cache detected the aspect change, projection[1][1] was
        // overwritten with the freshly-computed value (which is far from
        // the sentinel).
        EXPECT_TRUE(c.projection[1][1] != sentinel);
        EXPECT_FALSE(c.projection_dirty_);
    }
    // --- horizontal_fov_radians change ---
    {
        CameraState c;
        CameraState_Reset(c);
        CameraState_Update(c);
        const float sentinel = -7777.25f;
        c.projection[0][0] = sentinel;
        c.horizontal_fov_radians = 0.9f;  // narrower → larger f_y
        CameraState_Update(c);
        EXPECT_TRUE(c.projection[0][0] != sentinel);
        EXPECT_FALSE(c.projection_dirty_);
    }
    // --- near_z change ---
    {
        CameraState c;
        CameraState_Reset(c);
        CameraState_Update(c);
        const float sentinel = -8888.125f;
        c.projection[2][2] = sentinel;
        c.near_z = 1.0f;
        CameraState_Update(c);
        EXPECT_TRUE(c.projection[2][2] != sentinel);
        EXPECT_FALSE(c.projection_dirty_);
    }
    // --- far_z change ---
    {
        CameraState c;
        CameraState_Reset(c);
        CameraState_Update(c);
        const float sentinel = -9999.0625f;
        c.projection[3][2] = sentinel;
        c.far_z = 1000.0f;
        CameraState_Update(c);
        EXPECT_TRUE(c.projection[3][2] != sentinel);
        EXPECT_FALSE(c.projection_dirty_);
    }
    // --- explicit projection_dirty_ flag ---
    {
        CameraState c;
        CameraState_Reset(c);
        CameraState_Update(c);
        EXPECT_FALSE(c.projection_dirty_);
        const float sentinel = -11111.0f;
        c.projection[1][1] = sentinel;
        c.projection_dirty_ = true;
        CameraState_Update(c);
        // The flag forced the rebuild — sentinel overwritten.
        EXPECT_TRUE(c.projection[1][1] != sentinel);
        EXPECT_FALSE(c.projection_dirty_);
    }
    // --- negative control: no field change AND no dirty flag ---
    // The sentinel-edit is NOT a field change, so the cache HIT path
    // leaves the sentinel in place. This pins that the optimization
    // doesn't accidentally recompute when nothing relevant changed.
    {
        CameraState c;
        CameraState_Reset(c);
        CameraState_Update(c);
        EXPECT_FALSE(c.projection_dirty_);
        const float sentinel = -22222.0f;
        c.projection[1][1] = sentinel;
        CameraState_Update(c);  // no field change, no flag set
        // Cache hit: sentinel survives.
        EXPECT_NEAR(c.projection[1][1], sentinel, 0.0f);
        EXPECT_FALSE(c.projection_dirty_);
    }
}

// CameraState_Update cached-path perf budget — the steady-state per-frame
// cost. After the projection cache lands, the hot path skips the entire
// projection rebuild (1 tanf + 1 divide + Mat4_Identity + 16 stores) on
// every Update() call, so the call should be visibly cheaper than the
// existing TestCameraStateUpdatePerfBudget number. The constant here is
// intentionally much tighter (sub-µs/call) so a future regression that
// accidentally routes the cache-hit path through a slow Math:: operator,
// or that re-introduces the rebuild unconditionally, fails this probe.
void TestCameraStateUpdateCachedPathPerfBudget() {
    CameraState c;
    CameraState_Reset(c);
    c.eye_position = Vec3{12.5, 2.0, -7.3};
    c.yaw_radians = 0.7f;
    c.pitch_radians = -0.3f;
    c.horizontal_fov_radians = 1.4f;
    c.aspect = 16.0f / 9.0f;
    c.near_z = 0.1f;
    c.far_z = 4000.0f;

    constexpr int kWarmup = 5000;
    constexpr int kIterations = 100000;

    // Establish cache state. The first Update here rebuilds; the rest hit.
    CameraState_Update(c);
    EXPECT_FALSE(c.projection_dirty_);

    for (int i = 0; i < kWarmup; ++i) {
        CameraState_Update(c);  // cache hit every time
    }

    const uint64_t t0 = walkercore::NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        CameraState_Update(c);
    }
    const uint64_t t1 = walkercore::NowMicros();

    const uint64_t total_us = t1 - t0;
    const double per_call_us =
        static_cast<double>(total_us) / static_cast<double>(kIterations);

    // 1.5 µs/call ceiling. The previous (rebuild-always) budget was
    // 5 µs; the cache hit path should land well below that on any modern
    // x86. Catches a regression that, e.g., accidentally always sets
    // projection_dirty_ before calling Update, or that routes the
    //    if (c.projection_dirty_ || ...)
    // branch through a slow call sequence.
    EXPECT_LT(per_call_us, 1.5);
    EXPECT_NE(total_us, 0ull);

    // Pin the projection cache to be warm at the end of the benchmark —
    // protects against an in-flight refactor that resets the dirty flag
    // for any reason and accidentally turns the benchmark into a
    // rebuild-every-call one (which would still pass the 1.5 µs ceiling
    // but slower, hiding the cache effect).
    EXPECT_FALSE(c.projection_dirty_);
}

// -----------------------------------------------------------------------------
// View rotation cache probe.
//
// CameraState_Update has a small cache for the view-matrix rotation
// block (rows 0/1/2 cols 0/1/2 — forward / right / up basis). The
// rotation depends only on yaw + pitch; the translation row depends
// on eye_position, which moves every walking frame. Splitting the
// view-matrix update lets us skip 4 sin/cos + 12 multiplies + 1 sqrt
// per frame when yaw and pitch haven't changed.
//
// The probe pins:
//   - First Update after Reset clears both dirty flags and rebuilds
//     both blocks.
//   - Subsequent Updates with no field change AND no flag is a CACHE
//     HIT — both the rotation block bytes and the dirty flag survive.
//   - Editing yaw or pitch invalidates the rotation cache (miss path
//     overwrites a sentinel byte in the rotation block).
//   - Editing eye_position DOES NOT invalidate the rotation cache —
//     translation row still refreshes, but the basis bytes don't change
//     from a cache hit perspective (negative control).
//   - The view_rotation_dirty_ flag invalidates the rotation cache
//     explicitly (mirror of the projection_dirty_ contract).
// -----------------------------------------------------------------------------

void TestCameraStateViewRotationCacheHitWhenInputsUnchanged() {
    CameraState c;
    CameraState_Reset(c);
    CameraState_Update(c);
    EXPECT_FALSE(c.view_rotation_dirty_);

    // Capture rotation-block bytes after the first (rebuilding) Update.
    float rot_bytes[12];  // rows 0/1/2 × cols 0/1/2 = 12 floats
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            rot_bytes[i * 4 + j] = c.view[i][j];
        }
    }
    // Also grab translation row, which must keep refreshing every frame
    // even on a rotation-cache hit.
    float expected_tx = c.view[3][0];

    // 32 subsequent Updates with all inputs unchanged. The rotation
    // block should be byte-identical (cache hit); the translation row
    // is re-derived from eye (but eye also unchanged in this test).
    for (int k = 0; k < 32; ++k) {
        CameraState_Update(c);
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            EXPECT_NEAR(c.view[i][j], rot_bytes[i * 4 + j], 0.0f);
        }
    }
    EXPECT_NEAR(c.view[3][0], expected_tx, 1e-6f);
    EXPECT_FALSE(c.view_rotation_dirty_);
}

void TestCameraStateViewRotationCacheInvalidatedByYaw() {
    CameraState c;
    CameraState_Reset(c);
    CameraState_Update(c);
    EXPECT_FALSE(c.view_rotation_dirty_);
    const float sentinel = -42345.5f;
    c.view[2][0] = sentinel;  // forward.x
    c.yaw_radians = 1.5708f;  // 90° — forward.x should swing from 0 to 1
    CameraState_Update(c);
    EXPECT_TRUE(c.view[2][0] != sentinel);
    EXPECT_FALSE(c.view_rotation_dirty_);
}

void TestCameraStateViewRotationCacheInvalidatedByPitch() {
    CameraState c;
    CameraState_Reset(c);
    CameraState_Update(c);
    EXPECT_FALSE(c.view_rotation_dirty_);
    const float sentinel = -77777.25f;
    c.view[2][1] = sentinel;  // forward.y
    c.pitch_radians = 0.5f;
    CameraState_Update(c);
    EXPECT_TRUE(c.view[2][1] != sentinel);
    EXPECT_FALSE(c.view_rotation_dirty_);
}

void TestCameraStateViewRotationCacheNegativeControlOnEyePosition() {
    // Eye-position edits must NOT invalidate the rotation cache. The
    // rotation block is independent of eye (and the translation row
    // always refreshes). Sentinel a rotation block byte, change only
    // eye, and verify the sentinel survives — that's the cache HIT.
    CameraState c;
    CameraState_Reset(c);
    CameraState_Update(c);
    EXPECT_FALSE(c.view_rotation_dirty_);
    const float sentinel = -11111.0f;
    c.view[2][2] = sentinel;  // forward.z — unchanged by pure eye movement
    c.eye_position = Vec3{99.0, 88.0, 77.0};
    CameraState_Update(c);
    EXPECT_NEAR(c.view[2][2], sentinel, 0.0f);  // rotation cache HIT
    EXPECT_FALSE(c.view_rotation_dirty_);
    // The translation row DID refresh — that's the unconditional path.
    // eye = (99, 88, 77) with the basis from Reset (mostly fwd=-Z) gives
    //   view[3][2] = -(-(77) * (-1)) = -77.
    // (We don't pin an exact value here — the basis math is exercised
    // by TestCameraViewMatrix; this probe only pins that translation
    // happens on a rotation-cache hit.)
    EXPECT_TRUE(c.view[3][0] != 0.0f || c.view[3][1] != 0.0f ||
                c.view[3][2] != 0.0f);
}

void TestCameraStateViewRotationCacheExplicitDirtyFlag() {
    CameraState c;
    CameraState_Reset(c);
    CameraState_Update(c);
    EXPECT_FALSE(c.view_rotation_dirty_);
    const float sentinel = -22222.0f;
    c.view[1][1] = sentinel;  // up.y
    c.view_rotation_dirty_ = true;
    CameraState_Update(c);
    EXPECT_TRUE(c.view[1][1] != sentinel);
    EXPECT_FALSE(c.view_rotation_dirty_);
}

// TestCameraStateViewRotationCacheFullBuildMatchesSplit — make sure the
// cache-split decomposition produces IDENTICAL matrices to a single
// BuildViewMatrix call. Pin this by comparing both translation rows
// and full rows 0/1/2.
void TestCameraStateViewRotationCacheFullBuildMatchesSplit() {
    Mat4 full_view;
    CameraState_BuildViewMatrix(full_view, Vec3{12.0, 3.0, -4.0}, 0.6f, -0.2f);

    Mat4 split_view;
    Mat4_Identity(split_view);  // row 3 starts at (0,0,0,1) — identity row3
    CameraState_BuildViewRotation(split_view, 0.6f, -0.2f);
    CameraState_UpdateViewTranslation(split_view, Vec3{12.0, 3.0, -4.0});

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            EXPECT_NEAR(full_view[i][j], split_view[i][j], 1e-6f);
        }
    }
}

// Per-frame cached-path perf budget — both the projection cache AND the
// view rotation cache are warm. The rotation update path is the
// translation-only refresh (3 dot products); the projection path is a
// pure cache hit (no rebuild). Total cost should be well under the
// TestCameraStateUpdateCachedPathPerfBudget ceiling, which only warmed
// the projection cache.
void TestCameraStateUpdateFullCacheHitPerfBudget() {
    CameraState c;
    CameraState_Reset(c);
    c.eye_position = Vec3{12.5, 2.0, -7.3};
    c.yaw_radians = 0.7f;
    c.pitch_radians = -0.3f;
    c.horizontal_fov_radians = 1.4f;
    c.aspect = 16.0f / 9.0f;
    c.near_z = 0.1f;
    c.far_z = 4000.0f;

    constexpr int kWarmup = 5000;
    constexpr int kIterations = 100000;

    // First call rebuilds BOTH blocks; subsequent calls hit BOTH caches.
    CameraState_Update(c);
    EXPECT_FALSE(c.projection_dirty_);
    EXPECT_FALSE(c.view_rotation_dirty_);

    for (int i = 0; i < kWarmup; ++i) {
        CameraState_Update(c);
    }

    const uint64_t t0 = walkercore::NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        CameraState_Update(c);
    }
    const uint64_t t1 = walkercore::NowMicros();

    const double per_call_us =
        static_cast<double>(t1 - t0) / static_cast<double>(kIterations);

    // Tight ceiling — projection cache-hit was 1.5 µs/call; the
    // rotation cache-hit subtracts ~5-25 ns more (4 sin/cos avoided),
    // so the combined path should land at or below ~1.5 µs/call. If a
    // future refactor routes the cache-hit path through a slow call
    // sequence, this catches it.
    EXPECT_LT(per_call_us, 1.5);
    EXPECT_NE(t1, t0);
    EXPECT_FALSE(c.projection_dirty_);
    EXPECT_FALSE(c.view_rotation_dirty_);
}

// When pitch == 0 (looking horizontally, the most common case in
// Walking), the right vector from the cross product (forward × world_up)
// is already unit length: |right| = |cos(pitch)| = 1. The previous
// implementation always ran a sqrtf + divide to "normalise" a vector
// that was already normalised — pure waste. This probe pins the
// "skip the sqrt when pitch ≈ 0" optimisation:
//
//   1. For pitch = 0, the resulting right vector must still be unit
//      length (no regression from the skip).
//   2. For pitch = 0, the right vector must match the explicit formula
//      (cos(yaw), 0, sin(yaw)) — i.e. the skip didn't change the math.
//   3. The view matrix must still be orthonormal for pitch = 0 — i.e.
//      up = right × forward still holds and the basis is right-handed.
//
// The performance benefit is in the camera-state perf budget (the
// rotation cache miss is the hot path when the mouse moves); this
// probe pins the correctness side.
void TestCameraStateBuildViewRotationPitchZeroSkipsNormalisation() {
    // Case 1: yaw = 0, pitch = 0. Right should be (1, 0, 0).
    {
        Mat4 view{};
        CameraState_BuildViewRotation(view, 0.0f, 0.0f);
        EXPECT_NEAR(view[0][0], 1.0f, 1e-6f);
        EXPECT_NEAR(view[0][1], 0.0f, 1e-6f);
        EXPECT_NEAR(view[0][2], 0.0f, 1e-6f);
        EXPECT_NEAR(view[0][3], 0.0f, 1e-6f);
    }
    // Case 2: yaw = π/2, pitch = 0. Right should be (0, 0, 1).
    {
        Mat4 view{};
        constexpr float kHalfPi = 1.5707963267948966f;
        CameraState_BuildViewRotation(view, kHalfPi, 0.0f);
        EXPECT_NEAR(view[0][0], 0.0f, 1e-6f);
        EXPECT_NEAR(view[0][1], 0.0f, 1e-6f);
        EXPECT_NEAR(view[0][2], 1.0f, 1e-6f);
    }
    // Case 3: yaw = π/4, pitch = 0. Right should be (cos(π/4), 0, sin(π/4)).
    {
        Mat4 view{};
        constexpr float kQuarterPi = 0.7853981633974483f;
        const float expected = 0.7071067811865476f;  // sqrt(2)/2
        CameraState_BuildViewRotation(view, kQuarterPi, 0.0f);
        EXPECT_NEAR(view[0][0], expected, 1e-6f);
        EXPECT_NEAR(view[0][1], 0.0f, 1e-6f);
        EXPECT_NEAR(view[0][2], expected, 1e-6f);
    }
    // Case 4: the orthonormal basis must still hold for pitch = 0. We
    // check |right| = 1 (pinned in cases 1-3) and right·forward = 0
    // (perpendicular). Forward at yaw=0, pitch=0 is (0, 0, -1), so the
    // dot product with right = (1, 0, 0) is 0.
    {
        Mat4 view{};
        CameraState_BuildViewRotation(view, 0.0f, 0.0f);
        const double dot_rf = view[0][0] * view[2][0] +
                               view[0][1] * view[2][1] +
                               view[0][2] * view[2][2];
        EXPECT_NEAR(dot_rf, 0.0, 1e-6);
    }
    // Case 5: non-zero pitch still goes through the normalisation path
    // (the fabsf(sp) >= 1e-6f guard). The output must match the
    // pre-optimisation math within float precision.
    {
        Mat4 view{};
        constexpr float kQuarterPi = 0.7853981633974483f;
        const float yaw = 0.0f;
        const float pitch = kQuarterPi;
        CameraState_BuildViewRotation(view, yaw, pitch);
        // At yaw=0, pitch=π/4: forward = (0, sin(π/4), -cos(π/4)).
        // right = forward × world_up = (cos(π/4), 0, 0) / cos(π/4) = (1, 0, 0)
        // (the divide normalises by |cos(pitch)|).
        EXPECT_NEAR(view[0][0], 1.0f, 1e-6f);
        EXPECT_NEAR(view[0][1], 0.0f, 1e-6f);
        EXPECT_NEAR(view[0][2], 0.0f, 1e-6f);
    }
}

// Sin/cos precomputed cache contract for the rotation block. PhaseApplyMouseLook
// writes sin(yaw)/cos(yaw) once per RunOnce; PhaseIntegrateMovement reads
// the same values; CameraState_AdoptPlayer forwards them into
// camera_.precomputed_yaw_trig_* so BuildViewRotation's cache-miss path
// can skip its own sinf/cosf. This test pins:
//   1. The cached path produces the same view rotation matrix as the
//      plain BuildViewRotation (no cache) for representative yaw values.
//   2. The override path (precomputed_yaw_trig_valid_ = false) still
//      produces the correct matrix from BuildViewRotation.
//   3. CameraState_Reset clears the cache flag, so a stale cache never
//      leaks between cameras.
//   4. The cache claim is honoured by CameraState_Update when the
//      rotation block needs to be rebuilt.
void TestCameraStatePrecomputedYawTrigCacheContract() {
    // Case 1: cache-hit path matches the plain path across multiple
    // representative yaw values (the precomputed cache is the same
    // math the plain path computes internally).
    {
        CameraState cached{};
        CameraState plain{};
        CameraState_Reset(cached);
        CameraState_Reset(plain);
        cached.yaw_radians = 0.3f;
        cached.pitch_radians = -0.2f;
        plain.yaw_radians = 0.3f;
        plain.pitch_radians = -0.2f;
        // Simulate FrameLoop::RunOnce Phase 6 (AdoptPlayer path): the
        // cache fields hold the sin/cos of yaw_radians the workflow
        // already computed in PhaseApplyMouseLook.
        cached.precomputed_sin_yaw_ = sinf(cached.yaw_radians);
        cached.precomputed_cos_yaw_ = cosf(cached.yaw_radians);
        cached.precomputed_yaw_trig_valid_ = true;
        // plain path leaves precomputed_yaw_trig_valid_ false (the
        // default after Reset) and lets BuildViewRotation compute fresh.
        plain.precomputed_yaw_trig_valid_ = false;

        CameraState_Update(cached);
        CameraState_Update(plain);

        // Rotation block must match between the two paths within
        // float precision (the math is identical — the cached path
        // uses precomputed sin/cos, the plain path computes them).
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                EXPECT_NEAR(cached.view[row][col], plain.view[row][col], 1e-6f);
            }
        }
        // Translation row must also match (UpdateViewTranslation uses
        // the rotation block to compute row 3; same rotation block →
        // same row 3).
        for (int col = 0; col < 4; ++col) {
            EXPECT_NEAR(cached.view[3][col], plain.view[3][col], 1e-6f);
        }
    }

    // Case 2: at yaw=0 the precomputed sin is 0.0 (a value the
    // optimizer must NOT treat as "absent" — sin(0) is a real 0).
    // The flag-based check is the only correct gate.
    {
        CameraState c{};
        CameraState_Reset(c);
        c.yaw_radians = 0.0f;
        c.pitch_radians = 0.0f;
        c.precomputed_sin_yaw_ = 0.0f;
        c.precomputed_cos_yaw_ = 1.0f;
        c.precomputed_yaw_trig_valid_ = true;

        CameraState_Update(c);
        EXPECT_NEAR(c.view[0][0], 1.0f, 1e-6f);  // right=+X at yaw=0
        EXPECT_NEAR(c.view[2][2], -1.0f, 1e-6f);  // forward=-Z at yaw=0, pitch=0
    }

    // Case 3: CameraState_Reset clears the cache flag. This is the
    // contract that keeps the override path correct — the FSM builds
    // its camera override via CameraState_Reset, so the fresh
    // precomputed_yaw_trig_valid_ = false.
    {
        CameraState c{};
        c.precomputed_yaw_trig_valid_ = true;
        c.precomputed_sin_yaw_ = 0.7f;
        c.precomputed_cos_yaw_ = 0.3f;
        CameraState_Reset(c);
        EXPECT_FALSE(c.precomputed_yaw_trig_valid_);
    }

    // Case 4: when the cache is invalid, the rotation matrix must
    // still be correct (BuildViewRotation's plain path is the
    // fallback). Pin this with yaw = π/3, pitch = π/6 — the matrix
    // entries are exact and verifiable by hand:
    //   sy = sin(π/3) = √3/2 ≈ 0.8660254
    //   cy = cos(π/3) = 1/2 = 0.5
    //   sp = sin(π/6) = 1/2 = 0.5
    //   cp = cos(π/6) = √3/2 ≈ 0.8660254
    //   fx = sy*cp ≈ 0.75, fy = 0.5, fz = -cy*cp ≈ -0.4330127
    {
        CameraState c{};
        CameraState_Reset(c);
        c.yaw_radians = 1.0471975511965976f;   // π/3
        c.pitch_radians = 0.5235987755982988f; // π/6
        c.precomputed_yaw_trig_valid_ = false;
        CameraState_Update(c);
        EXPECT_NEAR(c.view[2][0], 0.75f, 1e-5f);              // fx = sy*cp
        EXPECT_NEAR(c.view[2][1], 0.5f, 1e-6f);                // fy = sp
        EXPECT_NEAR(c.view[2][2], -0.4330127018922193f, 1e-5f); // fz = -cy*cp
    }
}

// CameraState_Reset must clear the precomputed-yaw-trig cache. The
// FrameLoop's Phase 6 override path (`camera_ = camera_override_`)
// honours precomputed_yaw_trig_valid_ as the contract for "BuildViewRotation
// can skip its own sinf/cosf". When the override target is a freshly
// CameraState_Reset instance (the FSM's M5 transition tweens build a
// reset target and write the camera fields on top), the valid flag must
// start false so BuildViewRotation recomputes fresh — otherwise stale
// bytes in precomputed_sin_yaw_ / precomputed_cos_yaw_ would feed the
// rotation block and produce a wrong view matrix on the override frame.
//
// The FrameLoop constructor calls CameraState_Reset on camera_override_
// to enforce this contract at init time (no uninitialized CameraState
// fields). The test below pins that CameraState_Reset itself clears the
// flag and zero-out the sin/cos fields (defence-in-depth against a
// future bug that ignores the flag).
void TestCameraStateResetClearsPrecomputedYawTrigCache() {
    CameraState c{};
    // Pre-poison: set the cache flag and the sin/cos fields to
    // recognisable garbage. CameraState_Reset must overwrite every
    // field, not just the ones it explicitly initialises.
    c.precomputed_yaw_trig_valid_ = true;
    c.precomputed_sin_yaw_ = 1234.5f;
    c.precomputed_cos_yaw_ = -6789.0f;
    c.yaw_radians = 0.3f;
    c.pitch_radians = -0.2f;

    CameraState_Reset(c);

    // After Reset, BuildViewRotation must recompute (the flag is
    // cleared). Verify the flag is false and the values are zeroed —
    // the zeroing is a defence-in-depth that ensures even a future
    // bug that ignores the flag would produce a degenerate (zero)
    // rotation vector that BuildViewRotation's fabsf(sp) >= 1e-6f
    // guard handles, instead of feeding garbage into the matrix.
    EXPECT_FALSE(c.precomputed_yaw_trig_valid_);
    EXPECT_NEAR(c.precomputed_sin_yaw_, 0.0f, 1e-9f);
    EXPECT_NEAR(c.precomputed_cos_yaw_, 0.0f, 1e-9f);

    // Drive Update and confirm the rotation matrix is correct (not
    // poisoned by the pre-Reset garbage).
    CameraState_Update(c);
    EXPECT_NEAR(c.view[2][2], -1.0f, 1e-6f);  // forward.z at yaw=0, pitch=0
    EXPECT_NEAR(c.view[0][0], 1.0f, 1e-6f);   // right.x at yaw=0, pitch=0
}

}  // namespace walkercore