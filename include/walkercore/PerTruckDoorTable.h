// WalkerCore — per-truck door geometry overrides.
//
// M5 PARTIAL extension: extends TruckDoorConfigFromTruckState with a
// small built-in table mapping SCS `truck.placement.model_id` strings
// (e.g. "vehicle.volvo.vnl_2016") to non-generic door / cabin offsets.
//
// Why this table exists:
//   The generic placeholder offsets in TruckDoorConfig.h assume a US-
//   style daycab with the door just forward of the rear axle. Real ATS
//   trucks are diverse: cab-overs have the door in front of the cabin
//   anchor; high-roof sleepers raise the door pivot; long-nose
//   conventionals shift the door forward by ~0.5 m. The generic
//   fallback is "wrong but functional" for every truck; per-truck
//   entries bring the door pivot to where the player actually expects
//   to see it.
//
//   TMRealWalk ships the same idea as `walker.door_outward` (a single
//   scalar offset) plus a per-truck INI block. We follow the same
//   shape: a fixed model_id → override table, with a single scalar
//   door offset and a couple of per-truck cabin extents. Expanding
//   into a JSON / SII table is left to a follow-up commit; the
//   integration point (ApplyOverride below) is stable.
//
// Coordinate space:
//   Offsets are truck-local (X right, Y up, Z back) — the same space as
//   TruckState.cabin_position and head_position.

#ifndef WALKERCORE_PER_TRUCK_DOOR_TABLE_H
#define WALKERCORE_PER_TRUCK_DOOR_TABLE_H

#include <stddef.h>

#include "walkercore/WorldState.h"  // Vec3 lives here, used by the override

namespace walkercore {

// Forward declaration — TruckDoorConfig.h includes this header, so we
// cannot include it back. ApplyOverride takes TruckDoorConfig by
// reference; the .cpp side includes the full header.
struct TruckDoorConfig;

// One row of per-truck data. When model_id matches, Apply() patches
// the relevant fields of TruckDoorConfig. Any field that is left at
// its default sentinel (door_outward_m == 0.0) is treated as
// "use the generic Cabin-derived value" and is not patched.
struct TruckDoorConfigOverride {
    // Door pivot offset from the SCS cabin anchor. Default 0 means
    // "use the generic -0.45 m left, +0.55 m up, -0.40 m forward".
    // Non-zero overrides replace those values per-axis (x is signed
    // left/right; y is signed up; z is signed forward/back).
    float door_outward_x_m = 0.0f;
    float door_outward_y_m = 0.0f;
    float door_outward_z_m = 0.0f;

    // Cabin bounding box size (full extents, not half-extents). When
    // 0, the generic 2.2 × 1.65 × 2.1 m box is used. A high-roof
    // sleeper would set cabin_size_y_m = 1.95, for example.
    float cabin_size_x_m = 0.0f;
    float cabin_size_y_m = 0.0f;
    float cabin_size_z_m = 0.0f;
};

// Internal row entry — model_id + override. Defined at namespace
// scope so the constructor signature is well-formed (the private
// struct inside the class cannot be used in a public prototype).
struct TruckDoorConfigRow {
    const char *model_id;
    TruckDoorConfigOverride ov;
};

// Lookup table. Owned statically; the BuiltIn() singleton is the only
// instance the test harness creates. Lookup is O(N) but N is small
// (single-digit rows today, bounded by the ATS truck roster — we
// never expect more than ~50 distinct model_ids). When we eventually
// switch to a hash / binary search, the public Find() surface stays
// identical.
class PerTruckDoorTable {
public:
    // Construct an instance with a custom rows pointer. Production
    // code does not call this; the BuiltIn() singleton is the only
    // instance the runtime uses. Tests construct a temporary table
    // with their own rows + a known count to validate the lookup +
    // ApplyOverride logic deterministically.
    PerTruckDoorTable(const TruckDoorConfigRow *rows, size_t count)
        : rows_(rows), count_(count) {}

    // Built-in table populated with the most common ATS trucks from
    // the 1.61 series. Adding more rows is the canonical extension
    // path; the integration code does not need to change.
    static const PerTruckDoorTable &BuiltIn();

    // Find a row by exact model_id match. Returns nullptr when no row
    // matches. The pointer's lifetime equals the table's lifetime —
    // tests compare the pointer identity, never copy the contents.
    const TruckDoorConfigOverride *Find(const char *model_id) const;

    // Apply an override to a config. nullptr is a no-op (caller
    // intended to fall back to the generic geometry). This is the
    // single integration point a future JSON / SII loader hooks into.
    static void ApplyOverride(TruckDoorConfig &cfg,
                              const TruckDoorConfigOverride *ov,
                              const TruckDoorConfig &generic);

private:
    // kBuiltInRows_ + kBuiltInRowCount_ are defined inside the .cpp
    // so adding rows does not require recompiling dependent TUs.
    static const TruckDoorConfigRow kBuiltInRows_[];
    static const size_t kBuiltInRowCount_;

    // Pointer to the active row array. Production code uses BuiltIn();
    // tests inject a custom table by constructing an instance with a
    // specific rows pointer.
    const TruckDoorConfigRow *rows_;
    size_t count_;
};

}  // namespace walkercore

#endif  // WALKERCORE_PER_TRUCK_DOOR_TABLE_H
