// WalkerCore — per-truck door table implementation.
//
// The built-in table covers the most common ATS 1.61 trucks. To add a
// truck: append one Row to kBuiltInRows_ with the correct model_id
// (verbatim from SCS's truck.placement.id) and the non-default
// offsets that differ from the generic Cabin-derived placeholder.
//
// Negative door_outward_x_m means the door is on the driver's side
// (LHD markets = -x). Cabin size is full extents; the cabin centre
// remains the SCS cabin_position.

#include "walkercore/PerTruckDoorTable.h"

#include <string.h>

#include "walkercore/TruckDoorConfig.h"

namespace walkercore {

const TruckDoorConfigRow PerTruckDoorTable::kBuiltInRows_[] = {
    // Volvo VNL 2016 — long-nose conventional. Door pivot is ~0.55 m
    // forward of the cabin anchor (cabin position is the BACK of the
    // long nose). Cabin is wide; the height is a high-roof sleeper
    // at 1.95 m.
    {"vehicle.volvo.vnl_2016",
     {-0.50f, 0.60f, 0.05f,   /* door offsets (x, y, z) */
      2.30f, 1.95f, 2.40f}},   /* cabin size (x, y, z) */

    // Kenworth W900 — long-nose conventional. Driver door is similar
    // to VNL but the nose is shorter; door pivot sits ~0.40 m forward
    // of the cabin anchor. Sleeper cab on the W900L bumps cabin
    // height to 1.95 m. We ship the day-cab version here.
    {"vehicle.kenworth.w900",
     {-0.50f, 0.60f, -0.20f,
      2.30f, 1.80f, 2.20f}},

    // Peterbilt 579 — long-nose, similar to W900.
    {"vehicle.peterbilt.579",
     {-0.50f, 0.60f, -0.10f,
      2.30f, 1.85f, 2.30f}},

    // Freightliner Cascadia — long-nose, slightly shorter. Driver door
    // sits closer to the cabin centre.
    {"vehicle.freightliner.cascadia",
     {-0.50f, 0.60f, -0.20f,
      2.30f, 1.85f, 2.30f}},

    // Mack Anthem — long-nose, very long sleeper. Cabin height 1.95 m.
    {"vehicle.mack.anthem",
     {-0.50f, 0.60f, -0.20f,
      2.30f, 1.95f, 2.40f}},

    // Volvo VNL 2009 — older long-nose, more upright cabin. Door pivot
    // is slightly higher (0.65 m) than the 2016 model.
    {"vehicle.volvo.vnl",
     {-0.50f, 0.65f, -0.30f,
      2.30f, 1.80f, 2.30f}},

    // Peterbilt 389 — heritage long-hood. Driver door sits closer to
    // the cabin centre than the 579 because the 389's nose is wider.
    {"vehicle.peterbilt.389",
     {-0.50f, 0.55f, -0.20f,
      2.30f, 1.80f, 2.20f}},

    // Kenworth T680 — aerodynamic mid-roof. The T680's nose is shorter
    // than the W900 but its cabin is wider (sloped aero fenders).
    // Door pivot is closer to the cabin anchor.
    {"vehicle.kenworth.t680",
     {-0.50f, 0.60f, -0.05f,
      2.30f, 1.85f, 2.30f}},

    // Kenworth T880 — set-back front axle version of the T680. The
    // bumper sits further forward of the cabin anchor than the T680.
    {"vehicle.kenworth.t880",
     {-0.50f, 0.60f, 0.10f,
      2.30f, 1.85f, 2.30f}},

    // Freightliner Cascadia 2018 — the post-2018 refresh. The cabin is
    // slightly taller (1.90 m) and the door pivot is the same as the
    // pre-refresh Cascadia.
    {"vehicle.freightliner.cascadia_2018",
     {-0.50f, 0.60f, -0.20f,
      2.30f, 1.90f, 2.30f}},

    // Western Star 49X — modern aero long-hood. The 49X's nose is
    // longer than the W900 (set-forward axle). Door pivot sits
    // ~0.30 m forward of the cabin anchor.
    {"vehicle.western_star.49x",
     {-0.50f, 0.60f, 0.30f,
      2.30f, 1.85f, 2.40f}},

    // Western Star 4900 — older long-hood, similar to the W900 in
    // proportions. Set-forward axle but smaller sleeper.
    {"vehicle.western_star.4900",
     {-0.50f, 0.60f, 0.20f,
      2.30f, 1.80f, 2.20f}},

    // International LT — aero mid-roof. Cabin is wider than tall
    // because the LT's sleeper is shorter than the LoneStar's.
    {"vehicle.international.lt",
     {-0.50f, 0.55f, -0.10f,
      2.30f, 1.80f, 2.30f}},

    // International LoneStar — classic long-hood with a tall,
    // distinctive sleeper. Cabin height 1.95 m and longer wheelbase.
    {"vehicle.international.lone_star",
     {-0.50f, 0.65f, 0.15f,
      2.30f, 1.95f, 2.40f}},
};

const size_t PerTruckDoorTable::kBuiltInRowCount_ =
    sizeof(kBuiltInRows_) / sizeof(kBuiltInRows_[0]);

const PerTruckDoorTable &PerTruckDoorTable::BuiltIn() {
    static const PerTruckDoorTable instance{kBuiltInRows_, kBuiltInRowCount_};
    return instance;
}

const TruckDoorConfigOverride *PerTruckDoorTable::Find(
    const char *model_id) const {
    if (model_id == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < count_; ++i) {
        const TruckDoorConfigRow &r = rows_[i];
        if (r.model_id == nullptr) {
            continue;
        }
        if (strcmp(r.model_id, model_id) == 0) {
            return &r.ov;
        }
    }
    return nullptr;
}

void PerTruckDoorTable::ApplyOverride(
    TruckDoorConfig &cfg,
    const TruckDoorConfigOverride *ov,
    const TruckDoorConfig &generic) {
    if (ov == nullptr) {
        (void)cfg;
        (void)generic;
        return;
    }
    // The driver-door position is derived from the SCS cabin anchor
    // in TruckDoorConfigFromTruckState; if the per-truck row says
    // the door is in a different position, the generic offset
    // (-0.45, +0.55, -0.40) was already applied, so we add the
    // per-truck delta on top of it. The per-truck data is expressed
    // in the same "delta from cabin" space.
    //
    // We replace the generic offsets when the row's offsets are not
    // exactly zero; this matches the (0, 0, 0) sentinel meaning
    // "use generic". The translation from "delta from cabin anchor"
    // to "absolute door pivot" was done by the generic path.
    //
    // The cleanest expression: re-anchor `door position` to
    // `cabin_position + ov->{x,y,z}` when any axis is non-zero. We
    // detect that with a single boolean per axis below; the decision
    // matches TMRealWalk's `walker.door_outward` knob.
    const bool x_set = ov->door_outward_x_m != 0.0f;
    const bool y_set = ov->door_outward_y_m != 0.0f;
    const bool z_set = ov->door_outward_z_m != 0.0f;
    if (x_set || y_set || z_set) {
        // Per-axis delta in metres between the per-truck offset and
        // the generic placeholder (-0.45, +0.55, -0.40). Compute in
        // double (Vec3 fields are doubles) so the additions compile
        // cleanly under -Wdouble-promotion.
        const double dx =
            (static_cast<double>(ov->door_outward_x_m) - (-0.45)) *
            (x_set ? 1.0 : 0.0);
        const double dy =
            (static_cast<double>(ov->door_outward_y_m) - 0.55) *
            (y_set ? 1.0 : 0.0);
        const double dz =
            (static_cast<double>(ov->door_outward_z_m) - (-0.40)) *
            (z_set ? 1.0 : 0.0);
        cfg.driver_door.local_position = Vec3{
            cfg.driver_door.local_position.x + dx,
            cfg.driver_door.local_position.y + dy,
            cfg.driver_door.local_position.z + dz,
        };
    }

    // Cabin box: replace when any axis is non-zero. The generic centre
    // sits at cabin_position; the new extents replace the defaults.
    // Note: cfg.cabin_* and generic.cabin_* are Vec3 doubles, but the
    // override fields are floats. We keep the computation in float
    // (SSE-friendly for the hot path the override fires only on
    // configuration events, not per-frame) and assign the result back
    // as doubles. The conversion is exact for the small integer values
    // we ship in the table.
    const bool cx_set = ov->cabin_size_x_m != 0.0f;
    const bool cy_set = ov->cabin_size_y_m != 0.0f;
    const bool cz_set = ov->cabin_size_z_m != 0.0f;
    if (cx_set || cy_set || cz_set) {
        // Compute everything in double (Vec3 fields are doubles; the
        // table fields are float, but the conversion is exact for the
        // small integer values we ship).
        const double cx = cx_set
            ? static_cast<double>(ov->cabin_size_x_m)
            : (generic.cabin_max.x - generic.cabin_min.x);
        const double cy = cy_set
            ? static_cast<double>(ov->cabin_size_y_m)
            : (generic.cabin_max.y - generic.cabin_min.y);
        const double cz = cz_set
            ? static_cast<double>(ov->cabin_size_z_m)
            : (generic.cabin_max.z - generic.cabin_min.z);
        const double cen_x = (cfg.cabin_max.x + cfg.cabin_min.x) * 0.5;
        const double cen_y = (cfg.cabin_max.y + cfg.cabin_min.y) * 0.5;
        const double cen_z = (cfg.cabin_max.z + cfg.cabin_min.z) * 0.5;
        cfg.cabin_min = Vec3{cen_x - cx * 0.5, cen_y - cy * 0.5,
                             cen_z - cz * 0.5};
        cfg.cabin_max = Vec3{cen_x + cx * 0.5, cen_y + cy * 0.5,
                             cen_z + cz * 0.5};
    }
}

}  // namespace walkercore
