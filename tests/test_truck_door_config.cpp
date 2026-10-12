// WalkerCore — TruckDoorConfig tests.
//
// Verifies the truck-local <-> world helpers roundtrip correctly.

#include "TestHarness.h"
#include "walkercore/TruckDoorConfig.h"

#include <math.h>

namespace walkercore {

void TestTruckLocalRoundtrip() {
    const double heading = 1.2;  // radians
    const Vec3 local = Vec3{-0.95, 0.55, -1.20};
    const Vec3 world = TruckLocalToWorld(local, heading);
    const Vec3 back = WorldToTruckLocal(world, heading);
    EXPECT_NEAR(back.x, local.x, 1e-6);
    EXPECT_NEAR(back.y, local.y, 1e-6);
    EXPECT_NEAR(back.z, local.z, 1e-6);
}

void TestTruckLocalWorldIdentity() {
    // Heading = 0 (truck facing +Z).
    const Vec3 local = Vec3{1.0, 2.0, 3.0};
    const Vec3 world = TruckLocalToWorld(local, 0.0);
    // Rotation by 0 is identity.
    EXPECT_NEAR(world.x, 1.0, 1e-6);
    EXPECT_NEAR(world.y, 2.0, 1e-6);
    EXPECT_NEAR(world.z, 3.0, 1e-6);
}

// -----------------------------------------------------------------------------
// TruckLocalToWorld / WorldToTruckLocal field-exhaustive probe.
//
// Both helpers have only one knob: heading_radians. Y is always
// preserved (rotation around the Y axis only — roll/pitch are ignored
// for door geometry). Rotation is right-handed around the +Y axis: a
// point at local +X rotates toward world -Z as heading increases.
//
// Tests pin the rotation behaviour at several canonical headings and
// the round-trip identity at each, so a future refactor that flips
// the rotation direction or breaks the inverse fails with a clear
// assertion.
// -----------------------------------------------------------------------------

void TestTruckLocalTransformEveryFieldHasObservableEffect() {
    // ---- Case 1: heading = π/2 — X axis rotates to -Z, Z to +X ----
    {
        const double h = 1.5707963267948966;  // π/2
        const Vec3 origin = Vec3{0.0, 0.0, 0.0};
        const Vec3 px = Vec3{1.0, 0.0, 0.0};
        const Vec3 pz = Vec3{0.0, 0.0, 1.0};

        // Origin always maps to origin.
        const Vec3 w_origin = TruckLocalToWorld(origin, h);
        EXPECT_NEAR(w_origin.x, 0.0, 1e-6);
        EXPECT_NEAR(w_origin.y, 0.0, 1e-6);
        EXPECT_NEAR(w_origin.z, 0.0, 1e-6);

        // +X → -Z (right-hand rule around +Y).
        const Vec3 w_px = TruckLocalToWorld(px, h);
        EXPECT_NEAR(w_px.x, 0.0, 1e-6);
        EXPECT_NEAR(w_px.y, 0.0, 1e-6);
        EXPECT_NEAR(w_px.z, -1.0, 1e-6);

        // +Z → +X.
        const Vec3 w_pz = TruckLocalToWorld(pz, h);
        EXPECT_NEAR(w_pz.x, 1.0, 1e-6);
        EXPECT_NEAR(w_pz.y, 0.0, 1e-6);
        EXPECT_NEAR(w_pz.z, 0.0, 1e-6);

        // Y preserved regardless of heading.
        const Vec3 py = Vec3{0.0, 5.0, 0.0};
        const Vec3 w_py = TruckLocalToWorld(py, h);
        EXPECT_NEAR(w_py.y, 5.0, 1e-6);
    }

    // ---- Case 2: heading = π — 180° rotation (X→-X, Z→-Z) ----
    {
        const double h = 3.14159265358979323846;  // π
        const Vec3 px = Vec3{2.0, 1.5, -3.0};
        const Vec3 w_px = TruckLocalToWorld(px, h);
        EXPECT_NEAR(w_px.x, -2.0, 1e-6);
        EXPECT_NEAR(w_px.y, 1.5, 1e-6);  // Y preserved
        EXPECT_NEAR(w_px.z, 3.0, 1e-6);

        // Roundtrip must recover the original.
        const Vec3 back = WorldToTruckLocal(w_px, h);
        EXPECT_NEAR(back.x, px.x, 1e-6);
        EXPECT_NEAR(back.y, px.y, 1e-6);
        EXPECT_NEAR(back.z, px.z, 1e-6);
    }

    // ---- Case 3: heading = -π/2 — opposite of Case 1 (X→+Z, Z→-X) ----
    {
        const double h = -1.5707963267948966;  // -π/2
        const Vec3 px = Vec3{1.0, 0.0, 0.0};
        const Vec3 pz = Vec3{0.0, 0.0, 1.0};
        const Vec3 w_px = TruckLocalToWorld(px, h);
        EXPECT_NEAR(w_px.x, 0.0, 1e-6);
        EXPECT_NEAR(w_px.z, 1.0, 1e-6);  // +X → +Z (opposite of π/2)
        const Vec3 w_pz = TruckLocalToWorld(pz, h);
        EXPECT_NEAR(w_pz.x, -1.0, 1e-6);
        EXPECT_NEAR(w_pz.z, 0.0, 1e-6);  // +Z → -X
    }

    // ---- Case 4: local.y is preserved under any heading ----
    {
        // Use the default driver-door anchor Y of 0.55 plus a high Y
        // (e.g. 10 m up) so we can detect any spurious Y contamination
        // from the rotation.
        const Vec3 local = Vec3{-1.4, 10.0, -1.2};
        const double headings[] = {0.0, 0.5, 1.0, 1.5708, 3.0,
                                    -0.5, -1.5708, -3.14159};
        for (double h : headings) {
            const Vec3 w = TruckLocalToWorld(local, h);
            EXPECT_NEAR(w.y, local.y, 1e-6);
        }
    }

    // ---- Case 5: roundtrip identity at multiple headings ----
    {
        const Vec3 locals[] = {
            Vec3{-0.95, 0.55, -1.20},   // default driver door trigger
            Vec3{-1.40, 0.0,  -1.20},   // default driver door enter anchor
            Vec3{-0.45, 0.55, -0.50},   // default driver seat
            Vec3{1.0, 2.0, 3.0},
            Vec3{0.0, 0.0, 0.0},
            Vec3{100.0, -50.0, 200.0},
        };
        const double headings[] = {0.0, 0.25, 1.0, 1.5708, 3.14159,
                                    -0.5, -1.5708};
        for (const Vec3 &loc : locals) {
            for (double h : headings) {
                const Vec3 world = TruckLocalToWorld(loc, h);
                const Vec3 back = WorldToTruckLocal(world, h);
                EXPECT_NEAR(back.x, loc.x, 1e-6);
                EXPECT_NEAR(back.y, loc.y, 1e-6);
                EXPECT_NEAR(back.z, loc.z, 1e-6);
            }
        }
    }

    // ---- Case 6: different headings produce different world positions ----
    // (Negative control for the inverse function: two headings that
    // differ must map the same local point to distinct world points.)
    {
        const Vec3 local = Vec3{1.0, 0.0, 0.0};
        const Vec3 w0   = TruckLocalToWorld(local, 0.0);
        const Vec3 wPi4 = TruckLocalToWorld(local, 0.7854);  // π/4
        const Vec3 wPi2 = TruckLocalToWorld(local, 1.5708);  // π/2
        EXPECT_TRUE(w0.x != wPi4.x || w0.z != wPi4.z);
        EXPECT_TRUE(w0.x != wPi2.x || w0.z != wPi2.z);
        EXPECT_TRUE(wPi4.x != wPi2.x || wPi4.z != wPi2.z);
    }

    // ---- Case 7: WorldToTruckLocal inverts TruckLocalToWorld at h=0 ----
    {
        // For heading=0, both functions are the identity. Verify both
        // directions agree.
        const Vec3 v = Vec3{7.0, -3.0, 11.0};
        EXPECT_NEAR(WorldToTruckLocal(v, 0.0).x, v.x, 1e-6);
        EXPECT_NEAR(WorldToTruckLocal(v, 0.0).y, v.y, 1e-6);
        EXPECT_NEAR(WorldToTruckLocal(v, 0.0).z, v.z, 1e-6);
    }
}

}  // namespace walkercore